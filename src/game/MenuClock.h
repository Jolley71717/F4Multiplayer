#pragma once

#include <string>
#include <string_view>

// Dev (menuclock): what stops the world under a menu, for the real-time menus (F4Multiplayer-tnl5, phase 0). Armed by
// the command, it logs a line each frame for a while after any menu opens or closes: the UI's menu count and freeze
// frame, the engine's timer and the UI's own (frame times and disable counts), Main's flags, the game hour and how far
// the NPC nearest the player moved since the frame before. Nothing runs until a developer arms it. Main thread.
namespace MenuClock
{
	// "on [seconds]": arm, logging that long after each menu event (2 s by default); "off"; nothing: the state and a
	// sample now.
	[[nodiscard]] std::string Command(std::string_view a_args);

	// Every frame (DevCommands::Frame), paused or not.
	void Frame();

	// The clocks now, without the NPC.
	[[nodiscard]] std::string Sample();
}
