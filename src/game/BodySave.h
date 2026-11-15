#pragma once

#include "StageRules.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// A companion's new body in a game that mirrors her quest (Curie's 300; QuestSync's NewBody) takes a few seconds: her
// items moved out, then her actor reset, then, a second later, her outfit, affinity and factions put back. A save made
// inside those seconds held her as it found her (a robot with no items and her 300 done, or a synth not yet dressed),
// and a load dropped what was left of the change for good. So a save made while a change is under way notes it in its
// co-save (the 'BODY' record: Encode), and the game that loads that save carries the change on from the step the save
// was made in (Decode, Resolved, QuestSync's ResumeNewBody). Each step is safe to make twice (the removal moves what
// she still holds, a second reset builds the same body, the dressing sets what it set), so the step the record names
// is made again: its console command may not have landed before the save was written. A save without the record (any
// made by an older build, or with no change under way) loads as it always did. Only plugins' forms are written: a
// form id made at runtime names another form after a load. Pure, so it can be tested without the game.
namespace BodySave
{
	constexpr std::uint16_t RECORD_VERSION = 1;
	constexpr std::size_t   MAX_FACTIONS = 64;  // faction changes kept with one (a guard: Curie has a handful)

	// The step the change was in as the save was made: the last one whose commands were sent.
	enum class Phase : std::uint8_t
	{
		kRemoval = 0,  // her items' removal sent, the reset still to come
		kReset = 1,    // the reset sent, the dressing still to come
		kDressed = 2,  // the dressing sent, within KEPT_AFTER of play: it may not have landed in the save
	};

	// How long the change is still noted in saves after its dressing was sent, counted in play (StageRules::PlayedIn).
	inline constexpr auto KEPT_AFTER = std::chrono::seconds(2);

	struct Pending
	{
		std::uint32_t                        ref = 0;          // the companion
		std::uint32_t                        items = 0;        // the body that takes her items
		std::uint32_t                        destination = 0;  // where the reset moves her (0: in place)
		std::uint32_t                        outfit = 0;       // 0: none set
		Phase                                phase = Phase::kRemoval;
		std::optional<float>                 affinity;  // as read before the reset; none if it couldn't be
		std::vector<StageRules::FactionRank> factions;  // her faction changes as read before the reset

		bool operator==(const Pending&) const = default;
	};

	// A plugin's form: the same id in every run of the save (F4SE resolves it for a changed load order).
	constexpr bool Plugin(std::uint32_t a_id)
	{
		return a_id != 0 && (a_id >> 24) != 0xFF;
	}

	constexpr std::size_t HEADER = 2 + 1 + 1 + 4 * 4 + 4 + 2;  // version, phase, flags, four ids, affinity, count
	constexpr std::size_t PER_FACTION = 4 + 1;
	constexpr std::size_t MAX_LENGTH = HEADER + MAX_FACTIONS * PER_FACTION;
	// Version 1's layout is these sizes, written as the fields are (tests/BodySaveTests.cpp holds a record's bytes): a
	// field made wider would change every record written since, so it is a new version's, read beside this one.
	static_assert(sizeof(RECORD_VERSION) == 2 && sizeof(Phase) == 1 && sizeof(float) == 4);
	static_assert(sizeof(StageRules::FactionRank::faction) == 4 && sizeof(StageRules::FactionRank::rank) == 1);

	// The record's bytes: the version, the phase, whether the affinity was read, the companion, the body for her items,
	// the destination, the outfit, the affinity, then each faction change (its faction and rank). Nothing when the
	// companion or (with the removal still to be made again) the body for her items is no plugin's form.
	inline std::vector<std::uint8_t> Encode(const Pending& a_pending)
	{
		std::vector<std::uint8_t> out;
		if (!Plugin(a_pending.ref) || (a_pending.phase == Phase::kRemoval && !Plugin(a_pending.items))) {
			return out;
		}
		const auto put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		const auto plugin = [](std::uint32_t a_id) { return Plugin(a_id) ? a_id : 0u; };
		std::vector<StageRules::FactionRank> factions;
		for (const auto& faction : a_pending.factions) {
			if (Plugin(faction.faction) && factions.size() < MAX_FACTIONS) {
				factions.push_back(faction);
			}
		}
		put(RECORD_VERSION);
		put(static_cast<std::uint8_t>(a_pending.phase));
		put(static_cast<std::uint8_t>(a_pending.affinity ? 1 : 0));
		put(a_pending.ref);
		put(plugin(a_pending.items));
		put(plugin(a_pending.destination));
		put(plugin(a_pending.outfit));
		put(a_pending.affinity.value_or(0.0f));
		put(static_cast<std::uint16_t>(factions.size()));
		for (const auto& faction : factions) {
			put(faction.faction);
			put(faction.rank);
		}
		return out;
	}

