#pragma once

#include <string>

// The developer HUD (bDeveloperHud, a row in the menu's settings): live numbers at the top right of the screen while a
// game is loaded. A menu of the game's UI (F4MPDevHud) with a movie of ours (ui/DevHud), which doesn't pause the game, take
// input or show a cursor. The numbers are gathered on the main thread and written to the movie in the menu's own update.
namespace UiDevHud
{
	// Once the game's data is loaded.
	void Register();

	// Main thread, every frame: opens or closes the menu as the setting and the game's state ask, and gathers the text.
	void Frame();

	// The menu's state and the latest text, for the dev channel. Main thread.
	[[nodiscard]] std::string Describe();
}
