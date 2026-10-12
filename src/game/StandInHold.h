#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

// Whether a stand-in stays where it is put. Kept free of the game so it can be tested.
namespace StandInHold
{
	// The turn from one heading to another in radians, the short way round, in [-pi, pi]: positive is
	// clockwise. A blended heading can run past a full turn or below zero: from 6.27 to 0.01 is a turn of
	// +0.02, not -6.26.
	inline float TurnBetween(float a_from, float a_to)
	{
		constexpr float turn = 2.0f * std::numbers::pi_v<float>;
		float           delta = std::fmod(a_to - a_from, turn);
		if (delta > turn * 0.5f) {
			delta -= turn;
		} else if (delta < -turn * 0.5f) {
			delta += turn;
		}
		return delta;
	}

	// The angle between two headings in radians, the short way round, in [0, pi].
	inline float AngleBetween(float a_from, float a_to)
	{
		return std::abs(TurnBetween(a_from, a_to));
	}

	// Counts the frames a stand-in that should be standing still is found away from where it was put the
	// frame before. Something else in the engine is moving it then: a power armor frame copy a few units
	// off pulled its stand-in back toward it on alternate frames, and a friend saw it flip between two spots.
	// Only the ground plane is compared; the engine settling the feet on uneven ground is not a move.
	struct Check
	{
		static constexpr float STILL = 0.5f;  // the target moved less than this since the last frame: standing still
		static constexpr float OFF = 2.0f;    // further than this from where it was put: moved by something else

		std::uint32_t frames = 0;  // frames it stood still
		std::uint32_t off = 0;     // of those, frames it was found away from its spot
		float         worst = 0.0f;

		// a_targetMoved: how far its target moved since the last frame; a_speed: the friend's own speed in
		// the state it was put by; a_off: how far the stand-in is from where it was put last frame. Only a
		// friend who says they stand still counts: one whose next state is late is held still by us while
		// still moving in their own game, and their stand-in's run cycle carries it on. True when this
		// frame counts as off.
		constexpr bool Add(float a_targetMoved, float a_speed, float a_off)
		{
			if (a_targetMoved >= STILL || a_speed != 0.0f) {
				return false;
			}
			++frames;
			if (a_off <= OFF) {
				return false;
			}
			++off;
			worst = (std::max)(worst, a_off);
			return true;
		}
	};
}
