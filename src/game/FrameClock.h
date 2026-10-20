#pragma once

#include <chrono>
#include <cstdint>

// The engine's frame on its own pace (EngineClock.h), from its frame timer (BSTimer): what our states are stamped with,
// and where a friend's stand-in is placed on its path.
namespace FrameClock
{
	using Clock = std::chrono::steady_clock;

	// The time of the frame the engine's timer last timed. Advances once a frame, when the timer's time changes, by the
	// frame time it measured. The wall clock instead while the timer is unread or stops: any thread.
	[[nodiscard]] Clock::time_point Now();

	// The time of the frame before that one: the last frame's place may be the one before the timer's last step.
	[[nodiscard]] Clock::time_point Previous();

	// Dev (trace): the timer as it is now.
	struct Raw
	{
		float         delta = 0.0f;      // the frame time everything was moved by (seconds)
		float         realDelta = 0.0f;  // the same, before the game's time multiplier
		std::uint64_t lastTime = 0;      // when it timed the frame
		bool          read = false;
	};
	[[nodiscard]] Raw Read();

	// Dev (netsim pace): what our states are stamped with: the moment our work runs (kWall, as shipped: the rig measured
	// the others as below), the frame the
	// timer last timed (kFrame: much worse, as the player's place read by our work is the frame before's), or the one before
	// it (kPrevious). Stand-ins go by the wall clock with kWall,
	// by the frame clock otherwise.
	enum class Pace : std::uint8_t
	{
		kWall,
		kFrame,
		kPrevious,
	};
	void SetPace(Pace a_pace);
	[[nodiscard]] Pace GetPace();
	[[nodiscard]] const char* PaceName(Pace a_pace);

	// The time to stamp our state sampled at a_now with, by the pace.
	[[nodiscard]] Clock::time_point StampAt(Clock::time_point a_now);
	// The time to place a stand-in at, in its update running at a_now.
	[[nodiscard]] Clock::time_point PlaceAt(Clock::time_point a_now);
}
