#pragma once

#include <string>
#include <string_view>

// The mod's own autosaves (after a teleport, and on going down by the host's rule) and a log line for each save and
// load the game makes, naming it, so a report of "an old save" can be read (AutoSaveRules.h says why and when).
// Main thread, except the load's line, which may come on the load's own thread.
namespace AutoSave
{
	// The player landed after the teleport key's coc or cow (Party): travel, as a load door or fast travel would be.
	void AfterTeleport();

	// The player just went down; a_rule: the host's bSaveOnDown (Downed). An autosave the game still holds is taken back
	// first: no travel autosave is made of a downed player.
	void OnWentDown(bool a_rule);

	// The player bleeds out (Downed, before its kill): an autosave the game still holds is taken back, so the newest save,
	// the one the death loads, is never one of a dead player.
	void OnDying();

	// Once a frame: a request the game never answered with a save is logged, and with the player dead (killed outright,
	// too), an autosave the game holds is taken back.
	void Frame();

	// F4SE's messages around a save and before a load; a_name is their data (a save's name or file path).
	void OnPreSave(std::string_view a_name);
	void OnPostSave(std::string_view a_name);
	void OnPreLoad(std::string_view a_name);

	// One line for the dev channel's status.
	std::string Describe();
}
