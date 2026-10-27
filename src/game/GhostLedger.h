#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Every ghost the mod sets (BlastShield: a copy made a ghost while a friend's explosion goes off) is written down here
// under our copy's own form id, the id the console's setghost takes, and stays written down until the engine reads the
// copy as no ghost again. Before this ledger a ghost could be left behind for good: its copy unloaded before its setghost
// 0 (a door, a fast travel, a despawn), the console's command landing a frame late and retried three times at most, a
// save written with the flag on and loaded into a game whose repair looked for thirty seconds, only at copies another
// game ran that were hostile right then. The player's shots passed through such a copy for the rest of the save: "only
// one of us can damage the NPCs". Now the ledger decides each frame what is to be done with each entry (Check): nothing
// while the copy is not loaded or a shield of ours still wants the ghost; setghost 0 again when it reads a ghost, with a
// gap that grows (RETRY_AFTER frames, doubling to RETRY_MOST) so a ghost the engine will not let go of costs a command
// every few seconds and no more; and the entry is forgotten once the copy reads no ghost. Whose the copy is, and whether
// it is hostile, is no part of it: a ghost of ours comes off whoever runs the NPC now.
//
// The ledger goes into the save's co-save record (Encode) and comes back with it (Decode): a save written while a
// ghost of ours was on (its copy unloaded, so HideGhostsFromSave could not turn the flag off; the game crashed or quit
// a moment later) loads with its ledger, and the copy's ghost comes off as it loads, in that save's first run or any
// later one. Only a plugin's references are written: a copy of a friend's spawn is never saved (Spawns discards it
// before the save), and a form id made at runtime names another actor after a load. Pure, so it can be tested without
// the game.
namespace GhostLedger
{
	constexpr std::size_t   MAX = 512;         // ghosts written down at once (a guard)
	constexpr std::uint32_t RETRY_AFTER = 6;   // frames after a setghost 0 before it is sent again, if the ghost is still on
	constexpr std::uint32_t RETRY_MOST = 300;  // ... growing to this many (five seconds at 60 fps) for a ghost that will not go
	constexpr std::uint8_t  STUCK_AFTER = 3;   // setghost 0s one copy needed before it counts as stuck (for the counters and the log)
	constexpr std::uint16_t RECORD_VERSION = 1;

	// A plugin's reference: the same form id in every run of the save (F4SE resolves it for a changed load order).
	constexpr bool Plugin(std::uint32_t a_id)
	{
		return a_id != 0 && (a_id >> 24) != 0xFF;
	}

	// What our game reads of the copy this frame.
	struct Seen
	{
		bool loaded = false;    // the actor is here with 3D (the console's command can take on it)
		bool ghost = false;     // the engine counts it a ghost now (Actor::GetGhost)
		bool shielded = false;  // a shield of ours still wants it a ghost (BlastShield::Book)
	};

	enum class Step : std::uint8_t
	{
		kWait,   // not loaded, still shielded, or a setghost 0 sent lately has yet to land
		kClear,  // send setghost 0 (again)
		kDone,   // no ghost: forgotten
	};

	class Book
	{
	public:
		// A ghost we set on the copy now (a_fromSave: one a loaded save brought, found by the repair scan). Whether it
		// is newly written down.
		bool Add(std::uint32_t a_id, std::uint64_t a_frame, bool a_fromSave = false)
		{
			if (entries.contains(a_id)) {
				return false;
			}
			if (entries.size() >= MAX) {
				++refused;
				return false;
			}
			entries.emplace(a_id, Entry{ .added = a_frame, .fromSave = a_fromSave });
			++set;
			if (a_fromSave) {
				++fromSaves;
			}
			return true;
		}

