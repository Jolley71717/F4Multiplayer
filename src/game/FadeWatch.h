#pragma once

#include <string>
#include <string_view>

// The game's side of FadeGuard: a fade the session caused that the game's scripts never faded back in is faded in
// after FadeGuard::STUCK_AFTER of play. Main thread only.
namespace FadeWatch
{
	// The session set a stage here or moved the player (a_what: for the log).
	void Arm(std::string_view a_what);

	// Every frame in the game.
	void Frame();

	// Fades the screen back in (Game.FadeOutGame(false, true, 0, 1, false)), as the guard does. Whether the call was queued.
	bool FadeIn();

	// Fades the screen out and keeps it so, as a quest script would (Game.FadeOutGame(true, true, 0, 1, true)): for
	// testing the guard (dev channel).
	bool FadeOutAndStay();

	[[nodiscard]] std::string Describe();
}
