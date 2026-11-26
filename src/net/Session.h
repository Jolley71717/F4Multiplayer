#pragma once

#include "Protocol.h"
#include "net/ModDiff.h"

// Ties the network client, optional hosted server and remote players together.
// All functions run on the game's main thread.
namespace Session
{
	// Called once game data is loaded: computes the load-order hash. With bHost, Frame starts hosting once
	// the player is in the world and out of the opening.
	void Initialize();

	// Per-frame update: connection management, sending our state, applying remote states.
	void Frame();

	// Before a save is written or another save is loaded (a_load).
	void OnBeforeSaveOrLoad(bool a_load);

	// A load begins (before OnBeforeSaveOrLoad): from here on the character's ID is the save's (CharacterBinding).
	void OnBeforeLoad();

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

	// Turns "join by address" on or off for a host who uses Steam: while hosting, the UDP port is opened (or closed) on this
	// PC's addresses beside the running session, so nobody in it is dropped but those who came in by address. Written to the
	// ini (bDirectJoin) either way. Returns "" or why it failed (nothing changed). Without Steam the port is always open.
	[[nodiscard]] std::string SetDirectJoin(bool a_on);
	// Leaves the session we joined (and its Steam lobby) and stops reconnecting.
	void Leave();
	// Dev only (`net drop`): as the link to the server dropping: the session ends here and the connection is tried again
	// (the server sees us go). Whether we were connected.
	bool DropConnection();

	// Host controls, while we host: the server's last word on its lock, password and bans (none before it came), and a
	// count that grows with each (the menu waits for the answer to a command by it).
	[[nodiscard]] std::optional<Protocol::HostInfo> HostControls();
	[[nodiscard]] std::uint32_t                     HostControlsSerial();
	// Kick, ban, unban, lock, unlock or a new password, to our own server (it takes them from the host's game only).
	// False when we aren't hosting and in our own session.
	bool SendHostCommand(const Protocol::HostCommand& a_command);
	// Asks our server for the state again (the answer comes as HostControls).
	void AskHostControls();
	// Dev: the host controls' state on one line, bans by alias.
	[[nodiscard]] std::string DescribeHostControls();

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
		bool          direct = false;  // hosting, and the UDP port is open to others (joins by address)
		std::string   address;        // the server we're set to reach ("" = none)
		std::string   rejectReason;   // why the last join was refused ("" = it wasn't)
		bool          retrySoon = false;    // the next try comes in a few seconds (the host isn't in yet), not in a minute
		bool          inWorld = false;      // a game is loaded: no connection is tried before that
		// Why no connection came up yet, for an address typed in the menu: the name was not found, or nothing answered.
		enum class Failure
		{
			kNone,
			kBadName,
			kNoAnswer
		};
		Failure failure = Failure::kNone;
		bool          hostWaiting = false;  // bHost, but not hosting yet: it starts once we're in the world, out of Vault 111
	};
	[[nodiscard]] Status GetStatus();

	// Our mods compared with the host's, once the host's list came: with a refusal for the load order, with the welcome
	// when ours differs only in plugins the host lets differ, or after AskHostMods. None otherwise (our load order is
	// the host's, or nothing came yet).
	[[nodiscard]] std::optional<ModDiff::Difference> HostMods();
	// Asks the host for its list again (the answer is told on the HUD whatever it says): not when not in a session we
	// joined, nor within a few seconds of the last list or ask (the server would not answer: the HUD says to wait).
	enum class AskResult
	{
		kAsked,
		kTooSoon,
		kNotJoined,
	};
	AskResult AskHostMods();
	// Dev: what is known of the host's mods, on one line.
	[[nodiscard]] std::string DescribeMods();
	// Dev: our form IDs against the session's plugin table (FormIds.h), what was skipped either way, and a_id as ours
	// in the session's numbering and as the session's in ours.
	[[nodiscard]] std::string DescribeForms(std::optional<std::uint32_t> a_id);

	// Test mode: shows a puppet that copies the local player's movement, offset to the side.
	void SetEcho(bool a_enabled, float a_offsetX = 150.0f, float a_offsetY = 0.0f);

	// The menu's "join by address": leaves a Steam lobby or another session first, and joins the address (one
	// UiAddressEntry::Valid took). It is remembered (Config::RememberAddress) once the host's welcome comes, not before.
	enum class JoinError
	{
		kNone,
		kHosting,   // our game hosts: it would be pointed away from its own server
		kHostWaits  // bHost is on and hosting starts in the world
	};
	JoinError JoinAddress(std::string a_address);
	// The next connection try comes now (after the password was typed), not when a refusal's wait ends.
	void RetryNow();
	// Test: leaves the current session and connects to another address ("ip[:port]" or "steam:<id>").
	void ConnectTo(std::string a_address);
	// Test: handles these bytes as if the server had sent them (the dev "fuzz" command), in our own numbering: they are
	// not translated from the session's.
	void InjectPacket(std::span<const std::uint8_t> a_data);
}
