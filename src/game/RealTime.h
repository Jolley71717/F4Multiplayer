#pragma once

#include <chrono>
#include <string>

// What runs differently in a session: VATS by the host's rules (HostRules; game/RealTimeVats.h: in real time, the game's
// slow motion, or that with friends' stand-ins slowed with it), and, by the player's own bHideTutorials, the game's
// tutorial hints hidden (its bShowTutorials, put back as it was after). Main thread.
namespace RealTime
{
	// Every frame. a_session: hosting, or joined to someone's.
	void Frame(bool a_session);

	// How far behind the present friends' stand-ins are played now (zero but in VATS with slowed friends, and while that
	// is made up afterwards): FrameClock::PlaceAt and the states' playback (StateBuffer, NpcSync) take it off their time.
	// Any thread.
	[[nodiscard]] std::chrono::steady_clock::duration StandInLag();

	// Dev: the game's time multiplier, its timer, VATS and the menus' pause count.
	[[nodiscard]] std::string Describe();
}
