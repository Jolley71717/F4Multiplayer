#pragma once

// A Multiplayer entry in the game's main menu, after Load, and in the pause menu (the same movie, MainMenu.swf, in
// PauseMode). Pressing it opens our multiplayer menu (MPMenu) beside the main list. The entry is
// put into the menu's own list from inside the menu's update, as the compass markers are put into the HUD's, and the
// game's MainMenu.swf is left as it is: a mod that replaces it and keeps a Load entry gets ours too.
namespace MainMenuEntry
{
	// At plugin load: hooks the main and pause menus' updates (their classes' vtables, by the address library).
	void Install();

	// The mouse beside the open multiplayer menu, on the stage both movies share. Any thread. It reaches the main list in that
	// menu's own update: a row under it is chosen, as a row of the game's own column is; a click on another row closes the
	// multiplayer menu and presses that row.
	void PanelMouse(float a_x, float a_y, bool a_click);

	// Main thread: opens the multiplayer menu beside the main panel, or where it usually is if no menu has been seen.
	void OpenPanel();
}
