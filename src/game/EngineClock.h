#pragma once

#include <chrono>

// The time an actor's engine update is at: the last one's plus the frame time the engine hands it, kept near the wall clock.
//
// The engine moves an actor by its speed times that frame time, which is what the frame takes on screen as the engine
// measures it, and not the time between the moments its update ran: those come earlier or later in each frame with the
// work before them. A game that skips frames unevenly (one 16 ms apart, the next 46) runs its updates up to a frame's
// time off that pace. Read at the wall clock, the player's own even run is a stand and a leap every few frames, and so
// is a friend's stand-in drawn at the wall clock: friends saw both (F4Multiplayer-ktmu). On this time, the player's
// place goes evenly, and the stand-in steps as far each frame as the engine moves everything else drawn with it.
//
// Pulled toward the wall clock a little each update, so it never wanders from it, and set to it when it is more than
// SNAP off (a long hitch the engine clamps, a game slowed in VATS, the first update).
namespace EngineClock
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr float PULL = 0.02f;  // of the way to the wall clock each update: 2 ms over 100 frames 10 ms off
	inline constexpr auto  SNAP = 100ms;
	inline constexpr float MAX_FRAME = 0.25f;  // seconds: a longer frame time is not one (the first, or a stall)

	struct Timeline
	{
		Clock::time_point at{};
		bool              started = false;

		// The time the update running at a_wall is at, handed a_frameSeconds by the engine.
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
