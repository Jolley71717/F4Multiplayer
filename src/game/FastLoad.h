#pragma once

#include <chrono>

// Loading screens in Fallout 4 load in lockstep with frame presentation, so vsync makes
// them much slower. This turns vsync off while the loading menu is open and restores it after.
// Whether it does is bFastLoading, read as each loading screen opens (the menu can change it).
// Vsync stays off for iFastLoadingSeconds of a screen at most, and comes back sooner when the
// game is stuck under one: the decision is FastLoadCap.h, this is its hands on the renderer.
namespace FastLoad
{
	void Install();

	// Once a second, from the frame watch's thread (main.cpp): puts vsync back when a loading screen has
	// been up for its limit, or at once when a_stalled (the frame watch finds no game work running while
	// the game draws). That thread runs whatever the main thread is doing, which a stuck load holds.
	void Tick(bool a_stalled);

	// How long vsync has been off for the loading screen up now (zero: it is on). Any thread.
	[[nodiscard]] std::chrono::steady_clock::duration VsyncOffFor();
}
