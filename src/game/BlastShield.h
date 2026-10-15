#pragma once

#include <algorithm>
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
// when the lending ends (or sooner, see below) what the blast took is given back. Our own player's blows
// meanwhile are ours to deal (the server believes a kill of ours), so they stay: one that hits a lent copy ends
// its lending at once, keeping its own damage, and a copy those blows take to nothing dies. A health report for
// the copy from another game ends the lending first too, so the report is measured without the lent health.
//
// How the engine keeps health decides how the loan is made and paid back. Its health is what the record gives
// plus a permanent, a temporary and a damage modifier. Healing (RestoreActorValue) never takes it past the
// permanent maximum, the record's health and the permanent modifier, and puts it there when it is above: lent
// as a temporary modifier, a copy the blast had hurt was "given back" its health right down to that maximum, and
// taking the loan away then left it 100000 below nothing, dead. And changing a modifier so that a living actor
// has no health left kills it. So the loan is a permanent modifier (healing then reaches the lent health too;
// WeaponFire ends every loan before a save, which would keep it). Paid back through the engine's own changes, the
// copy healed to the health it keeps with the loan still in it and the loan then taken away, the live test's two
// copies read their own 30 at once and lay dead at 0 a moment later: the blast had left them 33 and 18 below
// nothing of their own, and the engine seems to judge the change by that. A kill of a living actor leaves it at 0;
// any damage would have left it below. So the loan is paid back by writing the copy's three health modifiers as
// Repaid says, which gives the engine no change to judge (WeaponFire checks first that they read as the engine
// reads them, and pays back through the engine only if they do not). Pure, so it can be tested without the game.
namespace BlastShield
{
	using Clock = std::chrono::steady_clock;

	// More than any thrown explosion takes (a Fat Man's shell is a few hundred).
	constexpr float LEND = 100000.0f;
	// How long a copy is lent health after a friend's explosion set off near it: the engine deals an explosion's
	// damage within its first frames. Another explosion meanwhile holds it longer.
	constexpr Clock::duration HOLD = std::chrono::seconds{ 3 };
	// Copies reached this far beyond the explosion's own radius are lent health too (the engine measures to their
	// bodies, not their feet).
	constexpr float MARGIN = 256.0f;
	constexpr std::size_t MAX_LENT = 256;  // copies lent health at once (a guard)
	// What a copy our player's blows killed under the loan is left with when the loan goes, before it is killed: some
	// health, so taking the loan away does not kill it first, with nobody to blame.
	constexpr float BEFORE_KILL = 1.0f;

	// Whether a friend's explosion set off in our game is kept from hurting this NPC: a_owner runs it (0 nobody),
	// a_local is our player. Ours, and those nobody runs, it hurts.
	constexpr bool Shields(std::uint32_t a_owner, std::uint32_t a_local)
	{
		return a_owner != 0 && a_owner != a_local;
	}

	// What to do when a copy's loan ends: leave it with health and the loan gone (Repaid), and kill it if it is to die.
	struct Release
	{
		float health = 0.0f;  // what it keeps
		float lent = 0.0f;
		bool  kill = false;   // our player's blows took it to nothing
	};

	// A copy's health modifiers, as the engine keeps them.
	struct Modifiers
	{
		float permanent = 0.0f;
		float temporary = 0.0f;
		float damage = 0.0f;
	};

	// The modifiers that leave a copy whose record gives it a_base health, and whose modifiers are a_now with a loan of
	// a_lent in them, with a_keep health and the loan gone: the loan comes out of the permanent modifier and the damage
	// modifier makes up the rest. Never above nothing: health past the maximum is not kept (healing keeps none).
	constexpr Modifiers Repaid(Modifiers a_now, float a_base, float a_lent, float a_keep)
	{
		Modifiers out{ .permanent = a_now.permanent - a_lent, .temporary = a_now.temporary };
		out.damage = std::min(0.0f, a_keep - (a_base + out.permanent + out.temporary));
		return out;
	}

	class Book
	{
	public:
		// A friend's explosion goes off near a copy whose health is a_health now: how much health to lend it (0: it
		// is lent some already, and keeps it a while longer; or too many are).
		float Begin(std::uint32_t a_id, float a_health, Clock::time_point a_now)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.until = std::max(it->second.until, a_now + HOLD);
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

		// The copies whose loan is over.
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

		// Ends the copy's loan: a_health is its health now (the loan in it), a_dead whether it died meanwhile (its
		// owner's game said so). Nothing if it was lent none; each loan ends once.
		std::optional<Release> End(std::uint32_t a_id, float a_health, bool a_dead)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return std::nullopt;
			}
			const Entry entry = it->second;
			entries.erase(it);
			const float without = a_health - entry.lent;  // what it has of its own
			if (a_dead) {
				// A corpse keeps what it had, and never reads the loan's worth below nothing.
				return Release{ .health = std::max(without, 0.0f), .lent = entry.lent };
			}
			// What it had, less what our player took: whatever else it lost meanwhile (the blasts) comes back. It
			// keeps health it gained meanwhile.
			const float keep = entry.before - entry.ours;
			if (!(keep > 0.0f)) {
				return Release{ .health = BEFORE_KILL, .lent = entry.lent, .kill = true };
			}
			return Release{ .health = std::max(keep, without), .lent = entry.lent };
		}

		// The copy is gone (deleted, or never found again): nothing to give back.
		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

	private:
		struct Entry
		{
			float             before = 0.0f;  // its health when the first explosion went off
			float             lent = 0.0f;
			float             ours = 0.0f;    // what our player's blows took since
			Clock::time_point until{};
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