	// A save's record (its ids not yet resolved for the load order); nothing if it can't be read: another version, a
	// phase not known, a count past MAX_FACTIONS, a length that is not the count's, a companion or a faction that is no
	// plugin's form, no body for her items with the removal still to be made again.
	inline std::optional<Pending> Decode(std::span<const std::uint8_t> a_bytes)
	{
		if (a_bytes.size() < HEADER) {
			return std::nullopt;
		}
		std::size_t at = 0;
		const auto  get = [&](auto& a_value) {
			std::memcpy(&a_value, a_bytes.data() + at, sizeof(a_value));
			at += sizeof(a_value);
		};
		std::uint16_t version = 0;
		std::uint8_t  phase = 0;
		std::uint8_t  flags = 0;
		float         affinity = 0.0f;
		std::uint16_t count = 0;
		Pending       out;
		get(version);
		get(phase);
		get(flags);
		get(out.ref);
		get(out.items);
		get(out.destination);
		get(out.outfit);
		get(affinity);
		get(count);
		if (version != RECORD_VERSION || phase > static_cast<std::uint8_t>(Phase::kDressed) || flags > 1 || count > MAX_FACTIONS ||
			a_bytes.size() != HEADER + std::size_t{ count } * PER_FACTION || !Plugin(out.ref)) {
			return std::nullopt;
		}
		out.phase = static_cast<Phase>(phase);
		if (out.phase == Phase::kRemoval && !Plugin(out.items)) {
			return std::nullopt;
		}
		if (flags & 1) {
			out.affinity = affinity;
		}
		out.factions.resize(count);
		for (auto& faction : out.factions) {
			get(faction.faction);
			get(faction.rank);
			if (!Plugin(faction.faction)) {
				return std::nullopt;
			}
		}
		return out;
	}

	// The record with each id as the loaded game has it (a_resolve: F4SE's ResolveFormID, nothing for a plugin no longer
	// loaded). Nothing without the companion, or without the body for her items when the removal is to be made again;
	// a destination or an outfit that is gone is none (the reset is made in place, no outfit set); a faction that is
	// gone is left out.
	template <class Resolve>
	std::optional<Pending> Resolved(const Pending& a_pending, Resolve&& a_resolve)
	{
		const auto now = [&](std::uint32_t a_id) -> std::uint32_t {
			if (!a_id) {
				return 0;
			}
			const std::optional<std::uint32_t> resolved = a_resolve(a_id);
			return resolved && Plugin(*resolved) ? *resolved : 0u;
		};
		Pending out = a_pending;
		out.ref = now(a_pending.ref);
		out.items = now(a_pending.items);
		out.destination = now(a_pending.destination);
		out.outfit = now(a_pending.outfit);
		if (!out.ref || (out.phase == Phase::kRemoval && !out.items)) {
			return std::nullopt;
		}
		out.factions.clear();
		for (const auto& faction : a_pending.factions) {
			if (const auto id = now(faction.faction)) {
				out.factions.push_back({ id, faction.rank });
			}
		}
		return out;
	}

	// The change between the main thread and the save's or load's thread: what a save made now would note (Set, by the
	// main thread as the change moves on; Save, on the save's thread), and a loaded save's record (Revert and Load on the
	// load's thread, while frames may still run; Take, once the game says it loaded).
	class Shared
	{
	public:
		void Set(std::optional<Pending> a_now)
		{
			std::scoped_lock l{ lock };
			now = std::move(a_now);
		}

		[[nodiscard]] std::vector<std::uint8_t> Save()
		{
			std::scoped_lock l{ lock };
			return now ? Encode(*now) : std::vector<std::uint8_t>{};
		}

		// The co-save is reverted (before a load, or for a new game): no record, until one is read.
		void Revert()
		{
			std::scoped_lock l{ lock };
			loaded.reset();
		}

		void Load(std::optional<Pending> a_loaded)
		{
			std::scoped_lock l{ lock };
			loaded = std::move(a_loaded);
		}

		// What the loaded save's record held, once.
		[[nodiscard]] std::optional<Pending> Take()
		{
			std::scoped_lock l{ lock };
			return std::exchange(loaded, std::nullopt);
		}

	private:
		std::mutex             lock;
		std::optional<Pending> now;
		std::optional<Pending> loaded;
	};
}
