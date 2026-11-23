#pragma once

#include <string>

// The console's place commands, watched: `placeatme` and `placeleveledactoratme` tell Spawns where they ran, so an NPC
// our player places with the console is shared with friends calm too (ConsoleSpawns.h says why and how it is found).
//
// The commands are the engine's old script functions, which only the console (and a batch file run from it) calls:
// Papyrus has its own PlaceAtMe, which never comes this way. Each has a slot in the engine's table of script functions
// holding the function that runs it; the slot is pointed at a wrapper of ours that notes the reference the command runs
// on and calls the engine's function with what it was given, untouched.
namespace ConsolePlace
{
	// At game data ready (the loading thread, before any console command can run), once: the slots are pointed at the
	// wrappers. A command the table does not hold, or one with no function, is left alone, logged.
	void Install();

	// For `net status` and the dev channel: which commands are watched and how often each ran.
	[[nodiscard]] std::string Describe();
}
