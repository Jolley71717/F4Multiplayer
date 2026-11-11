#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

// Time as the world moves: a clock of steady_clock time points that advances by the game's own frame time (BSTimer's
// delta, the time multiplier in it), not the wall's. A frame at the group's VATS slow motion (0.3: game/VatsTime.h) or
// under Jet moves the world 0.3 of a wall second a second; in VATS with real time off, 0.04.
//
// Everything that pairs a blow with what dealt it is a window in this time, not the wall's. CopyHealth pairs a drop on
// a copy with our shot (SHOT_REACH 1 s), our hit event (OURS_GRACE 1 s) or a friend's blast around it, and WeaponFire's
// throw watch (EXPLOSION_WATCH) pairs an explosion with the throw before it; a missile, a Junk Jet round or a grenade
// flies 0.4 s of the world's time, a frag's fuse runs 3 s of it. On the wall clock, at the group's 0.3, that flight took
// 1.3 s and the fuse 10 s: outside both windows, so the drop was "no hit of ours" and given back (#234's symptom, behind
// an option), and the grenade's blast was no throw's. On this clock the windows are the world's, at any speed.
//
// Free of the game (tests/GameClockTests.cpp); game/GameClock.cpp reads the timer.
namespace GameClock
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// A frame moves the world by this much of its time at most: the engine clamps its own delta, and a stall (the first
	// frame, a loading screen) is no second of the world's.
	inline constexpr float MAX_FRAME = 0.25f;
	// Between two steps the time runs on at the last frame's rate for at most this long on the wall clock: a game that
	// posts no frame (a loading screen, a stall) moves nothing, so its time stands.
	inline constexpr auto  RUN_ON = 250ms;
	inline constexpr float MAX_RATE = 8.0f;  // game seconds per wall second, at most (a timer read wrong is not a leap)

	// Game seconds per wall second a frame ran at: the frame's delta over its real delta (BSTimer's, before the
	// multiplier). Paused (no delta) is 0; a real delta that is none, with a delta, is full speed.
	constexpr float Rate(float a_delta, float a_realDelta)
	{
		if (!(a_delta > 0.0f)) {
			return 0.0f;
		}
		if (!(a_realDelta > 0.0f)) {
			return 1.0f;
		}
		return std::clamp(a_delta / a_realDelta, 0.0f, MAX_RATE);
	}

	struct Timeline
	{
		Clock::time_point at{};        // the world's time at the last step
		Clock::time_point wall{};      // the wall clock at that step
		float             rate = 1.0f;  // Rate at that step
		std::uint64_t     mark = 0;    // the timer's mark (BSTimer::lastTime) at that step
		bool              started = false;
		// The latest time Now handed out: it never returns less (a frame whose delta is shorter than the run-on at the
		// last rate, as slow motion starts or after a hitch the engine clamps, would else put it back a little). Mutable:
		// reading the time is const; the caller holds the lock that guards the whole timeline.
		mutable Clock::time_point last{};

		// A frame the timer marked a_mark, seen at a_wall, that moved the world a_delta seconds (its real time
		// a_realDelta): the world's time now. The same mark again (our work ran twice in one of the engine's frames) steps
		// nothing. The first frame starts the time at the wall clock.
		Clock::time_point Step(Clock::time_point a_wall, std::uint64_t a_mark, float a_delta, float a_realDelta)
		{
			if (!started) {
				started = true;
				at = a_wall;
			} else if (a_mark != mark) {
				const float seconds = std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, MAX_FRAME) : 0.0f;
				at += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(seconds));
			}
			mark = a_mark;
			wall = a_wall;
			rate = Rate(a_delta, a_realDelta);
			return at;
		}

		// The world's time at a_wall: the last step's, plus the wall time since at that frame's rate, for RUN_ON at most.
		// Before the first step, the wall clock. Never earlier than the last step's (a wall time before it reads as it).
		[[nodiscard]] Clock::time_point Now(Clock::time_point a_wall) const
		{
			if (!started) {
				return a_wall;
			}
			const auto since = std::clamp(a_wall - wall, Clock::duration::zero(), std::chrono::duration_cast<Clock::duration>(RUN_ON));
			const auto computed = at + std::chrono::duration_cast<Clock::duration>(since * rate);
			last = std::max(computed, last);
			return last;
		}
	};
}

#include <string>

// The game's clock (game/GameClock.cpp; the tests don't link it): the timeline above, stepped by BSTimer each frame.
namespace GameClock
{
	// Once a frame, on the main thread, before the frame's sync work: steps the time by the timer's delta.
	void Frame();

	// The world's time now. Any thread.
	[[nodiscard]] Clock::time_point Now();

	// The last frame's rate (game seconds per wall second).
	[[nodiscard]] float RateNow();

	// Dev (timescale): the rate, how far the world's time stands behind the wall clock, and the frames stepped.
	[[nodiscard]] std::string Describe();
}
