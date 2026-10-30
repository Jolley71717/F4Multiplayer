#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// A friend's hit on their copy of an NPC our game runs comes as the damage it did (kActorDamaged: a fraction of the
// NPC's maximum), for our game to take off the NPC (WorldSync::ApplyRemoteDamage). Up to 0.9.2 what could not be taken
// off at once was dropped: our game on a loading screen or in the opening, the NPC not loaded here yet (the friend has
// its cell loaded, ours not), or the server's word that we run it not here yet (a takeover's claim leaves a frame
// ahead of the hit's damage; after a load our table is empty until the owners come again). The friend's copy had lost
// the health, our NPC had not, and our next health report (HealthShare) set the copy back up: as the friend saw it,
// their shot healed the NPC, and only the game that ran it could hurt it.
//
// So such damage is held, each hit on its own (it is numbered, and answered on its own: Protocol::DamageAnswer), and
// taken off as soon as it can be: every frame while in the world, before the NPC's health is shared, so the first
// reading after a load has it. Held for up to HOLD of time in the world (a loading screen's length doesn't count: the
// friend's hits keep landing on their copy meanwhile), then refused as held too long. What can never be taken off (no
// such actor here, the NPC dead here or dead for the session, another game running it) is refused at once. Every
// answer goes back to the friend whose hit it was (Answer), who gives their copy the health back on a refusal; up to
// 0.10.0 a drop here was a log line, and the friend's copy learnt of it as a heal. Pure, so it can be tested without
// the game.
namespace DamageHold
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration HOLD = std::chrono::seconds{ 15 };
	// Hits held at once (a guard: a flood of damage for NPCs we never load). Each friend's copy sends at most ten a second
	// (CopyHealth::SEND_EVERY), so a HOLD's worth from several friends on several NPCs fits with room to spare.
	constexpr std::size_t MAX_HELD = 2048;

	// What our game finds of the NPC the damage names, as WorldSync reads it. Nothing of the actor is read out of the
	// world (a_inWorld false: only whether the id names anything of ours).
	struct Facts
	{
		bool          known = false;       // the id names an actor every game has, or a runtime NPC of ours (a spawn)
		bool          inWorld = false;     // our game is in the world, out of the opening (WorldSync::CanApply)
		bool          deadHere = false;    // dead in our game
		bool          ledgerDead = false;  // dead for the session, as our game knows it (SessionDeaths)
		std::uint32_t local = 0;           // our player's id in the session (0: none)
		std::uint32_t owner = 0;           // who the server says runs the NPC, as our game last heard (0: nobody, or not yet)
		bool          loaded = false;      // loaded here with 3D, in a cell
		bool          hasHealth = false;   // its maximum health is above 0
	};

	enum class Verdict : std::uint8_t
	{
		kApply,
		kHold,
		kDrop,
	};

	// Why the damage was applied, held or dropped.
	enum class Why : std::uint8_t
	{
		kApplied,
		kNotInWorld,  // held: a loading screen, or the opening
		kNoOwnerYet,  // held: the server hasn't said who runs it (our claim's answer, or the owners after a load, on their way)
		kNotLoaded,   // held: not loaded here yet
		kNoActor,     // dropped: no such actor here
		kDeadHere,    // dropped: dead in our game
		kLedgerDead,  // dropped: dead for the session
		kNotOurs,     // dropped: another game runs it, and takes it off there
		kNoHealth,    // dropped: no maximum health to take a fraction of
		kExpired,     // dropped: held for HOLD without a chance to apply it
	};

	struct Decision
	{
		Verdict verdict = Verdict::kDrop;
		Why     why = Why::kNoActor;
	};

	// What to do with a friend's damage on the NPC now. What can't be applied yet is held; what can never be is dropped.
	constexpr Decision Decide(const Facts& a_facts)
	{
		if (!a_facts.known) {
			return { Verdict::kDrop, Why::kNoActor };
		}
		if (!a_facts.inWorld) {
			return { Verdict::kHold, Why::kNotInWorld };
		}
		if (a_facts.deadHere) {
			return { Verdict::kDrop, Why::kDeadHere };
		}
		if (a_facts.ledgerDead) {
			return { Verdict::kDrop, Why::kLedgerDead };
		}
		if (a_facts.local == 0 || (a_facts.owner != 0 && a_facts.owner != a_facts.local)) {
			return { Verdict::kDrop, Why::kNotOurs };
		}
		if (a_facts.owner == 0) {
			return { Verdict::kHold, Why::kNoOwnerYet };
		}
		if (!a_facts.loaded) {
			return { Verdict::kHold, Why::kNotLoaded };
		}
		if (!a_facts.hasHealth) {
			return { Verdict::kDrop, Why::kNoHealth };
		}
		return { Verdict::kApply, Why::kApplied };
	}

	constexpr const char* Name(Why a_why)
	{
		switch (a_why) {
		case Why::kApplied:
			return "applied";
		case Why::kNotInWorld:
			return "our game is not in the world";
		case Why::kNoOwnerYet:
			return "the server hasn't said who runs it yet";
		case Why::kNotLoaded:
			return "it isn't loaded here";
		case Why::kNoActor:
			return "no such actor here";
		case Why::kDeadHere:
			return "it is dead here";
		case Why::kLedgerDead:
			return "the session has it dead";
		case Why::kNotOurs:
			return "another game runs it";
		case Why::kNoHealth:
			return "it has no health to take from";
		case Why::kExpired:
			return "held too long";
		}
		return "?";
	}

	// The answer the friend whose hit it was is sent (Protocol::DamageAnswer::Why). A hold's reason answers nothing
	// while it holds; at the end of the hold it is "held too long".
	constexpr Protocol::DamageAnswer::Why Answer(Why a_why)
	{
		using Out = Protocol::DamageAnswer::Why;
		switch (a_why) {
		case Why::kApplied:
			return Out::kApplied;
		case Why::kNotInWorld:
		case Why::kNoOwnerYet:
		case Why::kNotLoaded:
		case Why::kExpired:
			return Out::kHeldTooLong;
		case Why::kNoActor:
			return Out::kNoActorThere;
		case Why::kDeadHere:
		case Why::kLedgerDead:
			return Out::kDeadThere;
		case Why::kNotOurs:
			return Out::kNotTheRunner;
		case Why::kNoHealth:
			return Out::kNoHealthThere;
		}
		return Out::kHeldTooLong;
	}

	// One friend's hit, held.
	struct Held
	{
		std::uint32_t     id = 0;
		float             fraction = 0.0f;  // of the NPC's maximum
		Why               why = Why::kNotInWorld;  // why it was last held
		Clock::time_point since{};                 // held since (restarted when the world comes back: BackInWorld)
		std::uint32_t     seq = 0;                 // the hit's number (ActorHealth::seq; 0: unnumbered, nothing to answer)
		std::uint32_t     from = 0;                // whose hit it is
	};

	class Book
	{
	public:
		// Holds the hit. False when the book is full (dropped).
		bool Hold(const Held& a_held)
		{
			if (entries.size() >= MAX_HELD) {
				return false;
			}
			entries.push_back(a_held);
			++held;
			return true;
		}

		// Tries everything held again, oldest first: a_try(held) applies or drops it and returns true, or leaves it held
		// (it may change why) and returns false.
		template <class Try>
		void Retry(Try&& a_try)
		{
			std::erase_if(entries, [&](auto& a_entry) { return a_try(a_entry); });
		}

		// The hits held for the NPC, oldest first (none: nothing held for it).
		[[nodiscard]] std::vector<Held> HeldFor(std::uint32_t a_id) const
		{
			std::vector<Held> out;
			for (const auto& entry : entries) {
				if (entry.id == a_id) {
					out.push_back(entry);
				}
			}
			return out;
		}

		// The world came back (a loading screen ended, the opening is over): the hold starts afresh for everything held,
		// since the time out of the world doesn't count.
		void BackInWorld(Clock::time_point a_now)
		{
			for (auto& entry : entries) {
				entry.since = a_now;
			}
		}

		// Everything held for HOLD or longer, taken out: to be refused, each counted.
		std::vector<Held> Expire(Clock::time_point a_now)
		{
			std::vector<Held> out;
			std::erase_if(entries, [&](const auto& a_entry) {
				if (a_now - a_entry.since < HOLD) {
					return false;
				}
				out.push_back(a_entry);
				return true;
			});
			expired += static_cast<std::uint32_t>(out.size());
			return out;
		}

		[[nodiscard]] std::size_t   Size() const { return entries.size(); }
		[[nodiscard]] std::uint32_t HeldCount() const { return held; }     // hits ever held
		[[nodiscard]] std::uint32_t Expired() const { return expired; }  // ... of them refused as held too long
		void                        Clear() { entries.clear(); }

	private:
		std::vector<Held> entries;
		std::uint32_t     held = 0;
		std::uint32_t     expired = 0;
	};
}
