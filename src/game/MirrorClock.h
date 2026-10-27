#pragma once

#include "game/Prediction.h"

#include <chrono>
#include <cstdint>

// When a mirrored NPC's state is to be drawn: the stamp its snapshot takes (NpcSync::ApplyStates). A state that says
// when its owner's game sampled it (ActorState::sentMs, the owner's clock) is stamped by Prediction::SenderClock, as a
// friend's own states are: as far after the last as the owner's clock says, however late it came. So a state read in a
// burst by a slow game, one held up on the way, and an owner that sends an NPC far from everyone every other tick
// (NpcSync's slow tick) all land where they were sampled. Stamped by their arrival alone (Prediction::ArrivalClock) the
// states of an NPC whose owner halved its rate were stamped up to 130 ms early for two seconds, while that clock learned
// the new gap (its copy stood at each state and stepped on, with no state ahead to blend to), and when the rate doubled
// again 125 ms late (its copy fell that much further behind, then caught up): at the edge of FAST_RANGE from the nearest
// player, and as a fight begins and ends. A state with no sentMs (an older build's) takes the arrival clock as before.
// The owner's clock is one game's: when the NPC changes hands the timeline starts over at the new owner's first state
// (NpcSync::ApplyOwners). Kept free of the game so it can be tested.
namespace MirrorClock
{
	using Clock = std::chrono::steady_clock;

	// The owner's tick (NpcSync's STATE_INTERVAL): the arrival clock's first guess at the gap between states.
	constexpr float TICK = 0.05f;

	struct Stamps
	{
		Prediction::SenderClock  sender;
		Prediction::ArrivalClock arrivals{ .gap = TICK };

		// The stamp for a state that came at a_now, sampled at a_sentMs on its owner's clock (0: not said).
		Clock::time_point Next(Clock::time_point a_now, std::uint32_t a_sentMs)
		{
			return a_sentMs ? sender.Next(a_now, a_sentMs) : arrivals.Next(a_now);
		}

		// How much earlier the stamps given before the last Next now are (Prediction::SenderClock::steppedBack, the first
		// state having come late): the snapshots kept move back with it.
		[[nodiscard]] Clock::duration SteppedBack() const { return sender.steppedBack; }

		// Another game runs the NPC now: its states are on another clock, so the timeline starts over at its first.
		void NewOwner()
		{
			sender = {};
			arrivals = Prediction::ArrivalClock{ .gap = TICK };
		}
	};
}
