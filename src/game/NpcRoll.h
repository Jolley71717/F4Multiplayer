#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// What an NPC is in the game that runs it, made the same in the others' (Protocol::NpcIdentity).
//
// A leveled NPC is put together by each game on its own the first time it loads: its leveled list is rolled at that
// game's player's level, and so is its chance of being legendary (the NPC's legendary chance and template). The engine
// keeps the roll on the reference (ExtraLeveledCreature): the NPC it was placed as, and one NPC per template slot
// (traits, stats, factions, spells and the rest). Two games had two rolls: the host a legendary bloatfly, the joiner a
// plain one, both under the same reference. Now the game that runs it reports its roll, and each other game whose NPC
// was placed as the same one and can come out that way (the roll's NPCs are among what its lists can give) makes its
// own copy of the NPC that roll, at the owner's level.
//
// A spawn (an NPC a game made at runtime, SpawnBook.h) is placed by the others as a copy, so its factions are theirs
// to set too: a random encounter's scavengers and a settlement's settlers get theirs from their quest at runtime, and a
// copy placed from one of its templates had that NPC's own factions only (a settler in one game was a hostile
// scavenger in the other). A plugin's NPC keeps its own factions: they are its game's story (a recruited settler, a
// faction's enemy), written into its saves.
//
// Pure, so it can be tested without the game: tests/NpcRollTests.cpp.
namespace NpcRoll
{
	using Protocol::FactionRank;
	using Templates = std::array<std::uint32_t, Protocol::IDENTITY_TEMPLATES>;

	// Factions never set from another game: our companion's mark (NpcSync::IsOurCompanion: two players can have the same
	// companion) and the player's own (SaveStrays: the stand-ins' mark, and it would make the copy one of ours).
	inline constexpr std::uint32_t CURRENT_COMPANION_FACTION = 0x00023C01;
	inline constexpr std::uint32_t PLAYER_FACTION = 0x0001C21C;

	[[nodiscard]] constexpr bool Shared(std::uint32_t a_faction)
	{
		return a_faction != 0 && a_faction != CURRENT_COMPANION_FACTION && a_faction != PLAYER_FACTION;
	}

	// What an actor is in: its base's factions (a rank below 0 is none), with its reference's changes over them (a rank
	// below 0 takes it out). Only the shared ones, by form ID.
	inline std::vector<FactionRank> Effective(std::span<const FactionRank> a_base, std::span<const FactionRank> a_changes)
	{
		std::vector<FactionRank> out;
		const auto set = [&](const FactionRank& a_entry) {
			const auto it = std::ranges::find(out, a_entry.faction, &FactionRank::faction);
			if (a_entry.rank < 0) {
				if (it != out.end()) {
					out.erase(it);
				}
			} else if (it != out.end()) {
				it->rank = a_entry.rank;
			} else {
				out.push_back(a_entry);
			}
		};
		for (const auto& entry : a_base) {
			if (entry.rank >= 0 && Shared(entry.faction)) {
				set(entry);
			}
		}
		for (const auto& entry : a_changes) {
			if (Shared(entry.faction)) {
				set(entry);
			}
		}
		std::ranges::sort(out, {}, &FactionRank::faction);
		return out;
	}

	// The factions in a report: the first MAX_IDENTITY_FACTIONS, and whether there were more.
	inline void SetFactions(Protocol::NpcIdentity& a_identity, std::vector<FactionRank> a_factions)
	{
		a_identity.flags |= Protocol::NpcIdentity::kFactions;
		if (a_factions.size() > Protocol::MAX_IDENTITY_FACTIONS) {
			a_factions.resize(Protocol::MAX_IDENTITY_FACTIONS);
			a_identity.flags |= Protocol::NpcIdentity::kFactionsCut;
		}
		a_identity.factions = std::move(a_factions);
	}

	// The slots a roll filled.
	[[nodiscard]] constexpr std::uint16_t MaskOf(const Templates& a_templates)
	{
		std::uint16_t mask = 0;
		for (std::size_t i = 0; i < a_templates.size(); ++i) {
			if (a_templates[i] != 0) {
				mask = static_cast<std::uint16_t>(mask | (1u << i));
			}
		}
		return mask;
	}

	// ---- What a leveled NPC can come out as ------------------------------------------------------------------------

