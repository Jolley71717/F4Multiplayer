#pragma once

#include <string>

// What runs differently in a session: VATS in real time, by the host's rules (HostRules; game/RealTimeVats.h), and,
// by the player's own bHideTutorials, the game's tutorial hints hidden (its bShowTutorials, put back as it was after).
// Main thread.
namespace RealTime
{
	// Every frame. a_session: hosting, or joined to someone's.
	void Frame(bool a_session);

	// Dev: the game's time multiplier, its timer, VATS, the menus' pause count, the UI's timer and the game hour.
	[[nodiscard]] std::string Describe();
}
