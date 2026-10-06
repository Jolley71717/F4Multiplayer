#pragma once

// Settings read from Data/F4SE/Plugins/F4Multiplayer.ini (simple "key = value" lines,
// '#' or ';' start a comment). Missing keys keep their defaults.
namespace Config
{
	struct Settings
	{
		// Name shown to other players. Empty = your Steam name.
		std::string playerName;

		// Host a session on this machine. The host also plays; friends join through Steam
		// (invite, or "Join Game" in the friends list) or connect to the host's IP.
		bool host = false;

		// How friends connect: "steam" (Steam invites and relays; no ports to open), "enet" (direct
		// UDP to the host's IP and port) or "auto" (Steam when it's available, else UDP).
		std::string transport = "auto";

		// Address of the host to join: "ip", "ip:port" or "steam:<host's Steam ID>". Empty = wait for
		// a Steam invite. Ignored when hosting.
		std::string serverAddress;

		std::uint16_t port = 7779;
		std::size_t   maxPlayers = 4;
		std::string   password;

		// When hosting: players can hurt each other.
		bool friendlyFire = false;

		// NPC base form used for other players' characters. It should belong to no factions,
		// or hitting another player can count as a crime against that faction.
		std::uint32_t puppetBaseForm = 0x0020A578;  // Settler (female, fixed look, no factions)

		// NPC base form other players should see for *you*. 0 = let them use their default.
		std::uint32_t myAppearance = 0;

		// Turn vsync off during loading screens so they finish faster.
		bool fastLoading = true;

		// Follow the first player's (the host's) time of day and weather.
		bool syncTime = true;

		// Share of a friend's kill XP you get (0 = off, 1 = all of it).
		float xpShare = 0.5f;

		// Hotkeys (Windows virtual-key codes, hex). 0 = off.
		std::uint32_t keyPlayerList = 0x75;  // F6: where everyone is
		std::uint32_t keyTeleport = 0x76;    // F7: tap to choose a friend, hold to teleport to them
		std::uint32_t keyPing = 0x77;        // F8: "over here"

		// Share story/faction/side quest progress between players.
		bool syncQuests = true;

		// Developer control channel (see DevChannel.h). Off unless explicitly enabled.
		bool          devChannel = false;
		std::uint16_t devChannelPort = 7790;
	};

	void            Load();
	const Settings& Get();
}
