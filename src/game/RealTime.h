#pragma once

#include <string>

// What runs in real time in a session, by the host's rules (HostRules): the Pip-Boy and the menus that stop the game
// alone (game/RealTimeMenus.h has which), and VATS. And, by the player's own bHideTutorials, the game's tutorial
// hints stay hidden in a session (its bShowTutorials, put back as it was after). Main thread.
namespace RealTime
{
	// Wraps the menus' creation so a real-time one is made without its "pauses the game" flag. Once the game's data is
	// loaded (the UI has registered its menus).
	void Install();

	// Every frame. a_session: hosting, or joined to someone's.
	void Frame(bool a_session);

	// Dev: the game's time multiplier, its timer, VATS and the menus made in real time so far.
	[[nodiscard]] std::string Describe();
}
