#pragma once

#include <limits>

// The time step a friend's stand-in (and a mirrored NPC) plays its animation by.
//
// A stand-in is placed on real time: its friend's path is walked at the wall clock's pace (FrameClock.h), so it covers
// the ground its friend really covers. Its legs are played by its behaviour graph, which the engine steps by the world's
// frame time: BSTimer's delta, the real frame time times the game's time multiplier (the engine's per-actor job hands
// that to Actor::UpdateAnimation, vtable slot 0x9F). While our game alone runs slowed (Jet at about 0.3, VATS without
// real time at 0.04) the legs cycled at that fraction of their pace while the body moved at full speed: it glided.
//
// So its graph is stepped by the frame time scaled by k = (the rate its friend's game ran at) / (the rate ours runs at),
// a rate being game seconds per real second. Ours is delta / realTimeDelta. Its friend's is the group's VATS slow motion
// while that is in effect (VatsTime::FriendsRate, from the table of kTimeScales): games all slowed together give k = 1,
// as they should, since the friend's states were sent at that same slowed pace. It is 1 else, so a friend who alone
// runs slowed (their Jet) still has legs a little fast for the ground; that is the other side of this and not covered.
namespace StandInTime
{
	inline constexpr float MAX_STEP = 0.25f;  // seconds: a scaled step is never longer (EngineClock::MAX_FRAME)
	inline constexpr float SAME = 0.001f;     // a k this close to 1 is 1: the step is left exactly as the engine made it

	constexpr bool Usable(float a_value)
	{
		return a_value > 0.0f && a_value <= std::numeric_limits<float>::max();  // false for 0, negatives, infinity, NaN
	}

	// k: how much faster than the world's step the stand-in's graph is stepped. a_worldDelta and a_realDelta are BSTimer's
	// delta and realTimeDelta for the frame; a_senderRate the rate its friend's game ran at (1: full speed). 1 whenever
	// one of them is not a usable time: a paused game (delta 0) stays paused.
	constexpr float Scale(float a_worldDelta, float a_realDelta, float a_senderRate = 1.0f)
	{
		if (!Usable(a_worldDelta) || !Usable(a_realDelta) || !Usable(a_senderRate)) {
			return 1.0f;
		}
		const float k = a_senderRate * a_realDelta / a_worldDelta;
		if (!Usable(k) || (k > 1.0f - SAME && k < 1.0f + SAME)) {
			return 1.0f;
		}
		return k;
	}

	// The step to hand the stand-in's animation update in place of the engine's a_delta. A step that is not a usable
	// time (0: nothing to play) is left as it is, and a scaled one is held to MAX_STEP, or to the engine's own step
	// when that is already longer, so it is never made longer than that by the scaling.
	constexpr float Step(float a_delta, float a_worldDelta, float a_realDelta, float a_senderRate = 1.0f)
	{
		if (!Usable(a_delta)) {
			return a_delta;
		}
		const float k = Scale(a_worldDelta, a_realDelta, a_senderRate);
		if (k == 1.0f) {
			return a_delta;
		}
		const float scaled = a_delta * k;
		const float cap = a_delta > MAX_STEP ? a_delta : MAX_STEP;
		if (!Usable(scaled)) {
			return a_delta;
		}
		return scaled < cap ? scaled : cap;
	}
}
