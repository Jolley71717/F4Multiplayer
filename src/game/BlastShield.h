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
// NPC our game runs or nobody does, nor on an actor that was a ghost already (unless it is a ghost of ours still to
// come off: GhostLedger), and it comes off before a save or a load, when the copy stops being mirrored, when its NPC
// stops being another game's, and when the session ends.
//
// Why a ghost and not something narrower: the mod's hit hook (WorldSync's HitWatcher) hears the engine's hit event a
// frame after the damage is dealt, so it can filter nothing; the engine's damage path has no hook of the mod's; and
// a copy that only loses health to the explosion gets it back from CopyWatch (a drop with no hit of ours is given back
// a tenth of a second later) unless the blow killed it, which no give-back undoes (its death here is never believed,
// and the copy stops being mirrored). So the ghost is kept for the blow that could kill: a copy is made one only when
// the explosion's damage comes near its health (Lethal). A copy with health to spare takes the blast and is given it
// back, its hit booked as the owner's blow (CopyWatch), so a friend firing explosive rounds no longer makes every copy
// around their target unhittable for a second per round. This is the bookkeeping, pure so it can be tested without the
// game.
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
	// An explosion whose record's damage, times this, reaches the copy's health could kill it (the engine adds its own
	// margins: a sneak attack's, a perk's, a difficulty's are the thrower's and never apply to one set off here with no
	// owner, but the record's figure is before resistances either way, so the margin errs on the ghost's side).
	constexpr float LETHAL_MARGIN = 2.0f;

	// Whether a friend's explosion set off in our game is kept from hurting this NPC: a_owner runs it (0 nobody),
	// a_local is our player. Ours, and those nobody runs, it hurts.
	constexpr bool Shields(std::uint32_t a_owner, std::uint32_t a_local)
	{
		return a_owner != 0 && a_local != 0 && a_owner != a_local;
	}

	// Whether an explosion dealing a_damage (its record's; 0 or less: unknown, a trap's grenades, a death blast) could
	// kill a copy with a_health (less than 0: unknown).
	constexpr bool Lethal(float a_health, float a_damage)
	{
		return a_damage <= 0.0f || a_health < 0.0f || a_health <= a_damage * LETHAL_MARGIN;
	}

	// An actor a friend's explosion reaches, as our game knows it. Every copy goes by the id the session knows it by
	// (NpcSync::SessionId): a plugin's reference is the same in every game, and our copy of an NPC a friend's game made
	// at runtime (a random encounter's raider, an ambush: Spawns) goes by its spawn id, which is what NpcSync says the
	// owner of and ends the ghost of (StopMirroring). By its own form id such a copy had no owner, so it was never a
	// ghost, and a friend's grenade killed the friend's spawned enemies in our game while they fought on in theirs.
	struct Reached
	{
		std::uint32_t id = 0;     // the session's id for it (0: never shared, so nobody's: one our game made, a stand-in)
		std::uint32_t owner = 0;  // the player whose game runs it (NpcSync::Owner of that id; 0 nobody)
		bool          dead = false;
		float         health = -1.0f;  // its health now (less than 0: not read)
	};

	// Whether the actor is to be a ghost while the explosion goes off: one the session knows, alive, that another
	// game runs (Shields above), and one the explosion's damage (a_damage) could kill (Lethal). The player and the
	// friends' stand-ins are never reached this way (the caller leaves them out: the engine never counts them as NPCs
	// here).
	constexpr bool Shields(const Reached& a_actor, std::uint32_t a_local, float a_damage = 0.0f)
	{
		return a_actor.id != 0 && !a_actor.dead && Shields(a_actor.owner, a_local) && Lethal(a_actor.health, a_damage);
	}

	// A save written while a ghost of ours was still on (its setghost 0 lands a frame late; 0.8.10 and 0.8.11 saved it
	// so mid-fight; up to 0.9.2 a copy that unloaded before its setghost 0 was written as a ghost too) loads it a
	// ghost. Saves made since carry the ledger of our ghosts (GhostLedger), which takes them off. For a save from
	// before, WeaponFire looks every REPAIR_EVERY frames at the loaded actors whose ghost flag reads on and takes the
	// ghost off one that looks like ours, once per actor per load: a plugin's reference (the only kind a save could
	// hold a ghost of ours on; the session need not know it now), alive, its flag on though its base record makes it no
	// ghost and no quest holds it in an alias (a story's ghost is set by a quest on an actor it holds: an essential
	// character in a scene), and not a ghost of ours now (shielded, or in the ledger: those the ledger clears). Whose
	// the NPC is now, and whether it is hostile, is no part of it: before, one our game had come to run, one nobody ran
	// or one not yet hostile when the thirty seconds ran out stayed a ghost for the rest of the save. A ghost cleared
	// this way that a script sets again is left alone from then on (once per actor per load).
	struct Found
	{
		std::uint32_t id = 0;            // its form id (0: none)
		bool          dead = false;
		bool          ghost = false;      // its ghost flag reads on (the extra data the save wrote)
		bool          baseGhost = false;  // its base record makes it a ghost: its story's own
		bool          questAlias = false; // a quest holds it in an alias: a story's own
		bool          ours = false;       // a ghost of ours now (shielded, or in the ledger)
	};

	constexpr bool Repairs(const Found& a_actor)
	{
		return a_actor.id != 0 && (a_actor.id >> 24) != 0xFF && !a_actor.dead && a_actor.ghost && !a_actor.baseGhost && !a_actor.questAlias && !a_actor.ours;
	}

	// How often (frames) the loaded actors are looked at for that.
	constexpr std::uint32_t REPAIR_EVERY = 15;

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
