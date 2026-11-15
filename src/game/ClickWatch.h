#pragma once

#include "game/ClickRates.h"

#include <string>

// The game side of ClickRates: the places the plugin does something the game answers with a sound note it here. A source
// that does it more often than any play does is a repeating sound (the constant clicking after a friend joined or left), and says
// so in the log, once a minute at most, by name. The counts are in the status line ("clicks:").
namespace ClickWatch
{
	// One or a_count of the source's events. Any thread.
	void Note(ClickRates::Source a_source, std::uint32_t a_count = 1);

	// One event of a source counted for each thing apart (a stand-in's draw asks); a_own: what that thing's own window
	// holds with it (ClickRates::Own::Note). Any thread.
	void NoteOwn(ClickRates::Source a_source, std::uint32_t a_own);

	[[nodiscard]] std::string Describe();
}
