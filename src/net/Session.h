#pragma once

// Ties the network client, optional hosted server and remote players together.
// All functions run on the game's main thread.
namespace Session
{
	// Called once game data is loaded: computes the load-order hash. With bHost, Frame starts hosting once
	// the player is in the world and out of the opening.
	void Initialize();

	// Per-frame update: connection management, sending our state, applying remote states.
	void Frame();

	// Before a save is written or another save is loaded.
	void OnBeforeSaveOrLoad();

	// The game is quitting (its shutdown stage, on the main thread, before the world is deleted): the hosted
	// server writes the session file and stops, and the connections close while Steam and the network are
	// still there. Nothing else runs after it.
	void Shutdown();

	// A save was loaded (or, a_newGame, a new game started): the world went back in time, so the session's
	// changes are requested and applied again. Another character's save (or a new game) leaves the session first.
	void OnGameLoaded(bool a_newGame);

	[[nodiscard]] std::string Describe();

	// The in-game menu: hosting and leaving while playing, and what to show.
	// Starts the session server in this game (as bHost does once in the world) and connects to it. Returns
	// what went wrong, or "" when it's up. Not in the opening.
	[[nodiscard]] std::string StartHosting();
	// Stops the hosted server; everyone in it is disconnected.
	void StopHosting();
	// Leaves the session we joined (and its Steam lobby) and stops reconnecting.
	void Leave();

	struct Status
	{
		enum class Link
		{
			kNone,        // not trying to reach anyone
			kConnecting,  // trying (or waiting to retry)
			kConnected,   // transport up, waiting for the welcome
			kJoined,      // welcomed: in the session
		};
		Link          link = Link::kNone;
		bool          hosting = false;
		bool          steam = false;  // friends come through Steam (invites, Join Game)
		std::string   address;        // the server we're set to reach ("" = none)
		std::string   rejectReason;   // why the last join was refused ("" = it wasn't)
	};
	[[nodiscard]] Status GetStatus();

	// Test mode: shows a puppet that copies the local player's movement, offset to the side.
	void SetEcho(bool a_enabled, float a_offsetX = 150.0f, float a_offsetY = 0.0f);

	// Test: leaves the current session and connects to another address ("ip[:port]" or "steam:<id>").
	void ConnectTo(std::string a_address);
	// Test: handles these bytes as if the server had sent them (the dev "fuzz" command).
	void InjectPacket(std::span<const std::uint8_t> a_data);
}
