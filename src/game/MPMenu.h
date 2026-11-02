#pragma once

// The multiplayer menu of our own: a menu of the game's UI (F4MPMenu) with a movie of ours (Interface/F4Multiplayer/MPMenu.swf,
// built from ui/MPMenu by tools/flash/build.sh), opened over the main or pause menu by its Multiplayer entry
// (MainMenuEntry) and lined up beside the main menu's list. Its input is its own while it is open: the main menu
// underneath keeps its state. For now each entry opens the F1 pages (Menu::Open) in its place.
namespace MPMenu
{
	// Once the game's data is loaded (the UI exists by then, and the main menu comes after).
	void Register();

	// Main thread. The main menu's panel box on its 1280x720 stage, which ours is laid out beside and scaled to.
	void Open(float a_left, float a_top, float a_right, float a_bottom);
}
