#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

// Our copy of an NPC another game runs has its health there: the owner's game deals what the NPC takes, our player's
// hits are reported (WorldSync's health reports, then the server's belief in our kill), and another player's hits come
// as their health reports. Nothing else in our world is to hurt the copy. In the live test of the explosion ghosts a
// host's raider kept its 30 in the host's game, while the friend's copy went 30, 26, 21, 19, 12, 11 between the
// explosions it was a ghost for, and then lay dead at 0: something in the friend's world wore it down where the owner's
// game did not (what, CopyWatch's log of hits on copies names), and its death there was never believed.
//
// So a copy that loses health with no hit of ours on it (from OURS_GRACE before the drop on) is given it back
// (CopyWatch, each frame). A drop is judged only once it has stood for JUDGE_AFTER, so that our hit, whose event can
// come a frame after its damage, is never taken back; what wears a copy down a little at a time (a fire, a bleed) is
// given back before it adds up. A lethal blow in one go is not given back (the explosions of friends' throws are
// passed by: BlastShield). Health a report sets, or a copy gains, is where it stands from then on.
//
// What our hits take off a copy is booked (TakeOurs) and sent as damage (kReportDamage), which the game that runs the NPC
// takes off its own health; that game then shares where its health stands (HealthShare). A report of the copy's health
// instead, as before protocol 40, healed off the NPC whatever its own game had taken from it since the copy last heard (a
// fire's burn, a friend's shot on its way).
//
// Our hits are our player's and those of the actors our game is answerable for (Judge): each hit is to count in one
// game only, the game that runs whoever struck it. Pure, so it can be tested without the game.
namespace CopyHealth
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration OURS_GRACE = std::chrono::milliseconds{ 1000 };
	constexpr Clock::duration JUDGE_AFTER = std::chrono::milliseconds{ 100 };
	constexpr float           SLACK = 0.05f;      // smaller drops are rounding
	constexpr std::size_t     MAX_COPIES = 1024;  // copies watched at once (a guard)

	// What struck a hit (or a killing blow), as our game knows it on the main thread a frame later.
	struct Cause
	{
		bool          player = false;        // our player
		bool          actor = false;         // an actor at all (not: no cause, a hazard, a trap, or gone since)
		bool          standIn = false;       // a friend's stand-in (Puppets::IsPuppet)
		bool          shareable = false;     // a reference every game has (Protocol::IsShareableRef)
		std::uint32_t owner = 0;             // the player whose game runs it (NpcSync::Owner; 0: nobody, or not shareable)
		bool          ourCompanion = false;  // it follows our player (NpcSync::IsOurCompanion)
	};

	enum class Hitter : std::uint8_t
	{
		kNotOurs,
		kOurPlayer,
		kOurs,  // an actor our game runs: our companion, a settler, an enemy, or a robot companion only our game has
	};

	// Whose hit it is, with a_local our player (0: out of a session). Our player's are ours. An actor every game has is
	// ours when our game runs it (NpcSync gives each NPC to one game): our companion, as our game claims it, and any
	// other NPC our game runs, friend or foe. A raider our game runs that shoots a friend's settler hurts it in our game
	// only: in the friend's game our raider is a copy that only plays its shots (WeaponFire), so its hits count here or
	// nowhere. One another game runs is that game's: our copy of it is a mirror, or (a companion two players share) our
	// own of one their game runs, and it fights for real in that game. An actor made at runtime exists in its own game
	// only, and among those only our companion is ours (a robot built at a robot workbench, never seen by friends): a
	// random encounter of our game's would hurt a friend's NPC with something their game never shows. Not the turrets
	// and robots built in workshop mode either: WorkshopSync places a copy of each in every friend's game, a live one in
	// the player's faction, so it fires there too, at the NPCs that game runs, and those hits are real there; ours
	// counted here as well would count every turret twice, as would a friend's copy here. Stand-ins (their player's game
	// reports their hits), hazards and nothing at all are nobody's.
	constexpr Hitter Judge(const Cause& a_cause, std::uint32_t a_local)
	{
		if (a_cause.player) {
			return Hitter::kOurPlayer;
		}
		if (!a_cause.actor || a_cause.standIn || a_local == 0) {
			return Hitter::kNotOurs;
		}
		if (a_cause.shareable) {
			return a_cause.owner == a_local ? Hitter::kOurs : Hitter::kNotOurs;
		}
		return a_cause.ourCompanion ? Hitter::kOurs : Hitter::kNotOurs;
	}

	// What WorldSync does after a hit by a_hitter on an NPC every game has, a_owner's to run (0: nobody's): reports its
	// health, and claims it, which also tells the server we fight it, so our kill of it is believed (Server.cpp,
	// handleDeath). Our player's hits do both, on any such NPC. The rest of ours count only on a copy, an NPC another
	// game runs (one ours runs is hurt for real here): its health is reported, which tells the server we fight it too,
	// and a blow that killed it (a_killed) claims it, for a kill in one blow with no report before it. A claim for every
	// blow of a companion's or a settler's would move NPCs from game to game every few seconds in a fight beside a friend.
	struct AfterHit
	{
		bool report = false;
		bool claim = false;
	};

	// What a blow was, as the hit event says.
	struct Blow
	{
		bool explosion = false;     // an explosion's (HitData's kExplosion flag)
		bool throwable = false;     // its weapon, or the source, is a grenade, a Molotov or a mine
		bool spellAlone = false;    // a spell or an enchantment with no weapon: what a Molotov's fire leaves burning
	};

	// Whether the game that runs the NPC deals this blow there itself, so that a copy's hit is no hit of ours: a throw's.
	// Every throw's explosion is reported by the thrower's game and set off in each friend's (WeaponFire::ApplyExplosion),
	// where it hurts the NPCs that game runs, the fire it leaves too. Reported from the copy as well, a friend's Molotov
	// hurt the NPC twice in its own game: a super mutant at full health lay dead within two seconds of one in the
	// 2026-10-18 test, while the thrower's copy stood at 67. Our hits on our copies with a weapon in hand still count.
	constexpr bool OwnersToDeal(const Blow& a_blow)
	{
		return a_blow.explosion || a_blow.throwable || a_blow.spellAlone;
	}

	// a_ownersToDeal: the blow is one OwnersToDeal says the NPC's own game deals; on a copy it is reported never, and
	// claims the NPC only if it killed it here (so the server believes a kill no other blow told it of).
	constexpr AfterHit After(Hitter a_hitter, std::uint32_t a_owner, std::uint32_t a_local, bool a_killed, bool a_ownersToDeal = false)
	{
		const bool copy = a_owner != 0 && a_local != 0 && a_owner != a_local;
		if (copy && a_ownersToDeal && a_hitter != Hitter::kNotOurs) {
			return { .report = false, .claim = a_killed };
		}
		if (a_hitter == Hitter::kOurPlayer) {
			return { .report = true, .claim = true };
		}
		if (a_hitter != Hitter::kOurs || !copy) {
			return {};
		}
		return { .report = true, .claim = a_killed };
	}

	// Whether a death a_cause dealt is our player's kill (ActorDeath::killed: the friends' kill feed names our player, and
	// our stand-in pushes the corpse): our player's, and our companion's when its hits are ours. Not every NPC our game
	// runs: a raider of ours that kills a settler is no kill of our player's.
	constexpr bool Credited(const Cause& a_cause, std::uint32_t a_local)
	{
		const auto hitter = Judge(a_cause, a_local);
		return hitter == Hitter::kOurPlayer || (hitter == Hitter::kOurs && a_cause.ourCompanion);
	}

	class Book
	{
	public:
		// A hit of ours (Judge) on the copy now.
		void OurHit(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.ourHit = a_now;
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .health = std::nullopt, .ourHit = a_now };
			}
		}

		// The copy's health is a_health now by a report, or as first seen: where it stands.
		void Set(std::uint32_t a_id, float a_health)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.health = a_health;
				it->second.dropSince.reset();
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .health = a_health };
			}
		}

		// The copy reads a_health now: how much to give back (0: none, or not yet). After giving it back, Set what it reads.
		float Check(std::uint32_t a_id, float a_health, Clock::time_point a_now)
		{
			auto it = entries.find(a_id);
			if (it == entries.end() || !it->second.health) {
				Set(a_id, a_health);
				return 0.0f;
			}
			auto& entry = it->second;
			if (a_health >= *entry.health - SLACK) {
				entry.health = a_health;
				entry.dropSince.reset();
				return 0.0f;
			}
			if (!entry.dropSince) {
				entry.dropSince = a_now;
			}
			// A hit of ours around the drop: the copy is ours to hurt, and what it took goes to the NPC's game (TakeOurs).
			if (entry.ourHit && *entry.ourHit + OURS_GRACE >= *entry.dropSince) {
				entry.ours += *entry.health - a_health;
				entry.health = a_health;
				entry.dropSince.reset();
				return 0.0f;
			}
			if (a_now - *entry.dropSince < JUDGE_AFTER) {
				return 0.0f;
			}
			entry.dropSince.reset();
			return *entry.health - a_health;
		}

		// Another game's report sets the copy from a_before to a_health now. What a hit of ours took off it since it was last
		// checked (the report came between the blow and the next frame) is ours all the same, not overwritten.
		void Heard(std::uint32_t a_id, float a_before, float a_health, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				auto& entry = it->second;
				if (entry.health && entry.ourHit && a_before < *entry.health - SLACK && *entry.ourHit + OURS_GRACE >= a_now) {
					entry.ours += *entry.health - a_before;
				}
			}
			Set(a_id, a_health);
		}

		// What hits of ours took off the copy since this was last asked (0: nothing): for the game that runs it to take off.
		float TakeOurs(std::uint32_t a_id)
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? std::exchange(it->second.ours, 0.0f) : 0.0f;
		}

		// The health the copy stands at (none before it is first seen).
		[[nodiscard]] std::optional<float> Health(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? it->second.health : std::nullopt;
		}

		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		// Forgets every copy a_keep says no to (no longer a copy of ours, or gone).
		template <class Keep>
		void Prune(Keep a_keep)
		{
			std::erase_if(entries, [&](const auto& a_entry) { return !a_keep(a_entry.first); });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		struct Entry
		{
			std::optional<float>             health;
			std::optional<Clock::time_point> ourHit;
			std::optional<Clock::time_point> dropSince;
			float                            ours = 0.0f;  // taken by hits of ours, not yet sent (TakeOurs)
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
