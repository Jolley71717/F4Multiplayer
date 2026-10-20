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

	// Dev (netsim stamp, netsim place), to measure one against another. What our states are stamped with: the moment our
	// work runs (kWall, as before), the frame the timer last marked (kFrame), or the one before it (kPrevious, as shipped).
	// The rig showed the place our work reads the player at is the one the frame before left them in: the timer has
	// already marked the frame our work runs in. Stamped with that frame, an even run read three times as uneven.
	enum class Stamp : std::uint8_t
	{
		kWall,
		kFrame,
		kPrevious,
	};
	void SetStamp(Stamp a_stamp);
	[[nodiscard]] Stamp GetStamp();
	[[nodiscard]] const char* StampName(Stamp a_stamp);

	// Where a stand-in is placed on its path in its update: at the moment the update runs (kWall, as shipped) or at the
	// time of the frame the timer last marked (kFrame).
	enum class Place : std::uint8_t
	{
		kWall,
		kFrame,
	};
	void SetPlace(Place a_place);
	[[nodiscard]] Place GetPlace();
	[[nodiscard]] const char* PlaceName(Place a_place);

	// The time to stamp our state sampled at a_now with.
	[[nodiscard]] Clock::time_point StampAt(Clock::time_point a_now);
	// The time to place a stand-in at, in its update running at a_now.
	[[nodiscard]] Clock::time_point PlaceAt(Clock::time_point a_now);
}
