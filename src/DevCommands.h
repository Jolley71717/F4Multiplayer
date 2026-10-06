#pragma once

// Game-side commands exposed through the developer control channel.
// Every handler runs on the game's main thread.
namespace DevCommands
{
	using Handler = std::string (*)(std::string_view a_args);

	// Returns the handler for a command name, or nullptr if unknown.
	Handler Find(std::string_view a_name);

	// One-line summary of available commands.
	std::string Help();
}
