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

		// When hosting: the main story and faction quests are shared too ("shared"), or each
		// player plays their own ("own", the default; side quests are shared either way).
		bool sharedStory = false;

		// When hosting: plugins that may differ between players (comma-separated file names).
		std::vector<std::string> ignoredPlugins;

		// NPC base form used for other players' characters. It should belong to no factions,
		// or hitting another player can count as a crime against that faction.
		std::uint32_t puppetBaseForm = 0x0020A578;  // Settler (female, fixed look, no factions)

		// NPC base form other players should see for *you*. 0 = let them use their default.
		std::uint32_t myAppearance = 0;

		// Turn vsync off during loading screens so they finish faster.
		bool fastLoading = true;

		// Follow the first player's (the host's) time of day and weather.
		bool syncTime = true;

		// When hosting: the shared world is saved to disk and carried on next time (same load order).
		bool saveSession = true;

		// Share of a friend's kill XP you get (0 = off, 1 = all of it).
		float xpShare = 0.5f;

		// Hotkeys (Windows virtual-key codes, hex). 0 = off.
		std::uint32_t keyPlayerList = 0x75;  // F6: where everyone is
		std::uint32_t keyTeleport = 0x76;    // F7: tap to choose a friend, hold to teleport to them
		std::uint32_t keyPing = 0x77;        // F8: "over here"

		// Proximity voice chat through Steam: hold keyVoice to talk (or always, with an open mic).
		// Friends hear you at full volume within 8 m, fading out at voiceRange meters (0 = everyone
		// always hears everyone).
		bool          voiceChat = true;
		bool          voiceOpenMic = false;
		std::uint32_t keyVoice = 0x05;  // mouse side button (back)
		float         voiceRange = 60.0f;

		// Share story/faction/side quest progress between players.
		bool syncQuests = true;

		// With friends in the session, a lethal hit knocks you down; a friend who stays next to
		// you for 2 seconds helps you up. Otherwise you die after 45 seconds (or when you give up).
		bool revive = true;

		// Locations a friend discovers show up on your map, ready for fast travel.
		bool shareMap = true;

		// Developer control channel (see DevChannel.h). Off unless explicitly enabled.
		bool          devChannel = false;
		std::uint16_t devChannelPort = 7790;
	};

	void            Load();
	const Settings& Get();
}
