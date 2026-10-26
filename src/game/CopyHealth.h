#pragma once

#include <algorithm>
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

	// What a blow was, as the hit event says, and whether our explosions are being shared as it lands.
	struct Blow
	{
		bool explosion = false;     // an explosion's (HitData's kExplosion flag)
		bool throwable = false;     // its weapon, or the source, is a grenade, a Molotov or a mine
		bool spellAlone = false;    // a spell or an enchantment with no weapon: what a Molotov's fire leaves burning
		bool inHand = false;        // its weapon, or the source, is a weapon that is no throwable: a gun, a bat, a baton
		bool explosionsShared = false;  // our game reports the explosions around our player now (a throw's watch is on)
		bool threwLately = false;       // ... or did a little while ago: the fire a Molotov left may burn there still
		bool blastThere = false;        // an explosion's: the one that dealt it goes off in the NPC's own game too
		                                // (RemoteEdits::DealtThere, judged on the main thread as the hit is taken)
	};

	// Whether the game that runs the NPC deals this blow there itself, so that a copy's hit is no hit of ours: a throw's.
	// Every throw's explosion is reported by the thrower's game and set off in each friend's (WeaponFire::ApplyExplosion),
	// where it hurts the NPCs that game runs, the fire it leaves too. Reported from the copy as well, a friend's Molotov
	// hurt the NPC twice in its own game: a super mutant at full health lay dead within two seconds of one in the
	// 2026-10-18 test, while the thrower's copy stood at 67.
	//
	// Only then. In 0.9.0 the players' gun hits on copies were left to the owner (none reported, each given back, no
	// takeover), while a shock baton's took the NPC over: only the game that ran an NPC could hurt it, and the one who
	// last hit it in melee became that game. The kExplosion bit (CommonLib's layout, from another game) and a hit with no
	// weapon read are no proof of a throw. An explosion's blow is the owner's exactly when the explosion that dealt it goes
	// off there (blastThere): one our game reported (our player's or an NPC's we run, the server sure to pass it on:
	// RemoteEdits::ReportGate; the NPC's game sets it off beside its NPC wherever its player is), a destructible object's
	// own or a world trap's (every game sets its own off). Any other, one kept ours (too far, too big, past the budget),
	// or a gun's hit read as an explosion's with none there, is dealt nowhere else and is ours to report. Any other blow with
	// a weapon in hand that is no throwable is ours whatever else the event reads, and the burn a Molotov's fire leaves
	// is the owner's for WeaponFire::THREW_LATELY after the watch.
	constexpr bool OwnersToDeal(const Blow& a_blow)
	{
		if (a_blow.explosion) {
			return a_blow.blastThere;
		}
		if (a_blow.inHand) {
			return false;
		}
		return (a_blow.throwable && a_blow.explosionsShared) || (a_blow.spellAlone && (a_blow.explosionsShared || a_blow.threwLately));
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

	// A drop with a hit of ours around it is ours. But the thrower's own copy is no ghost for their throw (BlastShield
	// shields copies from friends' explosions only), so their shot in the same second as their Molotov's blast, or while its
	// fire burns, took the whole drop for theirs: blast and burn were sent as their damage to the NPC's game, which had dealt
	// them itself, and the NPC took them twice. So with a blow left to the owner (OwnersBlow) around the drop, only what our
	// hits said they took (their HitData figure) is ours; the rest is the owner's, given back. Without one, or with hits of
	// ours none of whose events had a figure, the whole drop is ours as before (a figure short of what a hit took must not lose it).
	class Book
	{
	public:
		// A hit of ours (Judge) on the copy now, that took a_damage as its event says (none: the event had no figure).
		void OurHit(std::uint32_t a_id, Clock::time_point a_now, std::optional<float> a_damage = std::nullopt)
		{
			auto it = entries.find(a_id);
			if (it == entries.end()) {
				if (entries.size() >= MAX_COPIES) {
					return;
				}
				it = entries.emplace(a_id, Entry{}).first;
			}
			auto& entry = it->second;
			if (!entry.ourHit || *entry.ourHit + OURS_GRACE < a_now) {
				entry.dealt = 0.0f;
				entry.unknown = false;
				entry.figured = false;
			}
			entry.ourHit = a_now;
			if (a_damage) {
				entry.dealt += *a_damage;
				entry.figured = true;
			} else {
				entry.unknown = true;
			}
		}

		// A blow of ours on the copy now that the NPC's own game deals itself (OwnersToDeal): a throw's blast, its fire.
		void OwnersBlow(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.ownersBlow = a_now;
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .ownersBlow = a_now };
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
			// A report held while our hits landed (Heard) stands once they are over: by then the owner's next report would
			// have said otherwise, had our hits reached its game. Not while a drop beside our last hit is being judged: that
			// may be ours to send yet.
			if (entry.held && !entry.dropSince && !(entry.ourHit && *entry.ourHit + OURS_GRACE >= a_now)) {
				const float held = *std::exchange(entry.held, std::nullopt);
				entry.unacked = 0.0f;
				if (held > a_health + SLACK) {
					entry.dropSince.reset();
					return held - a_health;
				}
			}
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
				const float drop = *entry.health - a_health;
				const float take = Unknown(entry) ? drop : (std::min)(drop, entry.dealt);
				entry.dealt = Unknown(entry) ? 0.0f : entry.dealt - take;
				entry.ours += take;
				entry.unacked += take;
				*entry.health -= take;
				if (drop - take <= SLACK) {
					entry.health = a_health;
					entry.dropSince.reset();
					return 0.0f;
				}
				// More went than our hits said they took. The owner's blow's event can come a frame after its damage, as ours
				// can: judged once it has stood, given back if one landed around it, else ours all the same.
				if (a_now - *entry.dropSince < JUDGE_AFTER) {
					return 0.0f;
				}
				if (!OwnersAround(entry)) {
					entry.ours += *entry.health - a_health;
					entry.unacked += *entry.health - a_health;
					entry.health = a_health;
					entry.dropSince.reset();
					return 0.0f;
				}
			}
			if (a_now - *entry.dropSince < JUDGE_AFTER) {
				return 0.0f;
			}
			entry.dropSince.reset();
			return *entry.health - a_health;
		}

		// Another game's report would set the copy from a_before to a_health now: the health to set it to. What a hit of ours
		// took off it since it was last checked (the report came between the blow and the next frame) is ours all the same.
		//
		// On a copy (a_copy) a report that would raise it within OURS_GRACE of a hit of ours may be one the owner's game sent
		// before our hits reached it: one the server passed on just before our hit's claim took the NPC over, which that game
		// then never takes our damage off (no longer its to deal). Set, it healed our hit back, and our game, running the NPC
		// from then on, told everyone the healed health. So such a raise is set only for what it has beyond what our hits took
		// that may still be on its way (more than that is a real heal in the owner's game); the rest is
		// held, and the copy goes to the report once our hits are over (Check) unless a newer report comes first, as the
		// owner's does once it took our damage off. A report that lowers the copy is set at once.
		float Heard(std::uint32_t a_id, float a_before, float a_health, Clock::time_point a_now, bool a_copy = true)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				Set(a_id, a_health);
				return a_health;
			}
			auto&      entry = it->second;
			const bool lately = entry.ourHit && *entry.ourHit + OURS_GRACE >= a_now;
			if (entry.health && lately && a_before < *entry.health - SLACK) {
				// With a blow left to the owner around, only what our hits said they took is ours (as in Check).
				const float drop = *entry.health - a_before;
				const bool  owners = entry.ownersBlow && *entry.ownersBlow + OURS_GRACE >= a_now;
				const float take = Unknown(entry) || !owners ? drop : (std::min)(drop, entry.dealt);
				entry.dealt = Unknown(entry) ? 0.0f : (std::max)(entry.dealt - take, 0.0f);
				entry.ours += take;
				entry.unacked += take;
			}
			entry.held.reset();
			float health = a_health;
			if (!lately || !a_copy) {
				entry.unacked = 0.0f;
			} else if (a_health > a_before + SLACK) {
				health = (std::max)(a_before, a_health - entry.unacked);
				if (health < a_health - SLACK) {
					entry.held = a_health;
				}
				// Unless it healed, no more of ours than the raise is still on its way: the rest the owner's game took. Else,
				// under a sustained burst every report raises the copy, and what is held grows to the whole burst's damage.
				entry.unacked = (std::min)(entry.unacked, a_health - a_before);
			} else {
				entry.unacked = 0.0f;
			}
			entry.health = health;
			entry.dropSince.reset();
			return health;
		}

		// Whether a report is held for the copy (Heard), to be set once our hits on it are over.
		[[nodiscard]] bool Holds(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() && it->second.held.has_value();
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
			std::optional<Clock::time_point> ownersBlow;   // the last blow of ours left to the NPC's own game
			float                            dealt = 0.0f;     // what our hits since ourHit's grace began said they took, not yet booked
			bool                             unknown = false;  // ... and one of them said nothing
			bool                             figured = false;  // ... and one of them said what it took
			float                            unacked = 0.0f;  // taken by hits of ours that the owner's reports may not have yet
			std::optional<float>             held;  // a report's health not set yet (Heard)
		};

		// Our hits around the drop said nothing of what they took: all of it is ours. Not when one of them did: a hit event
		// with no figure beside one with (a legendary's effect riding along with the shot, a blocked blow) adds nothing, so
		// one of those in a burst no longer takes every burn tick near the player's shots for theirs.
		static bool Unknown(const Entry& a_entry) { return a_entry.unknown && !a_entry.figured; }

		// A blow left to the owner landed around the drop being judged (from OURS_GRACE before it on).
		static bool OwnersAround(const Entry& a_entry) { return a_entry.ownersBlow && a_entry.dropSince && *a_entry.ownersBlow + OURS_GRACE >= *a_entry.dropSince; }

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
