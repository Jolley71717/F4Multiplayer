#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
// So while a friend's explosion goes off, those copies are lent so much health that it cannot kill them, and
// when the lending ends (or sooner, see below) what the blast took is given back. Lent as a temporary modifier
// of health, the way the engine fortifies it, which a save never keeps. Our own player's blows meanwhile are
// ours to deal (the server believes a kill of ours), so they stay: one that hits a lent copy ends its lending
// at once, keeping its own damage, and a copy those blows take to nothing dies. A health report for the copy
// from another game ends the lending first too, so the report is measured without the lent health. Pure, so
// it can be tested without the game.
namespace BlastShield
{
	using Clock = std::chrono::steady_clock;

	// More than any thrown explosion takes (a Fat Man's shell is a few hundred).
	constexpr float LEND = 100000.0f;
	// How long a copy is lent health after a friend's explosion set off near it: the engine deals an explosion's
	// damage within its first frames.
	constexpr Clock::duration HOLD = std::chrono::seconds{ 3 };
	// Copies reached this far beyond the explosion's own radius are lent health too (the engine measures to their
	// bodies, not their feet).
	constexpr float MARGIN = 256.0f;
	constexpr std::size_t MAX_LENT = 256;  // copies lent health at once (a guard)

	// Whether a friend's explosion set off in our game is kept from hurting this NPC: a_owner runs it (0 nobody),
	// a_local is our player. Ours, and those nobody runs, it hurts.
	constexpr bool Shields(std::uint32_t a_owner, std::uint32_t a_local)
	{
		return a_owner != 0 && a_owner != a_local;
	}

	// What to do to the copy's health when its lending ends, in this order.
	struct Release
	{
		float restore = 0.0f;  // given back first, while the lent health is still there
		bool  kill = false;    // our player's blows took it to nothing: it dies
		float lent = 0.0f;     // then the lent health is taken away again
	};

	class Book
	{
	public:
		// A friend's explosion goes off near a copy whose health is a_health now: how much health to lend it (0: it
		// is lent some already, and keeps it a while longer; or too many are).
		float Begin(std::uint32_t a_id, float a_health, Clock::time_point a_now)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.until = a_now + HOLD;
				return 0.0f;
			}
			if (entries.size() >= MAX_LENT || !(a_health > 0.0f)) {
				return 0.0f;
			}
			entries.emplace(a_id, Entry{ a_health, LEND, 0.0f, a_now + HOLD });
			return LEND;
		}

		[[nodiscard]] bool Lent(std::uint32_t a_id) const { return entries.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }

		// Our player's blow took a_damage from a lent copy (each counted, a shotgun's pellets in one frame too).
		void OurBlow(std::uint32_t a_id, float a_damage)
		{
			if (const auto it = entries.find(a_id); it != entries.end() && a_damage > 0.0f) {
				it->second.ours += a_damage;
			}
		}

		// The copies whose lending is over.
		[[nodiscard]] std::vector<std::uint32_t> Due(Clock::time_point a_now) const
		{
			std::vector<std::uint32_t> due;
			for (const auto& [id, entry] : entries) {
				if (a_now >= entry.until) {
					due.push_back(id);
				}
			}
			return due;
		}

		// Ends the copy's lending: a_health is its health now (the lent health in it), a_dead whether it died
		// meanwhile (its owner's game said so). Nothing if it was lent none.
		std::optional<Release> End(std::uint32_t a_id, float a_health, bool a_dead)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return std::nullopt;
			}
			const Entry entry = it->second;
			entries.erase(it);
			if (a_dead) {
				return Release{ .lent = entry.lent };
			}
			// What it had, less what our player took: whatever else it lost meanwhile (the blast) comes back. It
			// keeps health it gained meanwhile.
			const float keep = entry.before - entry.ours;
			if (!(keep > 0.0f)) {
				return Release{ .kill = true, .lent = entry.lent };
			}
			const float restore = keep + entry.lent - a_health;
			return Release{ .restore = restore > 0.0f ? restore : 0.0f, .lent = entry.lent };
		}

		// The copy is gone (deleted, or never found again): nothing to give back.
		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

	private:
		struct Entry
		{
			float             before = 0.0f;  // its health when the explosion went off
			float             lent = 0.0f;
			float             ours = 0.0f;    // what our player's blows took since
			Clock::time_point until{};
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
