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

	// A save was loaded (or a new game started): the world went back in time, so the session's
	// changes are requested and applied again.
	void OnGameLoaded();

	[[nodiscard]] std::string Describe();

	// Test mode: shows a puppet that copies the local player's movement, offset to the side.
	void SetEcho(bool a_enabled, float a_offsetX = 150.0f, float a_offsetY = 0.0f);
}
