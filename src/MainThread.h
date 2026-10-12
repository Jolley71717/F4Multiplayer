#pragma once

#include <functional>

// Game work runs on the game's main thread, between frames (see main.cpp).
namespace MainThread
{
	// Runs a_fn on the main thread before the next frame's game work.
	void Post(std::function<void()> a_fn);
}
