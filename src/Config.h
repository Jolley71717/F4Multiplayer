#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Settings read from Data/F4SE/Plugins/F4Multiplayer.ini (simple "key = value" lines,
// '#' or ';' start a comment). Missing keys keep their defaults.
namespace Config
{
	struct Settings
	{
		// Name shown to other players: a name, "character" (the name of the character you play; your Steam name until it
		// has one) or empty (your Steam name, as older ini files have it). See net/NameChoice.h.
		std::string playerName;

		// Language of the mod's own texts (HUD notices, the F1 menu): a game language code ("de", "ru", "ptbr", "ja", "cn"...) or the
		// code of a file in the lang folder ("en-gb"). Empty = the game's own sLanguage (Fallout4.ini); English when there is
		// no file for it (see Text.h).
		std::string language;

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

		// Friends' stand-ins wear their own face (head parts, hair, complexion, sculpting, skin tone and face paint)
		// on a copy of the settler, built by the engine as the stand-in loads, the way it builds the player's head
		// (game/FaceHeads.h). Off: the settler's face with their sex and body shape, as 0.8.8 and earlier showed everyone
		// (and what a stand-in falls back to when the engine builds no head, or when the engine could not be patched).
		bool friendFaces = true;

		// A friend's grenade, mine or Molotov is seen in their stand-in's hand through the throw's wind-up: the engine
		// hangs it there but never draws it on a throw it did not start itself, so it is drawn for it (game/HandThrowable.h).
		// Off: the stand-in throws from an empty fist, as 0.8.11 and earlier showed it.
		bool grenadeInHand = true;

		// Turn vsync off during loading screens so they finish faster.
		bool fastLoading = true;

		// For this many seconds of a loading screen at most: then vsync goes back while the screen stays up, so a
		// loading screen that never ends (a hang) stops drawing at 1800 fps. Any real load is done well within them.
		std::uint32_t fastLoadingSeconds = 20;

		// While a session runs, keep the game running while its window is not focused (the game's own bAlwaysActive), so
		// a player who alt-tabs keeps their stand-in, and the NPCs their game runs, going for friends. Their character
		// stays live meanwhile (it can be hurt while alt-tabbed) and the sound plays on. Out of a session, or off, the
		// game's setting is as the player has it (see Background).
		bool runInBackground = true;

		// Friends' markers on the compass (bCompassMarkers). Off to test whether writing to the HUD
		// is behind loading-screen crashes.
		bool compassMarkers = true;

		// An icon over a friend's stand-in while they are in the Pip-Boy or another menu (bMenuIcons; MenuIcons). Off, and
		// not in the shipped ini: on the rig the HUD showed it only in part, or where it was first drawn (docs/DESIGN.md,
		// Menus). Their stand-in raises its Pip-Boy and its name tag says so either way.
		bool menuIcons = false;

		// A Pip-Boy over the nearest friend's stand-in while they are in theirs, hung in the world from its 3D rather than
		// drawn on the HUD (bMenuBadges; MenuBadges, StandInBadge.h). Off until it is seen on the rig: the Pip-Boy's world
		// model, the engine's one node for it, has never been drawn this way.
		bool menuBadges = false;

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
		std::uint32_t keyPing = 0x77;        // F8: ping what the crosshair is on
		std::uint32_t keyWhere = 0x79;       // F10: how far and which way each friend is (F5 and F9 are the game's quicksave and quickload)
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

		// Share the Minutemen's radiant quests by their run (RadiantSync): a friend's settlement request starts
		// here with the same settlement and place, and pays its reward once in each game. Needs syncQuests and a
		// shared story.
		bool sharedRadiants = true;

		// When hosting, the session's rules for everyone (Protocol::SessionRules, sent with the welcome; joining, the
		// host's apply and these are not used).
		// With friends in the session, a lethal hit knocks you down; a friend who stays next to
		// you for reviveSeconds helps you up. Otherwise you die after bleedOutSeconds (or when you give up).
		bool          revive = true;
		std::uint32_t bleedOutSeconds = 45;
		float         reviveSeconds = 2.0f;
		std::uint32_t reviveHealth = 30;           // percent of the maximum a player gets up with
		std::uint32_t maxRevives = 0;              // times one player can be helped up per window; 0 = no limit
		std::uint32_t reviveWindowMinutes = 0;     // that window; 0 = per fight
		std::uint32_t reviveCooldownSeconds = 0;   // helped up, a lethal hit within this long kills instead of downing
		bool          reviveOutOfCombat = false;   // friends can help only while they are not in a fight
		// In a session, VATS runs in real time, as in Fallout 76, instead of slowing the world almost to a stop.
		bool realTimeVats = true;

		// Not a host's rule: in a session, the game's tutorial hints (its help messages) stay hidden (bShowTutorials).
		bool hideTutorials = true;

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
