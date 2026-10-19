#pragma once

#include <chrono>
#include <optional>

// The player as their engine update last left them, and the time that update was at (EngineClock.h): what our state is
// sent from. Read at the moment our frame's work runs, the place is the update's, but that moment is not its time.
namespace PlayerMotion
{
	struct Sample
	{
		std::chrono::steady_clock::time_point at;    // the update's time, on the engine's pace (EngineClock.h)
		std::chrono::steady_clock::time_point wall;  // when it ran
		RE::NiPoint3                          position;
		float                                 heading = 0.0f;
		const RE::TESObjectCELL*              cell = nullptr;
	};

	// Hooks the player's update once (main thread). Until it has run, Last is empty.
	void Install();

	// The last update's sample, if it ran within a_fresh of a_now in a_cell, the cell the player is in now.
	[[nodiscard]] std::optional<Sample> Last(std::chrono::steady_clock::time_point a_now, const RE::TESObjectCELL* a_cell, std::chrono::steady_clock::duration a_fresh);

	// Dev (netsim, for measuring): whether our states and the stand-ins' places go by the engine's pace (on, as shipped) or
	// by the moments the updates run, as they did before.
	void SetEngineTime(bool a_on);
	[[nodiscard]] bool EngineTime();
}