	// The game's forms as Walk sees them: an NPC or a leveled list (anything else ends there), and where it leads. An
	// NPC leads to its default template, its slots' own templates and its legendary template (legendary: true); a
	// leveled list to its entries.
	struct Edge
	{
		std::uint32_t id = 0;
		bool          legendary = false;
	};

	struct Node
	{
		bool              npc = false;
		std::vector<Edge> next;
	};

	// The NPCs a roll of one can give: by its legendary chance (through a legendary template) or not.
	struct Reach
	{
		std::unordered_set<std::uint32_t> normal;
		std::unordered_set<std::uint32_t> legendary;
		bool                              cut = false;  // the walk stopped at MAX_WALK: something may be missing

		[[nodiscard]] bool Has(std::uint32_t a_id) const { return normal.contains(a_id) || legendary.contains(a_id); }
	};

	// Forms looked at at most (vanilla's longest lists, nested, are a few dozen).
	inline constexpr std::size_t MAX_WALK = 1024;

	// Everything a_original can be rolled as. a_look(id): the Node, or nothing for a form that isn't there.
	template <class Look>
	Reach Walk(std::uint32_t a_original, Look a_look)
	{
		Reach                                                    reach;
		std::deque<std::pair<std::uint32_t, bool>>               queue{ { a_original, false } };
		std::unordered_set<std::uint64_t>                        seen;
		const auto key = [](std::uint32_t a_id, bool a_legendary) { return (static_cast<std::uint64_t>(a_id) << 1) | (a_legendary ? 1u : 0u); };
		while (!queue.empty()) {
			const auto [id, legendary] = queue.front();
			queue.pop_front();
			if (id == 0 || !seen.insert(key(id, legendary)).second) {
				continue;
			}
			if (seen.size() > MAX_WALK) {
				reach.cut = true;
				break;
			}
			const std::optional<Node> node = a_look(id);
			if (!node) {
				continue;
			}
			if (node->npc) {
				(legendary ? reach.legendary : reach.normal).insert(id);
			}
			for (const auto& edge : node->next) {
				queue.emplace_back(edge.id, legendary || edge.legendary);
			}
		}
		return reach;
	}

	// A roll is legendary when one of its NPCs only a legendary template gives.
	[[nodiscard]] inline bool Legendary(const Templates& a_templates, std::uint16_t a_mask, const Reach& a_reach)
	{
		for (std::size_t i = 0; i < a_templates.size(); ++i) {
			if ((a_mask & (1u << i)) && a_reach.legendary.contains(a_templates[i]) && !a_reach.normal.contains(a_templates[i])) {
				return true;
			}
		}
		return false;
	}

	// ---- Making ours the owner's ---------------------------------------------------------------------------------

	// Our NPC under the same id, as our game has it.
	struct Ours
	{
		bool                     leveled = false;   // its own base is the roll's copy (a runtime NPC with leveled data): it can be rolled again
		std::uint32_t            originalBase = 0;  // what it was placed as
		Templates                templates{};
		std::uint16_t            level = 0;
		bool                     copy = false;      // our copy of a friend's spawn: its factions are ours to set
		std::vector<FactionRank> factions;          // Effective
	};

	enum class Roll
	{
		kSame,           // ours is their roll already (or theirs has none to give: it isn't leveled there)
		kMake,           // ours is made their roll
		kNotLeveled,     // ours isn't leveled here: its base is a plugin's NPC, every game's own
		kOtherOriginal,  // ours was placed as another NPC (a mod changed the reference here): not the same roll to make
		kNotWhole,       // one of their roll's NPCs, or the NPC it was placed as, has no ID here (a mod we lack)
		kNotOurs,        // one of their roll's NPCs is nothing our lists can give: never made here
	};

	struct Plan
	{
		Roll                       roll = Roll::kSame;
		bool                       templates = false;  // make ours their roll
		std::optional<std::uint16_t> level;            // and give it their level
		std::vector<FactionRank>   add;                // factions to put ours in (or at another rank)
		std::vector<std::uint32_t> remove;             // and to take it out of

		[[nodiscard]] bool Any() const { return templates || level || !add.empty() || !remove.empty(); }
	};

