#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include "Protocol.h"

// A friend in furniture: a chair, a couch, a bench, a stool, a bed or a sleeping bag. Not a terminal, a workbench or a
// station: a stand-in snapped into a terminal that our own player then used crashed the game after our next cell change
// (the engine ran the stand-in's furniture package once it had no AI process), so a friend there is followed, standing.
// Their status names the furniture (Protocol::Seat), and their stand-in is put in the same one here (RemotePlayers'
// UpdateSeat, Puppets::Enter). Without it the stand-in followed their position into the furniture and stood on the couch
// they sat on. Power armor frames are furniture too, but never this: their own path (ArmorFrames.h) switches the stand-in
// in. Kept free of the game so it can be tested.
namespace StandInSeat
{
	using Duration = std::chrono::steady_clock::duration;

	// The engine's sit and sleep states (RE::SIT_SLEEP_STATE in 1.11.240).
	enum class SitState : std::int32_t
	{
		kNormal = 0,
		kWantToSit = 1,
		kWaitingForSitAnim = 2,
		kIsSitting = 3,
		kWantToStand = 4,
		kWantToSleep = 5,
		kWaitingForSleepAnim = 6,
		kIsSleeping = 7,
		kWantToWake = 8,
	};

	// In furniture, or on the way into it. Getting up (kWantToStand, kWantToWake) is out of it already: the stand-in gets up
	// as they start to, not once they are done.
	constexpr bool InFurniture(std::int32_t a_state)
	{
		using enum SitState;
		switch (static_cast<SitState>(a_state)) {
		case kWantToSit:
		case kWaitingForSitAnim:
		case kIsSitting:
		case kWantToSleep:
		case kWaitingForSleepAnim:
		case kIsSleeping:
			return true;
		default:
			return false;
		}
	}

	// Our own player, as Party::SampleStatus reads it.
	struct Own
	{
		std::int32_t  sitState = 0;        // DoGetSitSleepState
		bool          powerArmor = false;  // in power armor: the frame's path tells that
		bool          found = false;       // the engine names furniture our player occupies
		bool          actor = false;       // that is an actor (a vertibird's seat): nothing a stand-in can sit in here
		bool          furniture = false;   // its base is furniture (FURN; a terminal, TERM, is furniture too)
		bool          frame = false;       // a power armor frame
		bool          station = false;     // a power armor station (its workbench type): used in power armor, the frames' business
		bool          workplace = false;   // a terminal, or a workbench or crafting station (any workbench type): not a seat
		std::uint32_t ref = 0;
		std::uint32_t base = 0;
		float         x = 0, y = 0, z = 0;  // where the furniture stands
	};

	// The seat our status tells, or none.
	inline std::optional<Protocol::Seat> Tell(const Own& a_own)
	{
		if (!InFurniture(a_own.sitState) || a_own.powerArmor || !a_own.found || a_own.actor || !a_own.furniture || a_own.frame || a_own.station || a_own.workplace) {
			return std::nullopt;
		}
		// Furniture made in our game (built in a settlement) is another reference, or none, elsewhere: it goes by base and place.
		Protocol::Seat seat{ .ref = Protocol::IsShareableRef(a_own.ref) ? a_own.ref : 0, .base = a_own.base, .x = std::round(a_own.x), .y = std::round(a_own.y), .z = std::round(a_own.z) };
		return seat.Valid() ? std::optional{ seat } : std::nullopt;
	}

	// Furniture made in their game is found here by its base within this of where they said it stands: a settlement's
	// furniture is copied to the very spot (WorkshopSync), and the place is sent to the unit.
	constexpr float MATCH_RADIUS = 32.0f;

	// A reference in the hearer's world that may be the seat.
	struct Candidate
	{
		std::uint32_t id = 0;
		std::uint32_t base = 0;
		float         x = 0, y = 0, z = 0;
		bool          usable = false;  // furniture, enabled, its 3D in, in the stand-in's cell or worldspace
	};

	// Which candidate is the seat: the reference they named when every game has it (and it is still of that base), else the
	// nearest of that base within MATCH_RADIUS of where they said it stands.
	inline std::optional<std::uint32_t> Pick(const Protocol::Seat& a_seat, std::span<const Candidate> a_candidates)
	{
		if (a_seat.ref != 0) {
			for (const auto& candidate : a_candidates) {
				if (candidate.id == a_seat.ref && candidate.base == a_seat.base && candidate.usable) {
					return candidate.id;
				}
			}
		}
		std::optional<std::uint32_t> best;
		float                        bestDistance = MATCH_RADIUS;
		for (const auto& candidate : a_candidates) {
			if (candidate.base != a_seat.base || !candidate.usable) {
				continue;
			}
			const float distance = std::hypot(candidate.x - a_seat.x, candidate.y - a_seat.y, candidate.z - a_seat.z);
			if (distance <= bestDistance) {
				best = candidate.id;
				bestDistance = distance;
			}
		}
		return best;
	}

	// The friend's spot (where their states put the stand-in) is within this of the furniture, or the status and the states
	// disagree (a status from before a door or a teleport): the stand-in follows the states then. A long couch's or a
	// bed's far end is some way off its reference.
	constexpr float NEAR = 256.0f;
	// How long one way into the furniture is given to take (Puppets::Enter; the engine's own, run a frame or more later)
	// before the next is tried.
	constexpr auto ENTRY_WAIT = std::chrono::seconds(2);
	// Ways tried before the stand-in is left standing beside the furniture (Puppets::Entry: the snap, then activation).
	constexpr int MAX_TRIES = 2;

	struct Facts
	{
		bool     wanted = false;    // their status names a seat
		bool     found = false;     // ... and it is here (Pick)
		bool     near = false;      // their spot is within NEAR of it (of where they said it stands while none is found)
		bool     inArmor = false;   // the stand-in is in power armor: the frames' own
		bool     inTarget = false;  // the stand-in is in that furniture
		bool     inOther = false;   // the stand-in is in other furniture (not a seat on an actor)
		int      tries = 0;         // ways into it tried for this seat on this stand-in
		Duration sinceTried{};      // since the last
	};

	enum class Act
	{
		kFollow,  // it follows their states as ever
		kEnter,   // the next way into the furniture is tried (and it is held meanwhile)
		kHold,    // neither placed nor driven by their states: in the furniture, on its way in, or left beside it
		kStand,   // taken out of the furniture at once, standing; it follows their states again
	};

	// One step each update. Held beside it (kHold without being in it) is where it stood as they sat down: once the ways in
	// are spent, or while no such furniture is found here, rather than following them up onto the couch.
	constexpr Act Choose(const Facts& a_facts)
	{
		if (a_facts.inArmor) {
			return Act::kFollow;
		}
		if (!a_facts.wanted) {
			return a_facts.inTarget || a_facts.inOther ? Act::kStand : Act::kFollow;
		}
		if (a_facts.inTarget) {
			return Act::kHold;
		}
		if (a_facts.inOther) {
			return Act::kStand;  // out of that first; into theirs on a later step
		}
		if (!a_facts.near) {
			return Act::kFollow;
		}
		if (!a_facts.found) {
			return Act::kHold;
		}
		if (a_facts.tries == 0 || (a_facts.tries < MAX_TRIES && a_facts.sinceTried >= ENTRY_WAIT)) {
			return Act::kEnter;
		}
		return Act::kHold;
	}
}
