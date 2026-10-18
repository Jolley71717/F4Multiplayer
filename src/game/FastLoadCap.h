#pragma once

#include <chrono>
#include <cstdint>

// How long fast loading (FastLoad.cpp) keeps the game's vsync off. A loading screen in Fallout 4 loads in
// lockstep with frame presentation, so with vsync off a load is done in a fraction of the time: bFastLoading
// turns it off as the loading screen opens and back on as it closes. A loading screen that never closed drew
// on with no cap for 12 minutes, at ~1800 fps (a host hung on a coc as a friend left: the frame watch counted
// 1.3 million frames in 735 s). So the uncapped stretch is bounded here: vsync stays off for the first
// iFastLoadingSeconds of a loading screen (every real load is done well within them; one that is not just
// finishes at the game's frame rate), then goes back while the screen stays up. A new loading screen starts
// the stretch again. It goes back at once when the frame watch finds no game work running while the game
// draws (the screen is not loading, it is stuck). The decision only, free of the game, so it is unit tested;
// FastLoad.cpp flips the renderer.
namespace FastLoadCap
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// iFastLoadingSeconds' default: a long load with every mod there is takes 10 s or so with vsync off.
	constexpr auto UNCAPPED = 20s;

	// What to do to the renderer's vsync.
	enum class Change : std::uint8_t
	{
		kNone,
		kOff,        // turn it off: a loading screen opened with fast loading on
		kOnClosed,   // put it back: the loading screen closed
		kOnLimit,    // put it back: the screen has been up for the limit
		kOnStalled,  // put it back: the frame watch finds the game stuck while the screen is up
	};

	constexpr bool RestoresVsync(Change a_change)
	{
		return a_change == Change::kOnClosed || a_change == Change::kOnLimit || a_change == Change::kOnStalled;
	}

	constexpr const char* Name(Change a_change)
	{
		switch (a_change) {
		case Change::kOff:
			return "off";
		case Change::kOnClosed:
			return "the loading screen closed";
		case Change::kOnLimit:
			return "the loading screen has been up for the limit";
		case Change::kOnStalled:
			return "no game work runs while the game draws";
		default:
			return "none";
		}
	}

	class Cap
	{
	public:
		// The loading screen opened (the menu event). a_enabled: bFastLoading, read per screen (the in-game menu
		// changes it); a_limit: iFastLoadingSeconds, likewise. A screen that opens while vsync is still off from
		// the last one (two open events, no close heard) keeps it off and starts the stretch again.
		[[nodiscard]] Change Opened(bool a_enabled, Clock::duration a_limit, Clock::time_point a_now)
		{
			if (!a_enabled) {
				return Change::kNone;
			}
			since = a_now;
			limit = a_limit;
			if (off) {
				return Change::kNone;
			}
			off = true;
			return Change::kOff;
		}

		// The loading screen closed (the menu event).
		[[nodiscard]] Change Closed()
		{
			if (!off) {
				return Change::kNone;  // on already, or never off for this screen
			}
			off = false;
			return Change::kOnClosed;
		}

		// Once a second, from a thread of ours that runs whatever the game does (the frame watch's: a stuck load
		// may hold the main thread in the engine's pump, where no frame of ours runs). a_stalled: the frame watch
		// finds no game work running while the game draws.
		[[nodiscard]] Change Tick(Clock::time_point a_now, bool a_stalled)
		{
			if (!off) {
				return Change::kNone;
			}
			if (a_stalled) {
				off = false;
				return Change::kOnStalled;
			}
			if (a_now - since >= limit) {
				off = false;
				return Change::kOnLimit;
			}
			return Change::kNone;
		}

		// Whether vsync is off now.
		[[nodiscard]] bool Off() const { return off; }

		// How long vsync has been off (none when it is on).
		[[nodiscard]] Clock::duration OffFor(Clock::time_point a_now) const { return off ? a_now - since : Clock::duration{}; }

	private:
		bool              off = false;
		Clock::time_point since{};  // when the loading screen opened (the latest open event)
		Clock::duration   limit{};  // this screen's iFastLoadingSeconds
	};
}