	// What to do to ours so it is what their report says. a_reachable(id): one of the NPCs our lists can give ours (Walk
	// from our original's Reach); asked only once the roll is the same NPC's.
	template <class Reachable>
	Plan Decide(const Ours& a_ours, const Protocol::NpcIdentity& a_theirs, Reachable a_reachable)
	{
		Plan plan;
		if (a_theirs.mask == 0) {
			plan.roll = Roll::kSame;
		} else if (!a_ours.leveled) {
			plan.roll = Roll::kNotLeveled;
		} else if (a_theirs.originalBase == 0) {
			plan.roll = Roll::kNotWhole;
		} else if (a_theirs.originalBase != a_ours.originalBase) {
			plan.roll = Roll::kOtherOriginal;
		} else {
			bool whole = true, ours = true, same = true;
			for (std::size_t i = 0; i < Protocol::IDENTITY_TEMPLATES; ++i) {
				const bool filled = (a_theirs.mask & (1u << i)) != 0;
				const auto theirs = filled ? a_theirs.templates[i] : 0u;
				if (filled && theirs == 0) {
					whole = false;
				} else if (filled && !a_reachable(theirs)) {
					ours = false;
				}
				same = same && theirs == a_ours.templates[i];
			}
			if (!whole) {
				plan.roll = Roll::kNotWhole;
			} else if (!ours) {
				plan.roll = Roll::kNotOurs;
			} else {
				plan.templates = !same;
				if (a_theirs.level >= 1 && a_theirs.level <= Protocol::MAX_IDENTITY_LEVEL && a_theirs.level != a_ours.level) {
					plan.level = a_theirs.level;
				}
				plan.roll = plan.templates || plan.level ? Roll::kMake : Roll::kSame;
			}
		}
		if (a_ours.copy && (a_theirs.flags & Protocol::NpcIdentity::kFactions)) {
			for (const auto& entry : a_theirs.factions) {
				if (!Shared(entry.faction) || entry.rank < 0) {
					continue;
				}
				const auto it = std::ranges::find(a_ours.factions, entry.faction, &FactionRank::faction);
				if (it == a_ours.factions.end() || it->rank != entry.rank) {
					plan.add.push_back(entry);
				}
			}
			// A cut list doesn't say what else theirs is in: nothing is taken away then.
			if (!(a_theirs.flags & Protocol::NpcIdentity::kFactionsCut)) {
				for (const auto& entry : a_ours.factions) {
					if (Shared(entry.faction) && std::ranges::find(a_theirs.factions, entry.faction, &FactionRank::faction) == a_theirs.factions.end()) {
						plan.remove.push_back(entry.faction);
					}
				}
			}
		}
		return plan;
	}

	// What Rolls::Apply does to make ours a plan's roll, by whether the actor's 3D is in (a_loaded). The roll is written on
	// the reference (the engine puts the NPC together from it at the actor's next load), and the level into its NPC (a
	// number the engine reads when asked). Our copy of the NPC is put together from the roll only with no 3D in: on a
	// loaded actor, one that may be in a fight, that and a rebuild of its 3D risk the game, and on the rig neither changed
	// an NPC already built. Nothing rebuilds the 3D: a roll written while it is in is put together at its next load.
	struct Steps
	{
		bool write = false;  // the roll on the reference
		bool build = false;  // our copy of the NPC put together from it now
		bool level = false;  // their level on it
	};

	[[nodiscard]] constexpr Steps StepsFor(const Plan& a_plan, bool a_loaded)
	{
		return { .write = a_plan.templates, .build = a_plan.templates && !a_loaded, .level = a_plan.level.has_value() };
	}

	// What our copy of a friend's spawn is placed from: the NPC their game placed it as when that is a leveled NPC here
	// too (our engine rolls it, and their roll is then made ours), else the spawn's own base (SpawnBook::PickBase).
	[[nodiscard]] constexpr std::uint32_t PlaceFrom(const Protocol::NpcIdentity* a_identity, std::uint32_t a_base, bool a_originalLeveledHere)
	{
		if (a_identity && a_identity->mask != 0 && a_identity->originalBase != 0 && Protocol::IsShareableRef(a_identity->originalBase) && a_originalLeveledHere) {
			return a_identity->originalBase;
		}
		return a_base;
	}

