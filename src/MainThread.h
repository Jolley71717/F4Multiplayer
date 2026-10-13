#pragma once

#include <functional>

// Game work runs on the game's main thread, between frames (see main.cpp).
namespace MainThread
{
	// Runs a_fn on the main thread before the next frame's game work.
	void Post(std::function<void()> a_fn);

	// Whether game work has a main thread to run on: "hooked", "pending" (no game window yet) or "failed" (the
	// window could not be hooked: multiplayer is off this run).
	const char* HookState();
}
