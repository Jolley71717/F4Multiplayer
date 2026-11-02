#pragma once

// A Multiplayer entry in the game's main menu, after Load, and in the pause menu (the same movie, MainMenu.swf, in
// PauseMode). Pressing it opens our multiplayer menu (MPMenu) beside the main list. The entry is
// put into the menu's own list from inside the menu's update, as the compass markers are put into the HUD's, and the
// game's MainMenu.swf is left as it is: a mod that replaces it and keeps a Load entry gets ours too.
namespace MainMenuEntry
{
	// At plugin load: hooks the main and pause menus' updates (their classes' vtables, by the address library).
	void Install();
}
