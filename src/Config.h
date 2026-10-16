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

		// When hosting: the main story and faction quests are shared too ("shared", the default), or each
		// player plays their own ("own"; side quests are shared either way).
		bool sharedStory = true;

		// When hosting: plugins that may differ between players (comma-separated file names).
		std::vector<std::string> ignoredPlugins;

		// NPC base form used for other players' characters. It should belong to no factions,
		// or hitting another player can count as a crime against that faction.
		std::uint32_t puppetBaseForm = 0x0020A578;  // Settler (female, fixed look, no factions)

		// NPC base form other players should see for *you*. 0 = let them use their default.
		std::uint32_t myAppearance = 0;

		// Turn vsync off during loading screens so they finish faster.
		bool fastLoading = true;

		// While a session runs, keep the game running while its window is not focused (the game's own bAlwaysActive), so
		// a player who alt-tabs keeps their stand-in, and the NPCs their game runs, going for friends. Their character
		// stays live meanwhile (it can be hurt while alt-tabbed) and the sound plays on. Out of a session, or off, the
		// game's setting is as the player has it (see Background).
		bool runInBackground = true;

		// Friends' markers on the compass (bCompassMarkers). Off to test whether writing to the HUD
		// is behind loading-screen crashes.
		bool compassMarkers = true;

		// Follow the first player's (the host's) time of day and weather.
		bool syncTime = true;

		// When hosting: the shared world is saved to disk and carried on next time (same load order).
		bool saveSession = true;

		// bSpokenLines: the lines a friend's NPCs and a friend's own character say are heard, in the
		// speaker's recorded voice, from your copy of the speaker (a plain sound there, never the
		// dialogue system). Off: you see them as text only. The old keys for the console's Say
		// (bVoiceLines, bExperimentalVoiceLines, both off by now in most ini files) are not read.
		bool spokenLines = true;

		// Share of a friend's kill XP you get (0 = off, 1 = all of it).
		float xpShare = 0.5f;

		// Hotkeys (Windows virtual-key codes, hex). 0 = off.
		std::uint32_t keyMenu = 0x70;        // F1: the multiplayer menu (host, join, who's here, settings)
		std::uint32_t keyPlayerList = 0x75;  // F6: where everyone is
		std::uint32_t keyTeleport = 0x76;    // F7: tap to choose a friend, hold to teleport to them
		std::uint32_t keyPing = 0x77;        // F8: "over here"
		std::uint32_t keyEmotePoint = 0x71;  // F2: point (F9 is the game's quickload)
		std::uint32_t keyEmoteCheer = 0x72;  // F3: cheer
		std::uint32_t keyEmoteClap = 0x73;   // F4: clap

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

		// A freeze this long (seconds without the game reading its window messages; a save, a load or a
		// loading screen gets four times as long) is logged with a short report of where the game was
		// stuck (HangWatch.h). 0 = off.
		std::uint32_t hangSeconds = 15;
		// Also write a minidump of every thread with each report. Off by default: writing it can turn a
		// freeze the game would come out of into a permanent one, and it holds paths and the Windows user name.
		bool hangDump = false;

		// What the log holds (sLogLevel): "info" (the default) or "debug", which adds the friend scans (by
		// alias) and the lines characters say. HUD messages name players and are never logged. Applied by
		// Load.
		std::string logLevel = "info";

		// Developer control channel (see DevChannel.h). Off unless explicitly enabled.
		bool          devChannel = false;
		std::uint16_t devChannelPort = 7790;
	};

	void            Load();
	const Settings& Get();

	// The in-game menu changes settings while playing: the change applies to the running game
	// through Mutable() and is kept for next time with Write() (one "key = value" line of the ini;
	// the rest of the file, comments included, stays as the player left it). Main thread only.
	Settings& Mutable();
	bool      Write(std::string_view a_key, std::string_view a_value);
}
