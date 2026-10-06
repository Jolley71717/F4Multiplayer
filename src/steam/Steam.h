#pragma once

#include "steam/SteamApi.h"

// Steam lobbies, invites and "Join Game", on top of the game's own Steam connection.
// Main thread only, except where noted.
namespace Steam
{
	// Every frame: finishes setting up once the game has started Steam, then handles lobby events.
	void Frame();

	// Steam is running in this game and has everything we need.
	[[nodiscard]] bool Available();

	[[nodiscard]] SteamApi::SteamId LocalId();
	[[nodiscard]] std::string       PersonaName();

	// Hosting keeps a friends-only lobby open, so friends can join from the Steam overlay or friends
	// list, and accepts connections from its members and from Steam friends.
	void StartHosting(std::size_t a_maxPlayers);
	void StopHosting();

	// The host the player chose to join (accepted an invite or clicked "Join Game"). Returned once.
	[[nodiscard]] std::optional<SteamApi::SteamId> TakeJoinTarget();

	// Opens the Steam overlay's invite dialog for the current lobby.
	bool OpenInviteDialog();

	// Joins a host's lobby by lobby ID, or a host directly by their Steam ID.
	void Join(SteamApi::SteamId a_id);

	[[nodiscard]] std::string Describe();

	// Developer test: sends a message to our own Steam ID, or reports what arrived.
	std::string SelfTest(bool a_send);
}