	// A copy of a friend's spawn waits this long for their report before it is placed from the spawn's base alone (the
	// report follows the server's grant, a moment after the spawn).
	inline constexpr auto IDENTITY_WAIT = std::chrono::seconds(2);

	// ---- The books ------------------------------------------------------------------------------------------------

	// The owner's: what it last reported for each NPC it runs, so each is reported when it changes (and afresh when it
	// runs one again).
	class Sent
	{
	public:
		// Whether a_identity is news (and from now on what was sent).
		bool Changed(const Protocol::NpcIdentity& a_identity)
		{
			auto& last = sent[a_identity.refId];
			if (last && *last == a_identity) {
				return false;
			}
			last = a_identity;
			return true;
		}

		[[nodiscard]] bool Has(std::uint32_t a_id) const { return sent.contains(a_id); }
		void               Forget(std::uint32_t a_id) { sent.erase(a_id); }
		void               Clear() { sent.clear(); }

	private:
		std::unordered_map<std::uint32_t, std::optional<Protocol::NpcIdentity>> sent;
	};

	// The others': each friend's report, and which of our copies are still to be checked against it. Making a roll
	// ours is tried TRIES times at most, RETRY apart: one that doesn't take is left as our game has it.
	class Book
	{
	public:
		static constexpr std::size_t MAX = 4096;
		static constexpr int         TRIES = 3;
		static constexpr auto        RETRY = std::chrono::seconds(5);
		using Clock = std::chrono::steady_clock;

		enum class Go
		{
			kWait,    // tried moments ago
			kTry,     // make it so now
			kGiveUp,  // tried TRIES times: left alone (Settled) until it changes
		};

		// A friend's game's report (never ours): kept, a newer one in place of the last, and our NPC checked again.
		bool Learn(const Protocol::NpcIdentity& a_identity, std::uint32_t a_localPlayer)
		{
			if (a_identity.playerId == 0 || a_identity.playerId == a_localPlayer) {
				return false;
			}
			if (!known.contains(a_identity.refId) && known.size() >= MAX) {
				return false;
			}
			auto& entry = known[a_identity.refId];
			if (entry && *entry == a_identity) {
				return false;
			}
			entry = a_identity;
			pending.insert(a_identity.refId);
			tries.erase(a_identity.refId);
			return true;
		}

		[[nodiscard]] const Protocol::NpcIdentity* Find(std::uint32_t a_id) const
		{
			const auto it = known.find(a_id);
			return it != known.end() && it->second ? &*it->second : nullptr;
		}

		// Our NPC is mirrored again (taken over from its own AI, or a copy placed): checked again, with what tries are left.
		void Check(std::uint32_t a_id)
		{
			if (known.contains(a_id)) {
				pending.insert(a_id);
			}
		}

		[[nodiscard]] std::vector<std::uint32_t> Due() const { return { pending.begin(), pending.end() }; }
		[[nodiscard]] bool                       IsDue(std::uint32_t a_id) const { return pending.contains(a_id); }

		// Nothing (more) to do: not checked again until it changes or is mirrored anew.
		void Settled(std::uint32_t a_id) { pending.erase(a_id); }

		Go Ask(std::uint32_t a_id, Clock::time_point a_now)
		{
			auto& entry = tries[a_id];
			if (entry.count >= TRIES) {
				return Go::kGiveUp;
			}
			if (entry.count > 0 && a_now - entry.last < RETRY) {
				return Go::kWait;
			}
			++entry.count;
			entry.last = a_now;
			return Go::kTry;
		}

		// Nobody runs it any more, or someone else does now: their report goes with it.
		void Forget(std::uint32_t a_id)
		{
			known.erase(a_id);
			pending.erase(a_id);
			tries.erase(a_id);
		}

		void Clear()
		{
			known.clear();
			pending.clear();
			tries.clear();
		}

		[[nodiscard]] std::size_t Size() const { return known.size(); }

	private:
		struct Tries
		{
			int               count = 0;
			Clock::time_point last{};
		};
		std::unordered_map<std::uint32_t, std::optional<Protocol::NpcIdentity>> known;
		std::unordered_set<std::uint32_t>                                       pending;
		std::unordered_map<std::uint32_t, Tries>                                tries;
	};
}
