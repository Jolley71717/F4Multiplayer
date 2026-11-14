#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

// Game work runs on the game's main thread, between frames (see main.cpp).
namespace MainThread
{
	// Runs a_fn on the main thread before the next frame's game work. Never dropped; a long queue is warned of once.
	void Post(std::function<void()> a_fn);

	// The most pieces of work ever waiting for the main thread at once (Post), for net status.
	[[nodiscard]] std::size_t PostedMost();

	// From here on game work runs on the main thread (the game window is hooked): Assert checks it.
	void Capture();

	// Game state isn't for two threads: called off the main thread (after Capture), a_where is logged as an
	// error, once, and counted. Never throws: the call goes on.
	void Assert(std::string_view a_where);

	// How many calls Assert found off the main thread.
	[[nodiscard]] std::uint32_t OffThread();

	// Whether the caller runs on the main thread (the one the plugin was loaded on): for work done at once there and handed
	// over (Post) from any other thread.
	[[nodiscard]] bool IsMain();

	// Whether game work has a main thread to run on: "hooked", "pending" (no game window yet) or "failed" (the
	// window could not be hooked: multiplayer is off this run).
	const char* HookState();
}
