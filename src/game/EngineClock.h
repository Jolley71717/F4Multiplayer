#pragma once

#include <chrono>
#include <cstdint>

// The time a frame of the engine's is at: the last frame's plus the frame time the engine's timer measured for it
// (BSTimer's, before the time multiplier: see FrameSeconds; FrameClock.h reads it), kept near the wall clock.
//
// The engine moves everything by its speed times that frame time. That is not the time between the moments our work
// runs in two frames: those come earlier or later with the work before them, and a game that skips frames unevenly
// (one 16 ms apart, the next 46) runs it up to a frame off the engine's pace. Stamped with those moments, the player's
// even run went out as a stand and a leap every few frames, and friends drew it so (F4Multiplayer-ktmu). On this time
// the place goes evenly, and a stand-in placed at it steps as far each frame as the engine moves all else.
//
// Pulled toward the wall clock a little each frame, so it never wanders from it, and set to it when it is more than
// SNAP off (a long hitch the engine clamps, the first frame).
namespace EngineClock
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr float PULL = 0.02f;  // of the way to the wall clock each frame
	inline constexpr auto  SNAP = 100ms;
	inline constexpr float MAX_FRAME = 0.25f;  // seconds: a longer frame time is not one (the first, or a stall)

	// The frame time to step by, from BSTimer: its realTimeDelta, the frame's time before the game's time multiplier.
	// Its delta has the multiplier in it: under Jet (about 0.3), or in VATS without real time, the clock fell behind the wall
	// clock by most of each frame and was set to it every SNAP, a stand and a leap again for friends. A real time that is
	// not one (none, or not a number) falls back to delta.
	constexpr float FrameSeconds(float a_delta, float a_realDelta)
	{
		return a_realDelta > 0.0f ? a_realDelta : a_delta;
	}

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

namespace EngineClock
{
	// A timer whose time stops for longer than this is not one we can go by (it is not running, its layout is not the
	// one we read, or nothing asked through a loading screen): the wall clock is used until it moves again.
	inline constexpr auto STOPPED = 250ms;

	// The engine's frames as its timer marks them (FrameClock.h reads BSTimer): a new frame when the timer's mark
	// changes, a_frameSeconds long. Asked from anywhere in a frame, any number of times.
	struct Frames
	{
		Timeline          timeline;
		std::uint64_t     seen = 0;       // the timer's mark at the last step
		Clock::time_point steppedAt{};    // when that was, on the wall clock
		Clock::time_point previous{};     // the frame before's time

		// The time of the frame the timer last marked.
		Clock::time_point Now(Clock::time_point a_wall, std::uint64_t a_mark, float a_frameSeconds)
		{
			if (a_mark != seen) {
				// After a stop (or a loading screen nobody asked through), the frame before is long gone: both start
				// again at the wall clock, rather than the time before the stop going out as the frame before.
				if (steppedAt == Clock::time_point{} || a_wall - steppedAt > STOPPED) {
					timeline.started = false;
				}
				seen = a_mark;
				steppedAt = a_wall;
				previous = timeline.started ? timeline.at : a_wall;
				const auto at = timeline.Advance(a_wall, a_frameSeconds);
				if (previous > at) {
					previous = at;  // set back to the wall clock: the frame before is no later than this one
				}
				return at;
			}
			if (a_wall - steppedAt > STOPPED) {
				timeline.started = false;
				return a_wall;
			}
			return timeline.at;
		}

		// The time of the frame before that one: the place our work reads the player at is the one the frame before
		// left them in, as the timer has already marked the frame it runs in.
		Clock::time_point Previous(Clock::time_point a_wall, std::uint64_t a_mark, float a_frameSeconds)
		{
			const auto now = Now(a_wall, a_mark, a_frameSeconds);
			return timeline.started && previous != Clock::time_point{} && previous <= now ? previous : now;
		}
	};
}
