#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Whether our game is stopped: a menu that pauses it is up (the pause menu, the Pip-Boy, a container, a terminal, a
// tutorial's message box), so our world, its scripts and the NPCs our game runs stand still. Asked in one place (Now)
// rather than of the UI by each part that cares, so a menu that one day runs in real time is told apart here only.
// How long the current stop has lasted (Clock) is kept free of the game, so it is tested.
namespace GameStop
{
	// How long our game has been stopped, from the frame the stop began. Advanced once a frame.
	class Clock
	{
	public:
		using Time = std::chrono::steady_clock::time_point;
		using Duration = std::chrono::steady_clock::duration;

		// a_stopped: the game is stopped this frame (Now). How long the stop has lasted: zero for its first frame, and
		// while the game runs.
		Duration Advance(Time a_now, bool a_stopped)
		{
			if (!a_stopped) {
				since.reset();
				return {};
			}
			if (!since) {
				since = a_now;
			}
			return a_now - *since;
		}

		// A load (or a loading screen): whatever stop was counting is over; one after it counts afresh.
		void Reset() { since.reset(); }

	private:
		std::optional<Time> since;  // the frame the stop began
	};

	// The game's side (GameStop.cpp).

	// Whether a menu has our game stopped now (UI::menuMode), or the game has no UI yet (nothing of ours runs then).
	[[nodiscard]] bool Now();

	// The UI's menu count, for logs and dev commands; none without the UI. GameStop.cpp is the plugin's one reader of
	// it (a test checks the source): once a menu runs in real time, a count above 0 no longer means a stopped game
	// (F4Multiplayer-tnl5).
	[[nodiscard]] std::optional<std::uint32_t> MenuMode();
}
