#pragma once

// Ties the network client, optional hosted server and remote players together.
// All functions run on the game's main thread.
namespace Session
{
	// Called once game data is loaded: computes the load-order hash and starts hosting if enabled.
	void Initialize();

	// Per-frame update: connection management, sending our state, applying remote states.
	void Frame();

	// Before a save is written or another save is loaded.
	void OnBeforeSaveOrLoad();

	[[nodiscard]] std::string Describe();
}
