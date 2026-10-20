#pragma once

#include <chrono>

// The time a frame of the engine's is at: the last frame's plus the frame time the engine's timer measured for it
// (BSTimer's delta; FrameClock.h reads it), kept near the wall clock.
//
// The engine moves everything by its speed times that frame time. That is not the time between the moments our work
// runs in two frames: those come earlier or later with the work before them, and a game that skips frames unevenly
// (one 16 ms apart, the next 46) runs it up to a frame off the engine's pace. Stamped with those moments, the player's
// even run went out as a stand and a leap every few frames, and friends drew it so (F4Multiplayer-ktmu). On this time
// the place goes evenly, and a stand-in placed at it steps as far each frame as the engine moves all else.
//
// Pulled toward the wall clock a little each frame, so it never wanders from it, and set to it when it is more than
// SNAP off (a long hitch the engine clamps, a game slowed in VATS, the first frame).
namespace EngineClock
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr float PULL = 0.02f;  // of the way to the wall clock each frame
	inline constexpr auto  SNAP = 100ms;
	inline constexpr float MAX_FRAME = 0.25f;  // seconds: a longer frame time is not one (the first, or a stall)

	struct Timeline
	{
		Clock::time_point at{};
		bool              started = false;

		// The time of a new frame, seen at a_wall, that the engine timed at a_frameSeconds.
		Clock::time_point Advance(Clock::time_point a_wall, float a_frameSeconds)
		{
			if (!started || !(a_frameSeconds >= 0.0f) || a_frameSeconds > MAX_FRAME) {
				started = true;
				at = a_wall;
				return at;
			}
			at += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_frameSeconds));
			const auto off = a_wall - at;
			if (off > SNAP || off < -SNAP) {
				at = a_wall;
			} else {
				at += std::chrono::duration_cast<Clock::duration>(off * PULL);
			}
			return at;
		}
	};
}
