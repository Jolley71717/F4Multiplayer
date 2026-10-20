#pragma once

// Friends on the Pip-Boy's maps: each friend's mark (PlayerMarks: their color and shape, as on the compass) with their
// name, at their spot on the world map and, when they are near enough to be on it, the local map; a friend in an
// interior is shown on the world map at its way out, as the game shows the player. Drawn into the map page's own
// marker layer from inside the Pip-Boy menu's own update (its AdvanceMovie, hooked by its class's vtable: the menu is
// never looked up), where it moves and zooms with the map. Nothing of it is in the game's data or a save: the page
// goes with the menu, and the marks with the page. See FriendMapPlace.h for the sums, docs/DESIGN.md (Map).
namespace FriendMap
{
	// Main thread, each frame after RemotePlayers::Update: where each friend goes, while the Pip-Boy is up.
	void Frame();

	// Dev (friendmap): what the last update of each map found and drew; friend IDs, no names.
	[[nodiscard]] std::string Probe();

	[[nodiscard]] std::string Describe();
}