		// A setghost 0 went out for the copy now (Unshield's, the repair's, or one Check asked for).
		void Sent(std::uint32_t a_id, std::uint64_t a_frame)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				auto& entry = it->second;
				entry.sent = a_frame;
				entry.tries = static_cast<std::uint8_t>((std::min)(entry.tries + 1, 255));
				if (entry.tries == STUCK_AFTER) {
					++stuck;
				}
			}
		}

		// What to do with the copy this frame. kDone forgets it; the caller sends the command for kClear and calls Sent.
		Step Check(std::uint32_t a_id, const Seen& a_seen, std::uint64_t a_frame)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return Step::kDone;
			}
			auto& entry = it->second;
			if (a_seen.shielded || !a_seen.loaded) {
				return Step::kWait;
			}
			if (!a_seen.ghost) {
				++cleared;
				if (entry.tries >= STUCK_AFTER) {
					++unstuck;
				}
				entries.erase(it);
				return Step::kDone;
			}
			if (entry.tries == 0 || a_frame - entry.sent >= RetryAfter(entry.tries)) {
				return Step::kClear;
			}
			return Step::kWait;
		}

		// Frames to wait after the n-th setghost 0 before the next: 6, 12, 24, ... up to RETRY_MOST.
		static constexpr std::uint32_t RetryAfter(std::uint8_t a_tries)
		{
			std::uint32_t after = RETRY_AFTER;
			for (std::uint8_t i = 1; i < a_tries && after < RETRY_MOST; ++i) {
				after *= 2;
			}
			return (std::min)(after, RETRY_MOST);
		}

		// The copy is gone for good (a runtime form id that names nothing of ours any more): forgotten, not cleared.
		bool Forget(std::uint32_t a_id) { return entries.erase(a_id) > 0; }

		void Clear() { entries.clear(); }

		[[nodiscard]] bool        Contains(std::uint32_t a_id) const { return entries.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }

		[[nodiscard]] std::uint8_t Tries(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? it->second.tries : 0;
		}

		[[nodiscard]] bool FromSave(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() && it->second.fromSave;
		}

		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(entries.size());
			for (const auto& [id, entry] : entries) {
				ids.push_back(id);
			}
			std::ranges::sort(ids);
			return ids;
		}

		// The entries a save keeps: the plugins' references, sorted.
		[[nodiscard]] std::vector<std::uint32_t> Saved() const
		{
			auto ids = Ids();
			std::erase_if(ids, [](std::uint32_t a_id) { return !Plugin(a_id); });
			return ids;
		}

		// A loaded save's entries (its record, each id resolved for this run's load order): written down afresh, as
		// ghosts a save brought. How many were.
		std::size_t Restore(const std::vector<std::uint32_t>& a_ids, std::uint64_t a_frame)
		{
			std::size_t added = 0;
			for (const auto id : a_ids) {
				if (Plugin(id) && Add(id, a_frame, true)) {
					++added;
				}
			}
			return added;
		}

		// Ghosts set, cleared (read as no ghost again), stuck (needed STUCK_AFTER commands or more), of those cleared
		// after all, brought by loaded saves, and refused for want of room. Rolling over the run.
		struct Counts
		{
			std::uint32_t set = 0, cleared = 0, stuck = 0, unstuck = 0, fromSaves = 0, refused = 0;
		};

		[[nodiscard]] Counts Count() const { return { set, cleared, stuck, unstuck, fromSaves, refused }; }

	private:
		struct Entry
		{
			std::uint64_t added = 0;     // the frame it was set in (or found, from a save)
			std::uint64_t sent = 0;      // the frame its last setghost 0 went out in
			std::uint8_t  tries = 0;     // setghost 0s sent for it
			bool          fromSave = false;
		};

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint32_t                            set = 0, cleared = 0, stuck = 0, unstuck = 0, fromSaves = 0, refused = 0;
	};

	// The record's bytes: the version, the count, then each id; nothing when there is none to write.
	inline std::vector<std::uint8_t> Encode(const std::vector<std::uint32_t>& a_ids)
	{
		std::vector<std::uint8_t> out;
		std::vector<std::uint32_t> ids;
		for (const auto id : a_ids) {
			if (Plugin(id) && ids.size() < MAX && std::ranges::find(ids, id) == ids.end()) {
				ids.push_back(id);
			}
		}
		if (ids.empty()) {
			return out;
		}
		const auto put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		put(RECORD_VERSION);
		put(static_cast<std::uint16_t>(ids.size()));
		for (const auto id : ids) {
			put(id);
		}
		return out;
	}

	// A save's record, each id once (not yet resolved for the load order); nothing if it can't be read (another version,
	// a count past MAX, a length that is not the count's, a runtime id).
	inline std::optional<std::vector<std::uint32_t>> Decode(std::span<const std::uint8_t> a_bytes)
	{
		std::uint16_t version = 0;
		std::uint16_t count = 0;
		if (a_bytes.size() < sizeof(version) + sizeof(count)) {
			return std::nullopt;
		}
		std::memcpy(&version, a_bytes.data(), sizeof(version));
		std::memcpy(&count, a_bytes.data() + sizeof(version), sizeof(count));
		if (version != RECORD_VERSION || count > MAX || a_bytes.size() != sizeof(version) + sizeof(count) + std::size_t{ count } * sizeof(std::uint32_t)) {
			return std::nullopt;
		}
		std::vector<std::uint32_t> ids(count);
		for (std::size_t i = 0; i < count; ++i) {
			std::memcpy(&ids[i], a_bytes.data() + sizeof(version) + sizeof(count) + i * sizeof(std::uint32_t), sizeof(std::uint32_t));
			if (!Plugin(ids[i])) {
				return std::nullopt;
			}
		}
		std::ranges::sort(ids);
		const auto repeats = std::ranges::unique(ids);
		ids.erase(repeats.begin(), repeats.end());
		return ids;
	}
}
