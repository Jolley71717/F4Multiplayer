#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// A friend's explosion set off in our game (WeaponFire::ApplyExplosion) is what hurts the NPCs our game runs,
// as it did those the friend's game runs in theirs. Our copies of NPCs another game runs are not ours to hurt
// with it: their owner's game sets the same explosion off on its own copy (the thrower's game first), and a
// death of one in our game is never believed (the server drops a death of an NPC another game runs unless our
// player was fighting it). A host's raider that threw a grenade survived it in the host's game with 30 health,
// while the friend's copy died of the replay, stopped being mirrored and lay dead beside the raider still
// fighting on the host's screen.
//
// So while a friend's explosion goes off, those copies are made ghosts (the engine's own flag, the console's
// SetGhost): the engine's hits pass a ghost by, the explosion's among them, and its health is never touched, so
// nothing has to be given back afterwards. Three rounds of lending them health and paying it back went wrong in
// the engine's arithmetic (a loan left on, one taken back as a kill). The ghost goes after a second and a few
// frames, when the engine has dealt the explosion's damage. While it lasts our player's shots pass the copy by
// too: a second of that, beside a grenade going off, is the price. It is never put on the player, a stand-in, an
// NPC our game runs or nobody does, nor on an actor that was a ghost already, and it comes off before a save or a
// load, when the copy stops being mirrored, when its NPC stops being another game's, and when the session ends.
// This is the bookkeeping, pure so it can be tested without the game.
namespace BlastShield
{
	using Clock = std::chrono::steady_clock;

	// How long a copy stays a ghost after a friend's explosion is set off near it, at least: the engine deals an
	// explosion's damage within its first frames. Another explosion meanwhile holds it longer.
	constexpr Clock::duration HOLD = std::chrono::seconds{ 1 };
	// ... and at least this many frames, should the game hitch as the explosion goes off.
	constexpr std::uint32_t HOLD_FRAMES = 10;
	// Copies reached this far beyond the explosion's own radius are made ghosts too (the engine measures to their
	// bodies, not their feet).
	constexpr float       MARGIN = 256.0f;
	constexpr std::size_t MAX_SHIELDED = 256;  // copies ghosted at once (a guard)

	// Whether a friend's explosion set off in our game is kept from hurting this NPC: a_owner runs it (0 nobody),
	// a_local is our player. Ours, and those nobody runs, it hurts.
	constexpr bool Shields(std::uint32_t a_owner, std::uint32_t a_local)
	{
		return a_owner != 0 && a_local != 0 && a_owner != a_local;
	}

	class Book
	{
	public:
		// A friend's explosion goes off near a copy (a_frame counts our frames): whether it is to be made a ghost now
		// (false: it is one of ours already, and stays one a while longer; or too many are).
		bool Begin(std::uint32_t a_id, Clock::time_point a_now, std::uint64_t a_frame)
		{
			const Entry until{ a_now + HOLD, a_frame + HOLD_FRAMES };
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.at = std::max(it->second.at, until.at);
				it->second.frame = std::max(it->second.frame, until.frame);
				return false;
			}
			if (entries.size() >= MAX_SHIELDED) {
				return false;
			}
			entries.emplace(a_id, until);
			return true;
		}

		[[nodiscard]] bool        Shielded(std::uint32_t a_id) const { return entries.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }

		// The copies whose ghost is to come off now: both its time and its frames are past. They are forgotten.
		std::vector<std::uint32_t> Due(Clock::time_point a_now, std::uint64_t a_frame)
		{
			std::vector<std::uint32_t> due;
			std::erase_if(entries, [&](const auto& a_entry) {
				if (a_now < a_entry.second.at || a_frame < a_entry.second.frame) {
					return false;
				}
				due.push_back(a_entry.first);
				return true;
			});
			return due;
		}

		// The copy's ghost is to come off at once (it stops being mirrored, or another game's): whether it had one of ours.
		bool End(std::uint32_t a_id) { return entries.erase(a_id) > 0; }

		// Every copy's, at once (a save, a load, the session's end). They are forgotten.
		std::vector<std::uint32_t> EndAll()
		{
			std::vector<std::uint32_t> all;
			all.reserve(entries.size());
			for (const auto& [id, entry] : entries) {
				all.push_back(id);
			}
			entries.clear();
			return all;
		}

		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(entries.size());
			for (const auto& [id, entry] : entries) {
				ids.push_back(id);
			}
			return ids;
		}

	private:
		struct Entry
		{
			Clock::time_point at{};
			std::uint64_t     frame = 0;
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
