#pragma once

// Quitting the game (qqq, Quit to Desktop, the main menu's Quit) could crash in TESObjectCELL::GetMusicType.
// Main::Shutdown deletes the player's region state before it deletes TES, and each cell TES destroys has
// the player's music changer check the cell's music against the music it holds: while it holds one, that
// looks the cell's music up through the region state that is gone. The game's shutdown stage event comes
// before both, so on it the changer's music is stopped and forgotten, the way the game's own audio reset
// does it, and the cells find nothing to check.
namespace ExitMusic
{
	void Install();
}
