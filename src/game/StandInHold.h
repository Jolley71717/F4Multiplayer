#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

// Whether a stand-in stays where it is put. Kept free of the game so it can be tested.
namespace StandInHold
{
	// The angle between two headings in radians, the short way round. A blended heading can run past a
	// full turn or below zero: 6.27 against 0.01 is a turn of 0.02, not 6.26.
	inline float AngleBetween(float a_from, float a_to)
	{
		constexpr float turn = 2.0f * std::numbers::pi_v<float>;
		const float     apart = std::fmod(std::abs(a_to - a_from), turn);
		return apart > turn * 0.5f ? turn - apart : apart;
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

		// a_targetMoved: how far its target moved since the last frame; a_off: how far the stand-in is from
		// where it was put last frame. True when this frame counts as off.
		constexpr bool Add(float a_targetMoved, float a_off)
		{
			if (a_targetMoved >= STILL) {
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
