#pragma once

#include "game/MenuLog.h"

// The game's menus, kept from their open and close events (MenuLog), for the stress loop's dev commands:
// which are open and since when, and how often each opened and closed. Installed only with the dev
// channel on. Thread-safe: the events come on whichever thread opens or closes the menu.
namespace MenuWatch
{
	void Install();

	// Whether the events are being heard (Install ran and the UI was there).
	[[nodiscard]] bool Installed();

	[[nodiscard]] bool IsOpen(std::string_view a_name);

	// How long the menu has been open; zero when it isn't.
	[[nodiscard]] MenuLog::Clock::duration OpenFor(std::string_view a_name);

	// MenuLog::Log::Describe, now.
	[[nodiscard]] std::string Describe();
}
