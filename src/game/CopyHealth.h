#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

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
// given back before it adds up, unless it is an effect of ours (a legendary's bleed or burn put on the copy by our hit:
// OurEffect, ours for EFFECT_WINDOW while nothing else hits it). A lethal blow in one go is not given back (the
// explosions of friends' throws are passed by: BlastShield). Health a report sets is where it stands from then on.
//
// Health a copy gains with no report behind it is taken back the same way (Check returns it as less than nothing): a
// copy's health is its own game's, never ours. Each game rolls a leveled NPC on its own as it first loads, and the
// owner's roll is written onto our copy for its next load only (Rolls::Apply: an NPC the engine has made is not made
// again in place), so a copy can be legendary here and plain in the game that runs it. Its legendary mutation healed
// it to full at half health; that was taken as where it stood and never sent, so the player who hit it saw their hit
// heal it, while the NPC itself stayed at half. A copy whose maximum changes (our level set to the owner's, Rolls) keeps
// the share of it that it stood at: the owner's reports are shares of the maximum too (WorldSync's kActorHealth).
//
// What our hits take off a copy is booked (TakeOurs) and sent as damage (kReportDamage), which the game that runs the NPC
// takes off its own health; that game then shares where its health stands (HealthShare). A report of the copy's health
// instead, as before protocol 40, healed off the NPC whatever its own game had taken from it since the copy last heard (a
// fire's burn, a friend's shot on its way).
//
// Each damage sent is numbered and stays in flight (Entry::inFlight) until the game that runs the NPC answers it
// (Answered: applied, or refused and why; protocol 56, docs/HEALTH-SYNC.md). A report of the NPC's health raises the
// copy only to the report less what is still in flight: the runner's word, minus our hits it can't have taken off yet.
// Up to 0.10.0 a hit was written off a second after it was sent, as if it had arrived: whenever it had in fact been
// dropped on the way (a table behind, a dead ledger out of step, a runner that couldn't apply it yet), the runner's
// next report healed it back, and the player saw their shot undone. Now a refused hit is given back at once (the
// player sees the bounce, the log says why), and one never answered is given back after LOST_AFTER, logged as lost:
// the one timer left, and it errs on the side of showing the truth.
//
// Our hits are our player's and those of the actors our game is answerable for (Judge): each hit is to count in one
// game only, the game that runs whoever struck it. Pure, so it can be tested without the game.
namespace CopyHealth
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration OURS_GRACE = std::chrono::milliseconds{ 1000 };
	constexpr Clock::duration JUDGE_AFTER = std::chrono::milliseconds{ 100 };
	// A damage sent and never answered (the runner crashed, the server restarted, an answer lost) is given back after
	// this long, as lost: the copy must not stand below the NPC for good. Longer than the runner's game can hold a hit
	// (DamageHold::HOLD of time in its world, which a loading screen of a minute doesn't count towards), so a held hit
	// is never written off before it is answered; the server answers for a runner that leaves (Server.cpp,
	// answerRoutedTo), so this is for what nothing answers.
	constexpr Clock::duration LOST_AFTER = std::chrono::minutes{ 2 };
	// What our hits took off a copy goes out at most this often for one copy (CopyWatch::Frame asks TakeOurs no more
	// often): under automatic fire one damage a frame would put hundreds in flight through a runner's loading screen
	// (DamageHold::HOLD), each to be answered.
	constexpr Clock::duration SEND_EVERY = std::chrono::milliseconds{ 100 };
	// Hits in flight on one copy (a guard: past it the oldest is given back as lost). Above what LOST_AFTER at
	// SEND_EVERY can put there (1200), so the timer, not the cap, is what gives a hit up.
	constexpr std::size_t MAX_IN_FLIGHT = 2048;
	// How long after a hit of ours that put an effect on the copy (a weapon's enchantment: a legendary's bleed, burn or
	// poison; a mod's spell) the copy's drops with no hit behind them are still ours: the effect's ticks fire no hit
	// event, and the NPC's own game never deals them (the effect is on our copy alone). Before, every tick after the
	// first second was "a drop with no hit of ours" and was given back: a Wounding or Incendiary weapon healed its own
	// damage back as the player watched. The longest of the game's own last ten seconds (Poisoner's).
	constexpr Clock::duration EFFECT_WINDOW = std::chrono::seconds{ 10 };
	constexpr float           SLACK = 0.05f;      // smaller drops are rounding
	constexpr float           RAISE_SHARE = 0.01f;  // of the maximum: a smaller gain is left until it adds up to one
	constexpr float           MIN_LEFT = 1.0f;    // a gain taken back never leaves the copy with less (it would die here)
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
		bool enchantment = false;   // ... and the source is a weapon's enchantment (an ENCH record: a legendary effect's,
		                            // a mod's), never what a throw leaves burning (a hazard's spell, a SPEL record)
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
	// a weapon in hand that is no throwable is ours whatever else the event reads, and so is a weapon's enchantment's
	// (a legendary's burn or bleed riding along with the shot, read as a spell with no weapon: its record is an ENCH,
	// which no throw's fire ever is), and the burn a Molotov's fire leaves (a hazard's SPEL) is the owner's for
	// WeaponFire::THREW_LATELY after the watch. Before, a Wounding or Incendiary hit within twenty seconds of the
	// player's own Molotov was left to the owner's game, which never dealt it.
	constexpr bool OwnersToDeal(const Blow& a_blow)
	{
		if (a_blow.explosion) {
			return a_blow.blastThere;
		}
		if (a_blow.inHand || a_blow.enchantment) {
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

	// A damage of ours sent to the NPC's game, until it is answered.
	struct Sent
	{
		std::uint32_t seq = 0;
		float         amount = 0.0f;  // health, as the copy reads it
	};

	// What became of a hit of ours, as its answer says (Protocol::DamageAnswer::Why, kept apart so this stays pure).
	enum class Answer : std::uint8_t
	{
		kApplied,   // the NPC's game took it off: its reports have it from now on
		kOurs,      // we run the NPC now (our hit's claim took it over): the drop on what was our copy is the truth
		kRefused,   // not taken off anywhere: the copy gets the health back
	};

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
		// Or a friend's explosion's that goes off there too (set off here for them: the hit names no actor).
		void OwnersBlow(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.ownersBlow = a_now;
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .ownersBlow = a_now };
			}
		}

		// A hit of ours on the copy now that put an effect on it (an enchantment's, a spell's: its ticks follow with no
		// hit event, ours for EFFECT_WINDOW). Called beside OurHit for the same hit.
		void OurEffect(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.effect = a_now;
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .effect = a_now };
			}
		}

		// A hit on the copy now that is nobody's of ours: a hazard's (a fire, gas), a friend's explosion's, another
		// game's actor's. From then on a drop with no hit behind it is no longer our effect's alone (the fire's ticks
		// fire no event either), so the effect's window closes.
		void OtherHit(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.otherHit = a_now;
			}
		}

		// The copy's health is a_health now by a report, or as first seen: where it stands.
		void Set(std::uint32_t a_id, float a_health)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.health = a_health;
				it->second.dropSince.reset();
				it->second.raiseSince.reset();
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .health = a_health };
			}
		}

		// The copy reads a_health now, of a_maximum at most (0: not known): how much to give back (0: none, or not yet),
		// or, below 0, how much of a gain no report gave it to take back. After either, Set what it reads.
		float Check(std::uint32_t a_id, float a_health, Clock::time_point a_now, float a_maximum = 0.0f)
		{
			auto it = entries.find(a_id);
			if (it == entries.end() || !it->second.health) {
				Set(a_id, a_health);
				if (const auto found = entries.find(a_id); found != entries.end() && a_maximum > 0.0f) {
					found->second.maximum = a_maximum;
				}
				return 0.0f;
			}
			auto& entry = it->second;
			// A new maximum (our level set to the owner's: Rolls) moves its health with it, the engine's reading as well as
			// where it stands: the same share of it.
			if (a_maximum > 0.0f) {
				if (entry.maximum > 0.0f && std::abs(a_maximum - entry.maximum) > SLACK) {
					const float scale = a_maximum / entry.maximum;
					*entry.health *= scale;
					for (auto& flight : entry.inFlight) {
						flight.amount *= scale;  // what is on its way is a share of the maximum too, and so is what is owed
					}
					entry.ours *= scale;
					entry.lost *= scale;
					entry.parked *= scale;
					if (entry.lastReport) {
						*entry.lastReport *= scale;
					}
				}
				entry.maximum = a_maximum;
			}
			// A drop a report healed back before any hit of ours was booked (Heard): a hit of ours that comes after all (its
			// event a frame behind the damage) means what the report healed back was ours; none within JUDGE_AFTER, and the
			// report's heal stands.
			if (entry.reportedDrop) {
				const auto [drop, at] = *entry.reportedDrop;
				// Only a hit whose event came within JUDGE_AFTER of the report: Check may not have run since (the copy had no
				// 3D), and a hit long after is nothing of the drop the report healed back.
				if (entry.ourHit && *entry.ourHit >= at - OURS_GRACE && *entry.ourHit <= at + JUDGE_AFTER) {
					const float take = Unknown(entry) ? drop : (std::min)(drop, entry.dealt);
					entry.dealt = Unknown(entry) ? 0.0f : (std::max)(entry.dealt - take, 0.0f);
					entry.ours += take;
					entry.reportedDrop.reset();
				} else if (a_now - at >= JUDGE_AFTER) {
					entry.reportedDrop.reset();
				}
			}
			// A gain no report gave it (a report Sets where it stands first): the copy's own, of something in our world
			// alone: a legendary mutation of a copy that is legendary only here (the owner's roll is ours from its next
			// load on), a heal no other game saw. Taken back once it has stood JUDGE_AFTER (as a drop is judged; and a new
			// maximum, which moves the health with it above, can come a frame after the health), all of it but MIN_LEFT;
			// what follows comes from the owner's reports. A small gain is not followed: one that creeps up a little at a
			// time is taken back once it adds up to RAISE_SHARE of the maximum.
			if (a_health > *entry.health + RaiseSlack(entry)) {
				entry.dropSince.reset();
				if (!entry.raiseSince) {
					entry.raiseSince = a_now;
					entry.peak = a_health;
				}
				entry.peak = (std::max)(entry.peak, a_health);
				if (a_now - *entry.raiseSince < JUDGE_AFTER) {
					return 0.0f;
				}
				entry.raiseSince.reset();
				// What it fell from the most it read, with a hit of ours around (or an effect of ours still on it: OursLately),
				// is ours as any drop is (TakeOurs): the hit landed on the gained health. Else a shot in the tenth of a second
				// after the mutation was taken back with the heal, and never reached the NPC's game.
				if (const float fell = entry.peak - a_health; fell > SLACK && OursLately(entry, a_now)) {
					const float take = Unknown(entry) ? fell : (std::min)(fell, entry.dealt);
					entry.dealt = Unknown(entry) ? 0.0f : entry.dealt - take;
					entry.ours += take;
					*entry.health -= take;
				}
				const float to = (std::max)(*entry.health, MIN_LEFT);
				if (to >= a_health - SLACK) {
					entry.health = a_health;
					return 0.0f;
				}
				return to - a_health;
			}
			entry.raiseSince.reset();
			if (a_health >= *entry.health - SLACK) {
				entry.health = (std::min)(*entry.health, a_health);
				entry.dropSince.reset();
				return 0.0f;
			}
			if (!entry.dropSince) {
				entry.dropSince = a_now;
			}
			// A hit of ours around the drop, or an effect of ours still on it: the copy is ours to hurt, and what it took goes
			// to the NPC's game (TakeOurs).
			if (OursLately(entry, *entry.dropSince)) {
				const float drop = *entry.health - a_health;
				const float take = Unknown(entry) ? drop : (std::min)(drop, entry.dealt);
				entry.dealt = Unknown(entry) ? 0.0f : entry.dealt - take;
				entry.ours += take;
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
		// On a copy (a_copy) the report is the runner's word, which can't have our damage still in flight (InFlight) taken
		// off yet: the copy is set to the report less what is in flight, whichever way that moves it, however long the
		// hits take (never below MIN_LEFT: the NPC's death comes as its own report). Once the runner answers (Answered),
		// its next report has the hit in it; a refusal gives the hit back, and the copy is at the report again. Up to
		// 0.10.0 the hits of the last second were taken for on their way and any older one for arrived, so a dropped hit
		// was healed back by the next report a second later.
		float Heard(std::uint32_t a_id, float a_before, float a_health, Clock::time_point a_now, bool a_copy = true)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				Set(a_id, a_health);
				return a_health;
			}
			auto&      entry = it->second;
			const bool lately = OursLately(entry, a_now);
			if (entry.health && a_before < *entry.health - SLACK) {
				const float drop = *entry.health - a_before;
				if (lately) {
					// With a blow left to the owner around, only what our hits said they took is ours (as in Check).
					const bool  owners = entry.ownersBlow && *entry.ownersBlow + OURS_GRACE >= a_now;
					const float take = Unknown(entry) || !owners ? drop : (std::min)(drop, entry.dealt);
					entry.dealt = Unknown(entry) ? 0.0f : (std::max)(entry.dealt - take, 0.0f);
					entry.ours += take;
				} else if (a_copy) {
					// No hit of ours is booked yet, but its event can come a frame behind its damage: judged in Check once
					// the event has had JUDGE_AFTER to come.
					entry.reportedDrop = { drop, a_now };
				}
			}
			float health = a_health;
			if (!a_copy) {
				// Ours to run: its health here is the truth, whatever was on its way, booked and not sent, or owed to it.
				entry.inFlight.clear();
				entry.ours = 0.0f;
				entry.parked = 0.0f;
				entry.lost = 0.0f;
				entry.lastReport.reset();
			} else {
				entry.lastReport = a_health;
				if (const float onWay = OnWay(a_id); onWay > 0.0f && a_health > MIN_LEFT) {
					// What our hits took that is still on its way (sent and unanswered, or booked this frame and not yet sent)
					// comes off the report: the runner's game hasn't taken it off yet. A refusal gives it back, up to the
					// report's figure (Refundable), so the copy never ends up above the runner's word by a refunded hit. Never
					// below MIN_LEFT: the NPC's death comes as its own report (a report at or under MIN_LEFT itself is set as
					// it comes).
					health = (std::max)(MIN_LEFT, a_health - onWay);
				}
			}
			entry.health = health;
			entry.dropSince.reset();
			entry.raiseSince.reset();  // a report is where it stands: no gain of its own is pending
			return health;
		}

		// What hits of ours took off the copy since this was last asked (nothing: none): for the game that runs it to take
		// off, numbered, and in flight until Answered. CopyWatch asks at most every SEND_EVERY for one copy.
		std::optional<Sent> TakeOurs(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end() || it->second.ours <= 0.0f) {
				return std::nullopt;
			}
			auto&      entry = it->second;
			const Sent sent{ ++lastSeq, std::exchange(entry.ours, 0.0f) };
			if (entry.inFlight.size() >= MAX_IN_FLIGHT) {
				entry.lost += entry.inFlight.front().amount;  // the oldest is given up on (TakeLost)
				entry.inFlight.erase(entry.inFlight.begin());
			}
			entry.inFlight.push_back({ sent.seq, sent.amount, a_now });
			return sent;
		}

		// What our hits took that can't be sent (the copy's maximum reads 0): forgotten, with nothing in flight for it.
		void Discard(std::uint32_t a_id)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.ours = 0.0f;
			}
		}

		// The answer to a damage of ours (its seq) came: what to give the copy back (0: nothing, it was taken off, or it is
		// ours to run now). An answer to nothing we sent (answered already, or lost) gives nothing back. Never more than
		// takes the copy to the runner's last word (Refundable).
		float Answered(std::uint32_t a_id, std::uint32_t a_seq, Answer a_answer)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return 0.0f;
			}
			auto&      flights = it->second.inFlight;
			const auto flight = std::ranges::find(flights, a_seq, &Flight::seq);
			if (flight == flights.end()) {
				return 0.0f;
			}
			const float amount = flight->amount;
			flights.erase(flight);
			++answered;
			if (a_answer != Answer::kRefused) {
				return 0.0f;
			}
			++refused;
			// What the runner's last word doesn't cover now (the copy reads above it: a gain of its own being judged) is
			// owed, and comes once the copy is back at the word (TakeParked).
			const float now = Refundable(it->second, amount);
			it->second.parked += amount - now;
			return now;
		}

		// What is on its way for the copy: sent and not answered yet.
		[[nodiscard]] float InFlight(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return 0.0f;
			}
			float sum = 0.0f;
			for (const auto& flight : it->second.inFlight) {
				sum += flight.amount;
			}
			return sum;
		}

		// ... and what our hits took that is booked but not sent yet, with it: everything a report can't have yet.
		[[nodiscard]] float OnWay(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it == entries.end() ? 0.0f : InFlight(a_id) + it->second.ours;
		}

		// Health the copy is owed from a refusal it couldn't take when the answer came (no 3D that frame): given back with
		// TakeParked once it is seen again.
		void Park(std::uint32_t a_id, float a_amount)
		{
			if (const auto it = entries.find(a_id); it != entries.end() && a_amount > 0.0f) {
				it->second.parked += a_amount;
			}
		}

		// What was parked for the copy (Park), to be given back now (0: nothing): no more than takes the copy to the
		// runner's last word, which may have come since.
		float TakeParked(std::uint32_t a_id)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end() || it->second.parked <= 0.0f) {
				return 0.0f;
			}
			const float now = Refundable(it->second, it->second.parked);
			it->second.parked -= now;  // the rest stays owed
			return now;
		}

		// What was sent for the copy and never answered in LOST_AFTER (or pushed out by MAX_IN_FLIGHT): given up on, and
		// to be given back to the copy now (0: nothing).
		float TakeLost(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return 0.0f;
			}
			auto& entry = it->second;
			std::erase_if(entry.inFlight, [&](const Flight& a_flight) {
				if (a_now - a_flight.since < LOST_AFTER) {
					return false;
				}
				entry.lost += a_flight.amount;
				return true;
			});
			const float lost = Refundable(entry, std::exchange(entry.lost, 0.0f));
			lostTotal += lost;
			return lost;
		}

		[[nodiscard]] std::uint32_t AnsweredCount() const { return answered; }
		[[nodiscard]] std::uint32_t Refused() const { return refused; }
		[[nodiscard]] float         Lost() const { return lostTotal; }

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
		struct Flight
		{
			std::uint32_t     seq = 0;
			float             amount = 0.0f;
			Clock::time_point since{};
		};

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
			std::vector<Flight>              inFlight;         // sent (TakeOurs) and not answered yet (Answered)
			float                            lost = 0.0f;      // given up on, not yet given back (TakeLost)
			float                            parked = 0.0f;    // refused and owed to the copy, not yet given back (Park, TakeParked)
			std::optional<float>             lastReport;       // the runner's last word (Heard, on a copy): no refund takes the copy past it
			std::optional<std::pair<float, Clock::time_point>> reportedDrop;  // a drop a report found with no hit of ours booked yet (Heard): ours if a hit's event follows within JUDGE_AFTER
			std::optional<Clock::time_point> effect;      // the last hit of ours that put an effect on the copy (OurEffect)
			std::optional<Clock::time_point> otherHit;    // the last hit on the copy that was nobody's of ours (OtherHit)
			std::optional<Clock::time_point> raiseSince;  // it has read more than it stands at since (a gain of its own)
			float                            peak = 0.0f;     // ... and the most it has read since then
			float                            maximum = 0.0f;  // its maximum as last read (0: not known)
		};

		// A gain smaller than this is not taken back (yet).
		static float RaiseSlack(const Entry& a_entry) { return (std::max)(SLACK, RAISE_SHARE * a_entry.maximum); }

		// How much of a_amount a refund may give the copy: not past the runner's last word, where it stands (a report
		// under MIN_LEFT, or more on its way than it had, left the copy above the word less its hits).
		static float Refundable(const Entry& a_entry, float a_amount)
		{
			if (!a_entry.lastReport || !a_entry.health) {
				return a_amount;
			}
			return std::clamp(a_amount, 0.0f, (std::max)(0.0f, *a_entry.lastReport - *a_entry.health));
		}

		// Our hits around the drop said nothing of what they took: all of it is ours. Not when one of them did: a hit event
		// with no figure beside one with (a legendary's effect riding along with the shot, a blocked blow) adds nothing, so
		// one of those in a burst no longer takes every burn tick near the player's shots for theirs.
		static bool Unknown(const Entry& a_entry) { return a_entry.unknown && !a_entry.figured; }

		// Whether the copy is ours to hurt at a_at: a hit of ours within OURS_GRACE before it, or an effect of ours put on
		// it within EFFECT_WINDOW before it with no other's hit on it since (a fire's ticks fire no event either, so once
		// something else hit the copy its drops are no longer the effect's alone: given back, as before).
		static bool OursLately(const Entry& a_entry, Clock::time_point a_at)
		{
			if (a_entry.ourHit && *a_entry.ourHit + OURS_GRACE >= a_at) {
				return true;
			}
			return a_entry.effect && *a_entry.effect + EFFECT_WINDOW >= a_at && a_at >= *a_entry.effect && !(a_entry.otherHit && *a_entry.otherHit > *a_entry.effect);
		}

		// A blow left to the owner landed around the drop being judged (from OURS_GRACE before it on).
		static bool OwnersAround(const Entry& a_entry) { return a_entry.ownersBlow && a_entry.dropSince && *a_entry.ownersBlow + OURS_GRACE >= *a_entry.dropSince; }

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint32_t                            lastSeq = 0;  // the number of the last damage sent (TakeOurs), over every copy
		std::uint32_t                            answered = 0;
		std::uint32_t                            refused = 0;
		float                                    lostTotal = 0.0f;
	};
}
