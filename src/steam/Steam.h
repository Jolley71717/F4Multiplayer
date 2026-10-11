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
	// A friend's Steam name ("" when Steam doesn't know them).
	[[nodiscard]] std::string NameOf(SteamApi::SteamId a_id);

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

	// Leaves the lobby we joined (not our own: StopHosting does that), when the player leaves a
	// friend's session from the menu.
	void Leave();

	// We are hosting and our friends-only lobby is open (so "Join Game" and invites work).
	[[nodiscard]] bool LobbyOpen();

	// Steam friends who are hosting a Fallout 4 co-op game right now: the lobby their rich presence
	// advertises (or the lobby Steam says they're in). For the menu's "join a friend" page.
	struct JoinableFriend
	{
		SteamApi::SteamId id = 0;
		SteamApi::SteamId lobby = 0;
		std::string       name;
	};
	struct FriendScan
	{
		std::size_t                 friends = 0;  // Steam friends
		std::size_t                 online = 0;
		std::size_t                 inGame = 0;   // in Fallout 4, as far as Steam tells us
		std::vector<JoinableFriend> hosting;
	};
	[[nodiscard]] FriendScan                  ScanFriends();
	[[nodiscard]] std::vector<JoinableFriend> JoinableFriends();  // ScanFriends().hosting

	// Steam hands out a friend's rich presence only once it's been asked for: this asks for every
	// online friend's and returns how many were asked. The answers come in over the next moment, each
	// counted in PresenceUpdates(), after which ScanFriends sees a host's lobby.
	std::size_t                 RefreshFriends();
	[[nodiscard]] std::uint32_t PresenceUpdates();

	// Dev: one line per Steam friend (state, game, lobby, "connect" presence).
	[[nodiscard]] std::string DescribeFriends();

	[[nodiscard]] std::string Describe();

	// Developer test: sends a message to our own Steam ID, or reports what arrived.
	std::string SelfTest(bool a_send);
}
