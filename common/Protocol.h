#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Wire protocol shared by the game plugin, the server and test tools.
// All values are little-endian. Every packet starts with a one-byte MessageType.
namespace Protocol
{
	inline constexpr std::uint32_t MAGIC = 0x504D3446;  // "F4MP"
	inline constexpr std::uint16_t VERSION = 35;
	inline constexpr std::uint16_t DEFAULT_PORT = 7779;
	// A name's bytes on the wire: Name::MAX_CHARACTERS characters of UTF-8, up to four bytes each (common/Name.h).
	inline constexpr std::size_t   MAX_NAME_LENGTH = 128;
	inline constexpr std::size_t   MAX_REASON_LENGTH = 200;
	// A load order on the wire (a hello's, kLoadOrder's): at most this many names, the light plugins' mark among them (the
	// game allows 255 full plugins plus thousands of light ones; the rest are left off), each of at most this many bytes
	// (a file name on Windows is at most 260 characters).
	inline constexpr std::size_t   MAX_PLUGINS = 512;
	inline constexpr std::size_t   MAX_PLUGIN_NAME_LENGTH = 260;
	// The plugins a host lets differ (sIgnoredPlugins) that kLoadOrder carries: far more than a host lists.
	inline constexpr std::size_t   MAX_IGNORED_PLUGINS = 64;
	inline constexpr std::size_t   MAX_PASSWORD_LENGTH = 64;
	inline constexpr std::size_t   MAX_LINE_LENGTH = 200;  // a spoken line (subtitle text)

	// How a Reject's reason for the load order starts (LoadOrderDifference). The joiner's game names the plugins that
	// differ from the session's list (kLoadOrder, sent just before), so the reason names none.
	inline constexpr std::string_view REJECT_LOAD_ORDER = "Your load order is different from the other players.";

	// A Reject's reason when a hosted server's own game hasn't joined yet: the host's world decides the
	// session, so nobody else may arrive first. The client knows this one and tries again soon, not in a
	// minute. (Only the text: older clients show it and retry as for any refusal.)
	inline constexpr std::string_view REJECT_HOST_NOT_READY = "The host isn't in the game yet";

	// The local player's reference. Never shared: each game has its own.
	inline constexpr std::uint32_t PLAYER_REF_ID = 0x14;

	// A ContainerChange whose item is this (count -1) says its container or corpse was emptied of everything the
	// session shares: each game rolls a chest's and an NPC's leveled loot on its own, so the receiver gives up every
	// shared stack its own copy holds (LootSync). The player's reference is never an inventory item, so an older
	// build takes it for something it can't share and leaves it, and an older server keeps and numbers it like any.
	inline constexpr std::uint32_t CONTAINER_EMPTIED = PLAYER_REF_ID;

	// References are shared by form ID, which matches for everyone because the load order must.
	// A coordinate the game world can hold: finite and well inside the engine's limits (the Commonwealth
	// spans about 150,000 units; a value like 1e38 from a bad client would break cell lookup).
	inline constexpr float MAX_COORDINATE = 1.0e6f;
	[[nodiscard]] inline bool InWorld(float a_v)
	{
		return std::isfinite(a_v) && a_v >= -MAX_COORDINATE && a_v <= MAX_COORDINATE;
	}

	// Runtime-created references (0xFF......) differ per game, and the player is per game.
	[[nodiscard]] constexpr bool IsShareableRef(std::uint32_t a_id)
	{
		return a_id != 0 && a_id != PLAYER_REF_ID && (a_id >> 24) != 0xFF;
	}

	// An NPC a game made at runtime (a random encounter's raider, an ambush) is shared under a spawn id that game picks
	// and the server grants it (kReportSpawn): every other game places a copy of it (kSpawned), and the game that made it
	// runs it for good. A spawn id is in the runtime range too, so no game ever takes one for a reference of its own: each
	// game looks it up in its own book of spawns (src/game/SpawnBook.h), and the server in its own.
	[[nodiscard]] constexpr bool IsSpawnRef(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF && a_id != 0xFF000000;
	}

	// An NPC the session can name: one from a plugin file, or a spawn.
	[[nodiscard]] constexpr bool IsActorRef(std::uint32_t a_id)
	{
		return IsShareableRef(a_id) || IsSpawnRef(a_id);
	}

	// Spawns one game may share at once (a settlement's attackers are a dozen or two).
	inline constexpr std::size_t MAX_SPAWNS_PER_PLAYER = 32;

	// Largest item count a single container change may move.
	inline constexpr std::int32_t MAX_ITEM_COUNT = 1'000'000;

	// ENet channels
	inline constexpr std::uint8_t CHANNEL_RELIABLE = 0;
	inline constexpr std::uint8_t CHANNEL_STATE = 1;  // unreliable, sequenced
	inline constexpr std::uint8_t CHANNEL_COUNT = 2;

	// A new type also goes in IsMessageType() and Samples() in tests/ProtocolSamples.h (the tests fail to build or fail without them), so the robustness tests reach its decoder.
	enum class MessageType : std::uint8_t
	{
		// client -> server
		kHello = 1,
		kPlayerState = 2,
		kReportDeath = 3,  // an actor died in the sender's world
		kReportHealth = 4,  // the sender damaged an actor; this is its health now
		kReportContainer = 5,  // the sender took items from / put items into a container
		kReportPickup = 6,     // the sender picked up an item lying in the world
		kReportRefState = 7,   // a door/container the sender used is now open/closed, locked/unlocked
		kEquipment = 8,        // what the sender is wearing and holding
		kClaimActors = 9,      // the sender has these NPCs loaded and offers to run their AI
		kReleaseActors = 10,   // the sender no longer runs these NPCs (unloaded or dead)
		kActorStates = 11,     // positions of NPCs the sender runs
		kPlayerHit = 12,       // an NPC in the sender's world hit another player's stand-in
		kReportQuestStage = 13,  // a quest reached a new stage in the sender's game
		kRequestWorldState = 14,  // the sender loaded a save and needs the session's changes again
		kReportShot = 15,         // the sender (or an NPC it runs) fired a weapon
		kReportStatus = 16,       // the sender's location, health and level, for the player list
		kReportTime = 17,         // the sender's time of day and weather (only the first player's are used)
		kPing = 18,               // "over here": the sender wants the others to come to them
		kReportXp = 19,           // the sender got XP for a kill; the others get a share
		kHeartbeat = 20,          // answered right away with kHeartbeatAck, to measure the connection
		kReportQuestDone = 21,    // the sender completed a quest themselves
		kRevive = 22,             // the sender helped a downed player up
		kVoice = 23,              // a piece of the sender's voice (Steam-compressed)
		kReportMarkers = 24,      // map markers the sender discovered
		kReportNpcEquipment = 26,  // what an NPC the sender runs is wearing and holding
		kReportFace = 27,          // the sender's character's face and body shape
		kReportWorkshopItem = 28,  // the sender placed, moved or scrapped a workshop object
		kReportExplosion = 29,     // an explosion after one of the sender's throws
		kReportActivation = 30,    // the sender pressed a button, switch or lever
		kReportSpawn = 31,         // the sender's game made an NPC at runtime; it shares it under this spawn id
		kRename = 32,              // the sender's name changed (a new character named in the opening after connecting)
		kRequestLoadOrder = 33,    // the sender wants the session's load order (answered with kLoadOrder)
		kReportLine = 25,         // the sender (or an NPC it runs) said something

		// server -> client
		kWelcome = 101,
		kReject = 102,
		kPlayerJoined = 103,
		kPlayerLeft = 104,
		kPlayerStates = 105,
		kActorDied = 106,     // an actor died in another player's world
		kWorldState = 107,    // sent on join: everything that already happened this session
		kActorHealth = 108,   // an actor's health changed in another player's world
		kContainerChanged = 109,  // a player changed a container's contents (also echoed to that player)
		kRefPickedUp = 110,       // another player picked up this world item
		kRefStateChanged = 111,   // a door/container's open or lock state changed in another player's world
		kPlayerEquipment = 112,   // what another player is wearing and holding
		kActorOwners = 113,       // who runs these NPCs' AI (0 = nobody)
		kActorStatesRelay = 114,  // NPC positions from their owners
		kPlayerDamaged = 115,     // an NPC hit you in another player's world
		kQuestStage = 116,        // a quest reached a new stage in another player's game
		kShotFired = 117,         // another player (or an NPC they run) fired a weapon
		kPlayerStatus = 118,      // another player's location, health and level
		kWorldTime = 119,         // the session's time of day and weather
		kPinged = 120,            // another player pinged their position
		kPartyXp = 121,           // another player got XP for a kill
		kNpcEquipment = 128,      // what an NPC run by another player is wearing and holding
		kPlayerFace = 129,        // another player's face and body shape
		kWorkshopItem = 130,      // a workshop object another player placed, moved or scrapped
		kWorkshopItems = 131,     // several of those at once (a joiner's catch-up)
		kExplosion = 132,         // an explosion from another player's throw
		kActivation = 133,        // another player pressed one
		kSpawned = 134,           // an NPC another player's game made at runtime: place a copy of it
		kRenamed = 135,           // another player's name changed
		kLoadOrder = 136,         // the session's load order (the host's): after a hello whose load order differs, or when asked
		kHeartbeatAck = 122,
		kQuestDone = 123,         // another player completed a quest
		kRevived = 124,           // another player helped you up
		kVoiceRelay = 125,        // a piece of another player's voice
		kMarkersFound = 126,      // map markers another player discovered (playerId 0: catching up)
		kLineSpoken = 127,        // another player (or an NPC they run) said something
	};

	enum StateFlags : std::uint8_t
	{
		kNone = 0,
		kRunning = 1 << 0,
		kSneaking = 1 << 1,
		kWeaponDrawn = 1 << 2,
		kInAir = 1 << 3,      // off the ground (jumping or falling)
		kJumping = 1 << 4,    // left the ground by jumping rather than falling
		kSwimming = 1 << 5,
		kOwnDirection = 1 << 6,  // an NPC's direction is its owner's graph's own (ActorState::direction)
		kOwnGait = 1 << 7,       // an NPC's gait is its owner's graph's own (ActorState::gait and gaitRate), a person's or a creature's
	};

	// A player's transform and movement, sampled on the owning client.
	struct PlayerState
	{
		std::uint32_t sequence = 0;    // increases per sample; receivers drop older ones
		std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;  // radians
		float         speed = 0;     // units/second, for picking walk/run animations
		std::uint16_t moveMode = 0;  // ActorState::moveMode bits (walk/run/sprint/sneak), copied to the puppet
		std::uint8_t  flags = kNone;
		// The sender's clock when it was sampled, in ms (any start; it wraps after 49 days), so the receiver can
		// place it in time however late it came; 0 when not known.
		std::uint32_t sentMs = 0;
		// Where the owner goes and looks: their graph's Direction, travel relative to facing in 256ths of a turn
		// (0 forward, 64 right, 128 back, 192 left), and the pitch they aim at, up (+) or down (-) in degrees.
		std::uint8_t direction = 0;
		std::int8_t  pitch = 0;

		[[nodiscard]] bool IsFinite() const
		{
			return InWorld(x) && InWorld(y) && InWorld(z) &&
			       std::isfinite(heading) && std::isfinite(speed);
		}
	};

	// PlayerState::direction from the graph's Direction (a fraction of a turn), and back.
	inline std::uint8_t PackDirection(float a_turns)
	{
		if (!std::isfinite(a_turns)) {
			return 0;
		}
		return static_cast<std::uint8_t>(std::lround((a_turns - std::floor(a_turns)) * 256.0f) & 0xFF);
	}
	inline float UnpackDirection(std::uint8_t a_direction)
	{
		return static_cast<float>(a_direction) / 256.0f;
	}

	// PlayerState::pitch from a pitch in degrees, held to a quarter turn either way.
	inline std::int8_t PackPitch(float a_degrees)
	{
		return std::isfinite(a_degrees) ? static_cast<std::int8_t>(std::clamp(std::lround(a_degrees), -90L, 90L)) : std::int8_t{ 0 };
	}

	// PlayerState::sentMs for a moment on the sender's steady clock (never 0, which means not known).
	inline std::uint32_t SentMs(std::chrono::steady_clock::time_point a_at)
	{
		const auto ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(a_at.time_since_epoch()).count());
		return ms ? ms : 1;
	}

	// Which of a session's container changes a player's world already contains: the changes
	// before containerFrom, if sessionId is the server's current session (otherwise none).
	struct WorldRequest
	{
		std::uint64_t sessionId = 0;
		std::uint32_t containerFrom = 0;
	};

	struct Hello
	{
		std::uint32_t magic = MAGIC;
		std::uint16_t version = VERSION;
		std::uint32_t contentHash = 0;  // hash of the client's load order; all players must match
		std::string   name;
		std::string   password;
		std::uint32_t appearance = 0;  // NPC base form others should use for this player (0 = their default)
		WorldRequest  world;
		// Random, kept on the player's PC: the server gives a player who comes back the same player
		// ID, so their own changes to the world are still known as theirs. 0 = none.
		std::uint64_t identity = 0;
		// The load order (plugin file names, in order: ComparedLoadOrder), so a refusal can say which plugins differ
		// and a joiner can be sent the session's to compare with its own (kLoadOrder). At most MAX_PLUGINS.
		std::vector<std::string> plugins;
	};

	// The session's load order as the server has it (the host's hello's, or a dedicated server's first arrival's), for a
	// joiner to compare with its own (src/net/ModDiff.h): sent before a refusal for the load order, before the welcome
	// of a joiner whose list differs only in plugins the host lets differ, and when a player asks (kRequestLoadOrder).
	// Information only: whether a joiner is let in is still decided by the hello's hash.
	struct LoadOrder
	{
		std::vector<std::string> plugins;  // as Hello::plugins
		// The plugins the host lets differ (sIgnoredPlugins: SameLoadOrderIgnoring), for the joiner's game to leave out
		// as the server does. At most MAX_IGNORED_PLUGINS.
		std::vector<std::string> ignored;

		bool operator==(const LoadOrder&) const = default;
	};

	// kRequestLoadOrder: the type alone.
	struct LoadOrderRequest
	{};

	struct Welcome
	{
		std::uint32_t playerId = 0;
		std::uint64_t sessionId = 0;  // random per server run
		bool          friendlyFire = false;  // players can hurt each other
		bool          sharedStory = false;   // main story and faction quests are shared too, not just side quests
	};

	struct Reject
	{
		std::string reason;
	};

	struct PlayerJoined
	{
		std::uint32_t playerId = 0;
		std::string   name;
		std::uint32_t appearance = 0;
	};

	struct PlayerLeft
	{
		std::uint32_t playerId = 0;
	};

	// A player's name changed after their hello: a joiner who connected before naming a new character in the
	// opening (Vault 111) sends it once the name is there. kRename: playerId is ignored client -> server; kRenamed:
	// whose name it is, cleaned by the server as a hello's (common/Name.h).
	struct Rename
	{
		std::uint32_t playerId = 0;
		std::string   name;

		bool operator==(const Rename&) const = default;
	};

	// Actors are identified by reference form ID, which is the same for everyone because the
	// load order must match. Only references from plugin files (not runtime-created ones,
	// whose IDs start with 0xFF and differ per game) are synced.
	struct ActorDeath
	{
		std::uint32_t refId = 0;
		std::uint32_t playerId = 0;  // who reported it (set by the server)
		bool          killed = false;  // that player killed it
		float         x = 0, y = 0, z = 0;  // where the corpse lies in the reporter's game (all zero: unknown)
		// Found already dead in the reporter's save, not seen dying (the host's game reports those: WorldSync's
		// CheckFoundDead). Nobody killed it there and it fell nowhere known, so killed is false and the place
		// all zero. Only such a report, from the host's game, counts over the NPC's runner (Server's handleDeath).
		bool          foundDead = false;

		bool operator==(const ActorDeath&) const = default;
	};

	// Health as a fraction of maximum: leveled actors can have different max health in each
	// player's world, so absolute values don't transfer.
	struct ActorHealth
	{
		std::uint32_t refId = 0;
		float         health = 0.0f;  // 0..1
	};

	// Items added to (count > 0) or removed from (count < 0) a container or corpse.
	struct ContainerChange
	{
		std::uint32_t container = 0;
		std::uint32_t item = 0;  // base object form ID
		std::int32_t  count = 0;
	};

	// A container change as the server relays it: numbered in session order, with who made it.
	struct IndexedContainerChange
	{
		std::uint32_t   index = 0;
		std::uint32_t   playerId = 0;
		ContainerChange change;
	};

	struct RefPickedUp
	{
		std::uint32_t refId = 0;
	};

	// Open and lock state of a door (or anything else that opens) or a lockable container.
	struct RefState
	{
		enum Value : std::uint8_t
		{
			kNo = 0,
			kYes = 1,
			kUnknown = 2,  // the reference has no such state; leave it alone
		};

		std::uint32_t refId = 0;
		std::uint8_t  open = kUnknown;
		std::uint8_t  locked = kUnknown;
		std::uint32_t playerId = 0;  // who reported it (set by the server); 0 in a world state

		// The same change, whoever reported it.
		[[nodiscard]] bool SameState(const RefState& a_other) const { return refId == a_other.refId && open == a_other.open && locked == a_other.locked; }
		bool operator==(const RefState&) const = default;
	};

	inline constexpr std::size_t MAX_EQUIPMENT = 32;
	// NPCs one player's game may run at once (a busy cell is a few dozen); one client cannot fill the owner table.
	inline constexpr std::size_t MAX_OWNED_PER_PLAYER = 4096;

	// Mods (OMOD forms) one item may carry: a weapon has a handful of attach points, a power armor piece a few more.
	inline constexpr std::size_t MAX_ITEM_MODS = 32;

	// One mod on the owner's copy of an item, as the engine keeps it there (BGSMod::ObjectIndexData): the mod, the
	// attach point it is in and its rank.
	struct ItemMod
	{
		std::uint32_t mod = 0;
		std::uint8_t  index = 0;
		std::uint8_t  rank = 0;

		bool operator==(const ItemMod&) const = default;
	};

	// The mods on the owner's copy of one item they wear or hold (its BGSObjectInstanceExtra), so the stand-in's copy is
	// built with the same barrel, receiver and sights. Without them the receiver's game rolls the item's object template
	// for its own level, and a stock 10mm shows up with a long barrel. templateIndex is the extra's own item index.
	struct ItemMods
	{
		std::uint32_t        item = 0;
		std::uint16_t        templateIndex = 0;
		std::vector<ItemMod> mods;

		bool operator==(const ItemMods&) const = default;
	};

	// Base forms of a player's equipped armor and weapons, and the mods on those that carry any (an item without an
	// entry has none on record: the stand-in's copy is given the item's default combination). playerId is ignored
	// client -> server.
	struct Equipment
	{
		std::uint32_t              playerId = 0;
		std::vector<std::uint32_t> items;
		std::vector<ItemMods>      mods;

		bool operator==(const Equipment&) const = default;
	};

	// A player's face and body shape, as the game stores it on their character's base NPC: head
	// parts (hair, eyes, beard, scars, ...), the complexion, sculpting sliders (by region and by
	// morph), bone sculpting, make-up and skin tints, and the body shape triangle. The others build
	// their stand-in's look from it.
	inline constexpr std::size_t MAX_FACE_HEAD_PARTS = 24;
	inline constexpr std::size_t MAX_FACE_SLIDERS = 256;
	inline constexpr std::size_t MAX_FACE_MORPHS = 256;
	inline constexpr std::size_t MAX_FACE_BONES = 256;
	inline constexpr std::size_t MAX_FACE_TINTS = 96;
	// LooksMenu's body sliders (F4EE body morphs), for players who have it: a morph's name as BodySlide
	// built it into the body's meshes, and its strength. Only that many, with names that long, travel; a
	// value is brought into the range below (LooksMenu's own sliders stay well inside it), so a modified
	// game cannot stretch a friend's stand-in out of shape in every game.
	inline constexpr std::size_t MAX_BODY_MORPHS = 128;
	inline constexpr std::size_t MAX_BODY_MORPH_NAME = 64;
	inline constexpr float       MAX_BODY_MORPH_VALUE = 2.0f;

	// A workshop object (a wall, a turret, a crop...) placed, moved or scrapped by a player. refId is
	// the object's reference ID in the sender's game (runtime IDs differ per game, so receivers map
	// (playerId, refId) to their own copy). workshop is the settlement's workbench reference.
	namespace WorkshopOp
	{
		inline constexpr std::uint8_t kPlaced = 0;  // placed, or moved (same message, new transform)
		inline constexpr std::uint8_t kScrapped = 1;
	}

	// An explosion the sender's game saw after one of its throws (a grenade, a mine, a Molotov): the
	// others set off the same explosion at the same spot, since a thrown projectile cannot be launched
	// for a stand-in (the engine's throw action plays the animation only).
	struct Explosion
	{
		std::uint32_t playerId = 0;  // who reported it (set by the server)
		std::uint32_t base = 0;      // the BGSExplosion form
		float         x = 0.0f, y = 0.0f, z = 0.0f;
		std::uint32_t cell = 0;        // interior cell, or 0 outside
		std::uint32_t worldspace = 0;

		bool operator==(const Explosion&) const = default;
	};

	// A button, switch or lever the sender pressed: the others press the same one, so what it drives
	// (a door, an elevator, a light) happens in their games too. Only activators travel; doors,
	// containers and pickups have their own messages.
	struct Activation
	{
		std::uint32_t playerId = 0;  // who pressed it (set by the server)
		std::uint32_t refId = 0;

		bool operator==(const Activation&) const = default;
	};

	struct WorkshopItem
	{
		std::uint32_t playerId = 0;
		std::uint32_t refId = 0;
		std::uint8_t  op = WorkshopOp::kPlaced;
		std::uint32_t base = 0;
		std::uint32_t workshop = 0;
		float         position[3] = { 0.0f, 0.0f, 0.0f };
		float         rotation[3] = { 0.0f, 0.0f, 0.0f };
		float         scale = 1.0f;

		bool operator==(const WorkshopItem&) const = default;
	};

	struct FaceBone
	{
		std::uint32_t region = 0;
		float         position[3] = {};
		float         rotation[3] = {};
		float         scale[3] = {};

		bool operator==(const FaceBone&) const = default;
	};

	struct FaceTint
	{
		std::uint16_t templateId = 0;  // the race's tint template entry (uniqueID)
		std::uint8_t  type = 0;        // 0 mask, 1 palette, 2 texture set
		std::uint8_t  value = 0;       // strength 0..100 (tintingValue)
		std::uint32_t color = 0;       // palette: tinting colour (ARGB)
		std::uint16_t swatch = 0;      // palette: swatch ID

		bool operator==(const FaceTint&) const = default;
	};

	// One of the character menu's sculpting sliders (a race morph slider, by its unique ID) and where it
	// was left, as the base NPC keeps them (morphSliderValues; the region sliders are a list apart).
	struct FaceMorph
	{
		std::uint32_t id = 0;
		float         value = 0.0f;

		bool operator==(const FaceMorph&) const = default;
	};

	struct BodyMorph
	{
		std::string name;  // the morph (a BodySlide slider), at most MAX_BODY_MORPH_NAME long
		float       value = 0.0f;

		bool operator==(const BodyMorph&) const = default;
	};

	struct Face
	{
		std::uint32_t              playerId = 0;  // ignored client -> server
		bool                       female = false;
		std::uint32_t              race = 0;  // form ID; the look only applies to a stand-in of the same race
		std::vector<std::uint32_t> headParts;  // BGSHeadPart form IDs
		std::uint32_t              hairColor = 0;        // BGSColorForm, 0 = none
		std::uint32_t              facialHairColor = 0;  // BGSColorForm, 0 = none
		std::uint32_t              faceDetails = 0;      // BGSTextureSet: the complexion (skin detail), 0 = the race's default
		float                      weight[3] = {};       // thin, muscular, fat
		std::vector<float>         sliders;  // morph region slider values, by index
		std::vector<FaceMorph>     morphs;   // morph slider values, by slider ID
		std::vector<FaceBone>      bones;
		std::vector<FaceTint>      tints;
		std::uint8_t               bodyTint[4] = {};  // RGBA
		// LooksMenu's body sliders, sorted by name; empty when the sender has no LooksMenu (or never moved one).
		// A receiver without LooksMenu leaves them be.
		std::vector<BodyMorph>     bodyMorphs;

		bool operator==(const Face&) const = default;
	};

	// An NPC's armor and weapons, from the player who runs it (each game rolls NPC gear on its own,
	// so without this a raider has a different weapon in every game). playerId is ignored client ->
	// server; the server relays only the owner's.
	struct NpcEquipment
	{
		std::uint32_t              playerId = 0;
		std::uint32_t              refId = 0;
		std::vector<std::uint32_t> items;

		bool operator==(const NpcEquipment&) const = default;
	};

	// NPC AI ownership: one player's game runs each NPC; the others copy its movement.
	inline constexpr std::size_t MAX_ACTORS_PER_PACKET = 64;

	enum class ClaimReason : std::uint8_t
	{
		kUnowned = 0,    // the sender has it loaded; run it if nobody does
		kInteract = 1,   // the sender is fighting or talking to it
		kCompanion = 2,  // it follows the sender
	};

	struct ActorClaim
	{
		std::uint32_t refId = 0;
		ClaimReason   reason = ClaimReason::kUnowned;
		// The sender's game won't let it die (essential: at the end of its health it goes down and gets up), so a
		// death the session has for it was never applied there. From the host's game, this claim undoes a death an
		// older build saved by mistake (Dogmeat went down and was taken for dead): Server's handleClaims.
		bool          essential = false;
	};

	struct ActorOwner
	{
		std::uint32_t refId = 0;
		std::uint32_t playerId = 0;  // 0 = nobody
	};

	// An NPC the sender's game made at runtime (see IsSpawnRef), as it was when shared: what to place a copy from, and
	// where. playerId is its owner (ignored client -> server; the server sets it). Its owner's states move the copies on.
	struct Spawn
	{
		std::uint32_t playerId = 0;
		std::uint32_t refId = 0;       // the spawn id
		std::uint32_t base = 0;        // the NPC to place: one from a plugin file, never a runtime one
		std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;

		bool operator==(const Spawn&) const = default;
	};

	// ActorState::aimPitch while the owner's copy aims at nothing: a value PackPitch never gives.
	inline constexpr std::int8_t NO_AIM = -128;

	struct ActorState
	{
		std::uint32_t refId = 0;
		std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;
		float         speed = 0;  // what its legs play (0 while its graph stands, however it is carried: MirrorGait::Reported)
		std::uint16_t moveMode = 0;
		std::uint8_t  flags = kNone;  // StateFlags
		std::uint8_t  direction = 0;  // with kOwnDirection: the owner's graph's Direction, as PlayerState::direction
		// With kOwnGait: the state its owner's graph moves in, as that graph numbers it (MirrorGait.h): a person's walk, jog
		// or run (its weapon-drawn walk or run while the weapon is out), a dog's walk, trot or run (CreatureGait.h), another
		// creature's locomotion range.
		std::uint8_t  gait = 0;
		std::uint8_t  gaitRate = 0;  // with kOwnGait: the rate that state's clip plays at there, in 64ths (0: not known)
		// The pitch its owner's copy aims its gun at, up (+) or down (-) in degrees (AimPoint.h), or NO_AIM while that
		// copy has no aim point. The flags have no bit left to say which.
		std::int8_t aimPitch = NO_AIM;

		[[nodiscard]] bool IsFinite() const
		{
			return InWorld(x) && InWorld(y) && InWorld(z) && std::isfinite(heading) && std::isfinite(speed);
		}
	};

	// ActorState::aimPitch from the pitch an NPC aims at in degrees (held to a quarter turn either way), or from none.
	inline std::int8_t PackAimPitch(std::optional<float> a_degrees)
	{
		return a_degrees && std::isfinite(*a_degrees) ? PackPitch(*a_degrees) : NO_AIM;
	}

	// The pitch an ActorState says its owner's copy aims at, or none (NO_AIM, or a value no sender makes).
	inline std::optional<float> UnpackAimPitch(std::int8_t a_aimPitch)
	{
		if (a_aimPitch < -90 || a_aimPitch > 90) {
			return std::nullopt;
		}
		return static_cast<float>(a_aimPitch);
	}

	// kPlayerHit: playerId is the victim; byPlayer = the sender hit them (friendly fire), not an
	// NPC in the sender's world. kPlayerDamaged: playerId is the sender of the kPlayerHit.
	// attacker: the NPC that hit them (0 when byPlayer); the victim checks it's there in their world.
	struct PlayerHit
	{
		std::uint32_t playerId = 0;
		float         damage = 0.0f;
		bool          byPlayer = false;
		std::uint32_t attacker = 0;
	};

	// What a weapon-holding actor just did (a shot, by default), for the animation on their stand-in.
	namespace ShotAction
	{
		inline constexpr std::uint8_t kShot = 0;      // fired, or swung a melee weapon
		inline constexpr std::uint8_t kReload = 1;
		inline constexpr std::uint8_t kAimStart = 2;  // aiming down the sights
		inline constexpr std::uint8_t kAimStop = 3;
		inline constexpr std::uint8_t kLightOn = 4;   // the Pip-Boy light (players only)
		inline constexpr std::uint8_t kLightOff = 5;
		inline constexpr std::uint8_t kPoint = 6;  // emotes (players only): an idle played on the stand-in
		inline constexpr std::uint8_t kCheer = 7;
		inline constexpr std::uint8_t kClap = 8;
		inline constexpr std::uint8_t kThrow = 9;  // a grenade or mine thrown (the stand-in throws whatever grenade it holds)
		inline constexpr std::uint8_t kMax = kThrow;
	}

	// A weapon shot (or another ShotAction), for the animation. refId 0 = the player themselves,
	// otherwise an NPC they run. playerId is ignored client -> server.
	struct Shot
	{
		std::uint32_t playerId = 0;
		std::uint32_t refId = 0;
		std::uint8_t  action = ShotAction::kShot;
		// The sound the shooter's game picked for their gun (a keyword sound mapping's form ID, chosen
		// by the gun's mods), 0 if none: the receiver's copy of the gun has none of those mods.
		std::uint32_t sound = 0;
	};

	// The power armor frame a player got out of last (their game's last used one), while they are out of it: where it
	// stands in their world, so friends show a copy of it there, parked at a station or wherever they left it.
	struct ParkedFrame
	{
		std::uint32_t base = 0;        // its base form (furniture that is a frame)
		std::uint32_t ref = 0;         // the frame itself when every game has it (a pre-placed one), 0 for one made in their game
		std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;  // radians

		bool operator==(const ParkedFrame&) const = default;

		[[nodiscard]] bool Valid() const
		{
			return base != 0 && (ref == 0 || IsShareableRef(ref)) && ((cell != 0) != (worldspace != 0)) && InWorld(x) && InWorld(y) && InWorld(z) &&
			       std::isfinite(heading);
		}
	};

	// The furniture a player sits, lies or works in (a chair, a bed, a workbench, a terminal; never a power armor frame nor
	// a seat on an actor such as a vertibird), so friends put their stand-in in the same one.
	struct Seat
	{
		std::uint32_t ref = 0;   // the furniture itself when every game has it (a pre-placed one), 0 for one made in their game
		std::uint32_t base = 0;  // its base form: one made in their game (a settlement's chair) is found by base and place
		float         x = 0, y = 0, z = 0;  // where it stands in their world

		bool operator==(const Seat&) const = default;

		[[nodiscard]] bool Valid() const
		{
			return base != 0 && (ref == 0 || IsShareableRef(ref)) && InWorld(x) && InWorld(y) && InWorld(z);
		}
	};

	// playerId is ignored client -> server in all of these.
	struct PlayerStatus
	{
		std::uint32_t playerId = 0;
		std::uint32_t location = 0;  // BGSLocation form ID, or 0
		std::uint32_t cell = 0;      // interior cell form ID, or 0 when outside
		std::uint8_t  health = 0;    // percent of maximum; 0 = dead or downed
		std::uint16_t level = 0;
		bool          downed = false;  // knocked down, waiting for a friend to help them up
		bool          opening = false;  // still in the game's opening (before leaving Vault 111)
		bool          light = false;    // Pip-Boy light on
		bool          powerArmor = false;     // in power armor
		std::uint32_t powerArmorFrame = 0;   // base form of the frame they are in (0 = none): the hearer places one for the stand-in
		// The frame reference itself when every game has it (placed in the world by a plugin, such as the museum's suit
		// in Concord), 0 for one made in their game: the hearer hides its own copy of that frame while they wear theirs.
		std::uint32_t powerArmorFrameRef = 0;
		// Out of power armor: the frame they got out of last, where it stands now (none while they are in one).
		std::optional<ParkedFrame> parked;
		// The furniture they are in or getting into (protocol 31); none while they stand, and in power armor.
		std::optional<Seat> seat;

		bool operator==(const PlayerStatus&) const = default;
	};

	// playerId: whose clock it is (ignored client -> server).
	struct WorldTime
	{
		std::uint32_t playerId = 0;
		float         gameHour = 0.0f;    // 0..24
		float         daysPassed = 0.0f;  // GameDaysPassed: tells a long sleep from a short one
		std::uint32_t worldspace = 0;     // where the weather applies (0 = inside)
		std::uint32_t weather = 0;        // TESWeather form ID, or 0
	};

	struct Ping
	{
		std::uint32_t playerId = 0;
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0, y = 0, z = 0;
	};

	// A line of dialogue. speaker: the NPC who said it, or 0 for the player (playerId) themselves.
	// playerId is ignored client -> server. info and response name the recorded line (its voice file)
	// and voiceType the voice it is said in: the hearer plays that file at its copy of the speaker. All
	// three are 0 when the speaker's game couldn't place the line; it is then shown as text only.
	struct Line
	{
		std::uint32_t playerId = 0;
		std::uint32_t speaker = 0;
		std::string   text;
		std::uint32_t info = 0;       // the TOPIC_INFO the line is from
		std::uint8_t  response = 0;   // its response number, 1 and up (the "_1" of the voice file)
		std::uint32_t voiceType = 0;  // the speaker's voice type (for a player, the male or female player voice)
	};

	struct Heartbeat
	{
		std::uint32_t token = 0;
	};

	inline constexpr float MAX_XP_SHARE = 10000.0f;

	struct XpGain
	{
		std::uint32_t playerId = 0;
		float         xp = 0.0f;
	};

	inline constexpr std::size_t MAX_VOICE_BYTES = 4000;
	inline constexpr std::size_t MAX_MARKERS_PER_PACKET = 200;
	inline constexpr std::size_t MAX_SESSION_MARKERS = 8192;

	struct MapMarker
	{
		std::uint32_t refId = 0;
		std::uint8_t  flags = 0;  // 1 = on the map, 2 = discovered (fast travel)
	};

	// playerId is ignored client -> server.
	struct MarkersFound
	{
		std::uint32_t          playerId = 0;
		std::vector<MapMarker> markers;
	};

	// Compressed voice as Steam produces it. playerId is ignored client -> server.
	struct VoiceData
	{
		std::uint32_t             playerId = 0;
		std::vector<std::uint8_t> data;
	};

	// kRevive: playerId is the downed player. kRevived: playerId is the helper.
	struct Revive
	{
		std::uint32_t playerId = 0;
	};

	// playerId is ignored client -> server.
	struct QuestDone
	{
		std::uint32_t playerId = 0;
		std::uint32_t quest = 0;
	};

	// playerId is ignored client -> server (the server stamps who reported it), and not kept in a WorldState.
	struct QuestStage
	{
		std::uint32_t quest = 0;
		std::uint16_t stage = 0;
		std::uint32_t playerId = 0;

		bool operator==(const QuestStage&) const = default;
	};

	// Everything that happened this session, sent on join and after loading a save. Large states
	// are split over several packets; each is applied on top of the previous ones.
	struct WorldState
	{
		std::vector<std::uint32_t>    deadActors;
		std::uint32_t                       containerFirstIndex = 0;  // session index of containerChanges[0]
		std::vector<IndexedContainerChange> containerChanges;  // in order (the index isn't sent)
		std::vector<std::uint32_t>    pickedUp;
		std::vector<RefState>         refStates;  // latest per reference
		std::vector<QuestStage>       questStages;  // in order
	};

	inline constexpr std::size_t MAX_WORLD_STATE_ACTORS = 60000;
	// Entries per list in one WorldState packet (keeps packets well under transport limits).
	inline constexpr std::size_t WORLD_STATE_CHUNK = 4000;

	struct RemoteState
	{
		std::uint32_t playerId = 0;
		PlayerState   state;
	};

	struct PlayerStates
	{
		std::uint32_t            serverTick = 0;
		std::vector<RemoteState> players;
	};

	// ---- serialization -------------------------------------------------------------

	class Writer
	{
	public:
		explicit Writer(MessageType a_type) { U8(static_cast<std::uint8_t>(a_type)); }

		void U8(std::uint8_t a_value) { buffer.push_back(a_value); }
		void U16(std::uint16_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void U32(std::uint32_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void U64(std::uint64_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void F32(float a_value) { Raw(&a_value, sizeof(a_value)); }

		void Str(std::string_view a_value, std::size_t a_maxLength)
		{
			const auto length = (std::min)(a_value.size(), (std::min<std::size_t>)(a_maxLength, 255));
			U8(static_cast<std::uint8_t>(length));
			Raw(a_value.data(), length);
		}

		// A string longer than Str's 255 bytes may be (a plugin's file name): a U16 length, then the bytes.
		void LongStr(std::string_view a_value, std::size_t a_maxLength)
		{
			const auto length = (std::min)(a_value.size(), (std::min<std::size_t>)(a_maxLength, 0xFFFF));
			U16(static_cast<std::uint16_t>(length));
			Raw(a_value.data(), length);
		}

		// A length-prefixed byte string (at most 65535 bytes).
		void Bytes(std::span<const std::uint8_t> a_value)
		{
			const auto length = (std::min<std::size_t>)(a_value.size(), 0xFFFF);
			U16(static_cast<std::uint16_t>(length));
			Raw(a_value.data(), length);
		}

		[[nodiscard]] const std::vector<std::uint8_t>& Data() const { return buffer; }

	private:
		void Raw(const void* a_data, std::size_t a_size)
		{
			const auto bytes = static_cast<const std::uint8_t*>(a_data);
			buffer.insert(buffer.end(), bytes, bytes + a_size);
		}

		std::vector<std::uint8_t> buffer;
	};

	// Reads values from a packet. Any read past the end sets the failed flag and
	// returns zero, so decoders can read everything and check Ok() once at the end.
	class Reader
	{
	public:
		explicit Reader(std::span<const std::uint8_t> a_data) :
			data(a_data)
		{}

		std::uint8_t  U8() { return Read<std::uint8_t>(); }
		std::uint16_t U16() { return Read<std::uint16_t>(); }
		std::uint32_t U32() { return Read<std::uint32_t>(); }
		std::uint64_t U64() { return Read<std::uint64_t>(); }
		float         F32() { return Read<float>(); }

		std::string Str(std::size_t a_maxLength)
		{
			const auto length = U8();
			if (length > a_maxLength || offset + length > data.size()) {
				failed = true;
				return {};
			}
			std::string out(reinterpret_cast<const char*>(data.data() + offset), length);
			offset += length;
			// Every string on the wire is text someone reads (a name, a reason, a line): control
			// characters are dropped here, before any of it reaches a HUD, a log or a status line.
			std::erase_if(out, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
			return out;
		}

		// Writer::LongStr's string, its control characters dropped as Str's are.
		std::string LongStr(std::size_t a_maxLength)
		{
			const auto length = U16();
			if (failed || length > a_maxLength || offset + length > data.size()) {
				failed = true;
				return {};
			}
			std::string out(reinterpret_cast<const char*>(data.data() + offset), length);
			offset += length;
			std::erase_if(out, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
			return out;
		}

		std::vector<std::uint8_t> Bytes(std::size_t a_maxLength)
		{
			const auto length = U16();
			if (failed || length > a_maxLength || offset + length > data.size()) {
				failed = true;
				return {};
			}
			std::vector<std::uint8_t> out(data.begin() + offset, data.begin() + offset + length);
			offset += length;
			return out;
		}

		[[nodiscard]] bool Ok() const { return !failed; }
		[[nodiscard]] bool AtEnd() const { return offset == data.size(); }

	private:
		template <class T>
		T Read()
		{
			if (failed || offset + sizeof(T) > data.size()) {
				failed = true;
				return T{};
			}
			T value;
			std::memcpy(&value, data.data() + offset, sizeof(T));
			offset += sizeof(T);
			return value;
		}

		std::span<const std::uint8_t> data;
		std::size_t                   offset = 0;
		bool                          failed = false;
	};

	inline std::optional<MessageType> PeekType(std::span<const std::uint8_t> a_data)
	{
		if (a_data.empty()) {
			return std::nullopt;
		}
		return static_cast<MessageType>(a_data[0]);
	}

	// Each Encode returns a complete packet; each Decode expects one (including the type byte)
	// and returns nullopt if it is malformed.

	inline void WriteState(Writer& a_writer, const PlayerState& a_state)
	{
		a_writer.U32(a_state.sequence);
		a_writer.U32(a_state.cell);
		a_writer.U32(a_state.worldspace);
		a_writer.F32(a_state.x);
		a_writer.F32(a_state.y);
		a_writer.F32(a_state.z);
		a_writer.F32(a_state.heading);
		a_writer.F32(a_state.speed);
		a_writer.U16(a_state.moveMode);
		a_writer.U8(a_state.flags);
		a_writer.U32(a_state.sentMs);
		a_writer.U8(a_state.direction);
		a_writer.U8(static_cast<std::uint8_t>(a_state.pitch));
	}

	inline PlayerState ReadState(Reader& a_reader)
	{
		PlayerState state;
		state.sequence = a_reader.U32();
		state.cell = a_reader.U32();
		state.worldspace = a_reader.U32();
		state.x = a_reader.F32();
		state.y = a_reader.F32();
		state.z = a_reader.F32();
		state.heading = a_reader.F32();
		state.speed = a_reader.F32();
		state.moveMode = a_reader.U16();
		state.flags = a_reader.U8();
		state.sentMs = a_reader.U32();
		state.direction = a_reader.U8();
		state.pitch = static_cast<std::int8_t>(a_reader.U8());
		return state;
	}

	// A list of plugins: a U16 count (at most a_max, MAX_PLUGINS for a load order; past it the rest are left off), then
	// each name (LongStr, at most MAX_PLUGIN_NAME_LENGTH bytes).
	inline void WritePlugins(Writer& a_writer, const std::vector<std::string>& a_plugins, std::size_t a_max = MAX_PLUGINS)
	{
		const auto count = (std::min)(a_plugins.size(), a_max);
		a_writer.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			a_writer.LongStr(a_plugins[i], MAX_PLUGIN_NAME_LENGTH);
		}
	}

	// None for more than a_max names or a name too long (the reader has failed then).
	inline std::optional<std::vector<std::string>> ReadPlugins(Reader& a_reader, std::size_t a_max = MAX_PLUGINS)
	{
		const auto count = a_reader.U16();
		if (!a_reader.Ok() || count > a_max) {
			return std::nullopt;
		}
		std::vector<std::string> plugins;
		plugins.reserve(count);
		for (std::uint16_t i = 0; i < count && a_reader.Ok(); ++i) {
			plugins.push_back(a_reader.LongStr(MAX_PLUGIN_NAME_LENGTH));
		}
		if (!a_reader.Ok()) {
			return std::nullopt;
		}
		return plugins;
	}

	inline std::vector<std::uint8_t> Encode(const Hello& a_msg)
	{
		Writer w{ MessageType::kHello };
		w.U32(a_msg.magic);
		w.U16(a_msg.version);
		w.U32(a_msg.contentHash);
		w.Str(a_msg.name, MAX_NAME_LENGTH);
		w.Str(a_msg.password, MAX_PASSWORD_LENGTH);
		w.U32(a_msg.appearance);
		w.U64(a_msg.world.sessionId);
		w.U32(a_msg.world.containerFrom);
		w.U64(a_msg.identity);
		// Where versions before 35 had their list (a byte's count of names of up to 80 bytes): empty, so a server of
		// one of those still reads the hello whole and says the version differs. The list follows.
		w.U8(0);
		WritePlugins(w, a_msg.plugins);
		return w.Data();
	}

	// A hello of another version (or with another magic) is read up to its version only: the rest may be laid out
	// otherwise, and the server needs no more to say the versions differ.
	inline std::optional<Hello> DecodeHello(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Hello msg;
		msg.magic = r.U32();
		msg.version = r.U16();
		if (!r.Ok()) {
			return std::nullopt;
		}
		if (msg.magic != MAGIC || msg.version != VERSION) {
			return msg;
		}
		msg.contentHash = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		msg.password = r.Str(MAX_PASSWORD_LENGTH);
		msg.appearance = r.U32();
		msg.world.sessionId = r.U64();
		msg.world.containerFrom = r.U32();
		msg.identity = r.U64();
		if (r.U8() != 0) {
			return std::nullopt;  // the old list's place, empty since version 35
		}
		auto plugins = ReadPlugins(r);
		if (!plugins || !r.Ok()) {
			return std::nullopt;
		}
		msg.plugins = std::move(*plugins);
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const LoadOrder& a_msg)
	{
		Writer w{ MessageType::kLoadOrder };
		WritePlugins(w, a_msg.plugins);
		WritePlugins(w, a_msg.ignored, MAX_IGNORED_PLUGINS);
		return w.Data();
	}

	inline std::optional<LoadOrder> DecodeLoadOrder(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto plugins = ReadPlugins(r);
		auto ignored = plugins ? ReadPlugins(r, MAX_IGNORED_PLUGINS) : std::nullopt;
		if (!plugins || !ignored || !r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return LoadOrder{ std::move(*plugins), std::move(*ignored) };
	}

	inline std::vector<std::uint8_t> Encode(LoadOrderRequest)
	{
		return Writer{ MessageType::kRequestLoadOrder }.Data();
	}

	inline std::optional<LoadOrderRequest> DecodeLoadOrderRequest(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return LoadOrderRequest{};
	}

	inline std::vector<std::uint8_t> Encode(const WorldRequest& a_msg)
	{
		Writer w{ MessageType::kRequestWorldState };
		w.U64(a_msg.sessionId);
		w.U32(a_msg.containerFrom);
		return w.Data();
	}

	inline std::optional<WorldRequest> DecodeWorldRequest(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		WorldRequest msg;
		msg.sessionId = r.U64();
		msg.containerFrom = r.U32();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerState& a_msg)
	{
		Writer w{ MessageType::kPlayerState };
		WriteState(w, a_msg);
		return w.Data();
	}

	inline std::optional<PlayerState> DecodePlayerState(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto state = ReadState(r);
		if (!r.Ok() || !r.AtEnd() || !state.IsFinite()) {
			return std::nullopt;
		}
		return state;
	}

	inline std::vector<std::uint8_t> Encode(const Welcome& a_msg)
	{
		Writer w{ MessageType::kWelcome };
		w.U32(a_msg.playerId);
		w.U64(a_msg.sessionId);
		w.U8(a_msg.friendlyFire ? 1 : 0);
		w.U8(a_msg.sharedStory ? 1 : 0);
		return w.Data();
	}

	inline std::optional<Welcome> DecodeWelcome(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Welcome msg;
		msg.playerId = r.U32();
		msg.sessionId = r.U64();
		msg.friendlyFire = r.U8() != 0;
		msg.sharedStory = r.U8() != 0;
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}

	// In a hello's load order, between the full plugins and the light ones (.esl files, and plugins flagged light).
	// The two kinds are numbered apart: a full plugin's form IDs start with its place among the full ones (09000004),
	// a light one's with FE and its place among the light ones.
	inline constexpr std::string_view LIGHT_PLUGINS = "--light--";

	// The free high resolution texture pack's plugin. It holds no records, so a player without it can play with one
	// who has it; but it takes a place in the load order like any other plugin.
	inline constexpr std::string_view TEXTURE_PACK = "DLCUltraHighResolution.esm";

	[[nodiscard]] inline bool SamePluginName(std::string_view a_a, std::string_view a_b)
	{
		return std::ranges::equal(a_a, a_b, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
	}

	// The full plugins, in load order, as players' load orders are compared: the texture pack is left out only when no
	// other full plugin loads after it. With mods after it, it moves them all up a place: a story mod's NPC is 09000004
	// with the pack and 08000004 without, and shared by form ID it would be some other mod's form, or nothing, in a
	// friend's game.
	[[nodiscard]] inline std::vector<std::string> ComparedFullPlugins(std::vector<std::string> a_full)
	{
		if (!a_full.empty() && SamePluginName(a_full.back(), TEXTURE_PACK)) {
			a_full.pop_back();
		}
		return a_full;
	}

	// A game's load order as its hello carries it: the full plugins (ComparedFullPlugins), LIGHT_PLUGINS, the light ones.
	[[nodiscard]] inline std::vector<std::string> ComparedLoadOrder(std::vector<std::string> a_full, const std::vector<std::string>& a_light)
	{
		auto out = ComparedFullPlugins(std::move(a_full));
		out.emplace_back(LIGHT_PLUGINS);
		out.insert(out.end(), a_light.begin(), a_light.end());
		return out;
	}

	// FNV-1a over a load order (ComparedLoadOrder), names in any case: the hello's content hash. Players whose hashes
	// match play together without the lists being compared.
	[[nodiscard]] inline std::uint32_t LoadOrderHash(const std::vector<std::string>& a_plugins)
	{
		std::uint32_t hash = 2166136261u;
		for (const auto& name : a_plugins) {
			for (const char c : name) {
				hash ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
				hash *= 16777619u;
			}
			hash ^= '|';
			hash *= 16777619u;
		}
		return hash;
	}

	// The hash older versions sent for a hello's load order: the texture pack left out wherever it loads. A session
	// saved with it is this load order's when it matches, so a host whose pack now counts keeps their shared world.
	// None for a list that is missing or cut short (MAX_PLUGINS).
	[[nodiscard]] inline std::optional<std::uint32_t> LoadOrderHashBeforeTexturePack(const std::vector<std::string>& a_plugins)
	{
		if (a_plugins.empty() || a_plugins.size() >= MAX_PLUGINS) {
			return std::nullopt;
		}
		auto plugins = a_plugins;
		std::erase_if(plugins, [](const std::string& a_name) { return SamePluginName(a_name, TEXTURE_PACK); });
		return LoadOrderHash(plugins);
	}

	// Whether two load orders give every plugin the same form IDs once the ignored plugins (the host's list) are left
	// out of both: the same plugins, in the same order, and each at the same place among its kind (full or light, split
	// by LIGHT_PLUGINS; a list without it is taken as all full). An ignored plugin that only one player has moves every
	// plugin of its kind after it, so it may differ only after the rest: the ones after it would be other forms in each
	// game. False when either list is unknown.
	inline bool SameLoadOrderIgnoring(const std::vector<std::string>& a_theirs, const std::vector<std::string>& a_sessions, const std::vector<std::string>& a_ignored)
	{
		if (a_theirs.empty() || a_sessions.empty()) {
			return false;
		}
		const auto lower = [](std::string a_s) {
			for (char& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		};
		// Each kept plugin: its name, whether it is light, and its place among its kind.
		const auto kept = [&](const std::vector<std::string>& a_names) {
			std::vector<std::tuple<std::string, bool, std::size_t>> out;
			bool                                                    light = false;
			std::size_t                                             place = 0;
			for (const auto& name : a_names) {
				if (name == LIGHT_PLUGINS) {
					light = true;
					place = 0;
					continue;
				}
				if (std::ranges::none_of(a_ignored, [&](const std::string& a_ignore) { return SamePluginName(a_ignore, name); })) {
					out.emplace_back(lower(name), light, place);
				}
				++place;
			}
			return out;
		};
		return kept(a_theirs) == kept(a_sessions);
	}

	// What a player refused for their load order hears: that it differs (REJECT_LOAD_ORDER), and that texture plugins
	// may differ only last when one the host lets differ (or the texture pack) is only in one game. No names: the
	// joiner's game names every plugin that differs (src/net/ModDiff.h) from the session's list, which the server
	// sends just before (kLoadOrder). Fits a Reject.
	inline std::string LoadOrderDifference(const std::vector<std::string>& a_theirs, const std::vector<std::string>& a_sessions, const std::vector<std::string>& a_ignored = {})
	{
		std::string out{ REJECT_LOAD_ORDER };
		if (a_theirs.empty() || a_sessions.empty()) {
			return out + " Everyone needs the same mods in the same order.";  // a list is missing: nothing to compare
		}
		const auto has = [](const std::vector<std::string>& a_names, std::string_view a_name) {
			return std::ranges::any_of(a_names, [&](const std::string& a_other) { return SamePluginName(a_other, a_name); });
		};
		// A plugin that may differ (the texture pack, or one the host lets differ) only one game has: others load after it.
		const auto mayDiffer = [&](const std::string& a_name) {
			return SamePluginName(a_name, TEXTURE_PACK) || has(a_ignored, a_name);
		};
		const auto onlyOne = [&](const std::vector<std::string>& a_in, const std::vector<std::string>& a_notIn) {
			return std::ranges::any_of(a_in, [&](const std::string& a_name) { return mayDiffer(a_name) && !has(a_notIn, a_name); });
		};
		if (onlyOne(a_theirs, a_sessions) || onlyOne(a_sessions, a_theirs)) {
			out += " Texture plugins may differ only when they load last.";
		}
		return out;
	}

	inline std::vector<std::uint8_t> Encode(const Reject& a_msg)
	{
		Writer w{ MessageType::kReject };
		w.Str(a_msg.reason, MAX_REASON_LENGTH);
		return w.Data();
	}

	inline std::optional<Reject> DecodeReject(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Reject msg;
		msg.reason = r.Str(MAX_REASON_LENGTH);
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerJoined& a_msg)
	{
		Writer w{ MessageType::kPlayerJoined };
		w.U32(a_msg.playerId);
		w.Str(a_msg.name, MAX_NAME_LENGTH);
		w.U32(a_msg.appearance);
		return w.Data();
	}

	inline std::optional<PlayerJoined> DecodePlayerJoined(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerJoined msg;
		msg.playerId = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		msg.appearance = r.U32();
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerLeft& a_msg)
	{
		Writer w{ MessageType::kPlayerLeft };
		w.U32(a_msg.playerId);
		return w.Data();
	}

	inline std::optional<PlayerLeft> DecodePlayerLeft(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerLeft msg;
		msg.playerId = r.U32();
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Rename& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.Str(a_msg.name, MAX_NAME_LENGTH);
		return w.Data();
	}

	inline std::optional<Rename> DecodeRename(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Rename msg;
		msg.playerId = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerStates& a_msg)
	{
		Writer w{ MessageType::kPlayerStates };
		w.U32(a_msg.serverTick);
		const auto count = (std::min<std::size_t>)(a_msg.players.size(), 255);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			w.U32(a_msg.players[i].playerId);
			WriteState(w, a_msg.players[i].state);
		}
		return w.Data();
	}

	inline std::optional<PlayerStates> DecodePlayerStates(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerStates msg;
		msg.serverTick = r.U32();
		const auto count = r.U8();
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			RemoteState entry;
			entry.playerId = r.U32();
			entry.state = ReadState(r);
			if (!entry.state.IsFinite()) {
				return std::nullopt;
			}
			msg.players.push_back(entry);
		}
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const ActorDeath& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		w.U32(a_msg.playerId);
		w.U8(a_msg.killed ? 1 : 0);
		w.F32(a_msg.x);
		w.F32(a_msg.y);
		w.F32(a_msg.z);
		w.U8(a_msg.foundDead ? 1 : 0);
		return w.Data();
	}

	inline std::optional<ActorDeath> DecodeActorDeath(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		ActorDeath msg;
		msg.refId = r.U32();
		msg.playerId = r.U32();
		msg.killed = r.U8() != 0;
		msg.x = r.F32();
		msg.y = r.F32();
		msg.z = r.F32();
		msg.foundDead = r.U8() != 0;
		if (!r.Ok() || !r.AtEnd() || !InWorld(msg.x) || !InWorld(msg.y) || !InWorld(msg.z)) {
			return std::nullopt;
		}
		if (msg.foundDead && (msg.killed || msg.x != 0.0f || msg.y != 0.0f || msg.z != 0.0f)) {
			return std::nullopt;  // nobody saw it die
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Explosion& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.base);
		w.F32(a_msg.x);
		w.F32(a_msg.y);
		w.F32(a_msg.z);
		w.U32(a_msg.cell);
		w.U32(a_msg.worldspace);
		return w.Data();
	}

	inline std::optional<Explosion> DecodeExplosion(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Explosion msg;
		msg.playerId = r.U32();
		msg.base = r.U32();
		msg.x = r.F32();
		msg.y = r.F32();
		msg.z = r.F32();
		msg.cell = r.U32();
		msg.worldspace = r.U32();
		if (!r.Ok() || !r.AtEnd() || msg.base == 0 || !std::isfinite(msg.x) || !std::isfinite(msg.y) || !std::isfinite(msg.z)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Activation& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.refId);
		return w.Data();
	}

	inline std::optional<Activation> DecodeActivation(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Activation msg;
		msg.playerId = r.U32();
		msg.refId = r.U32();
		if (!r.Ok() || !r.AtEnd() || !IsShareableRef(msg.refId)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Spawn& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.refId);
		w.U32(a_msg.base);
		w.U32(a_msg.cell);
		w.U32(a_msg.worldspace);
		w.F32(a_msg.x);
		w.F32(a_msg.y);
		w.F32(a_msg.z);
		w.F32(a_msg.heading);
		return w.Data();
	}

	// A spawn id, a base every game has, a place in the world, and in one space: an interior or a worldspace.
	inline std::optional<Spawn> DecodeSpawn(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Spawn msg;
		msg.playerId = r.U32();
		msg.refId = r.U32();
		msg.base = r.U32();
		msg.cell = r.U32();
		msg.worldspace = r.U32();
		msg.x = r.F32();
		msg.y = r.F32();
		msg.z = r.F32();
		msg.heading = r.F32();
		if (!r.Ok() || !r.AtEnd() || !IsSpawnRef(msg.refId) || !IsShareableRef(msg.base) || !InWorld(msg.x) || !InWorld(msg.y) || !InWorld(msg.z) ||
			!std::isfinite(msg.heading) || (msg.cell == 0) == (msg.worldspace == 0)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const ActorHealth& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		w.F32(a_msg.health);
		return w.Data();
	}

	inline std::optional<ActorHealth> DecodeActorHealth(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		ActorHealth msg;
		msg.refId = r.U32();
		msg.health = r.F32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.health)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const ContainerChange& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.container);
		w.U32(a_msg.item);
		w.U32(static_cast<std::uint32_t>(a_msg.count));
		return w.Data();
	}

	inline std::optional<ContainerChange> DecodeContainerChange(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		ContainerChange msg;
		msg.container = r.U32();
		msg.item = r.U32();
		msg.count = static_cast<std::int32_t>(r.U32());
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	[[nodiscard]] inline bool IsValid(const ContainerChange& a_change)
	{
		return IsShareableRef(a_change.container) && a_change.item != 0 && a_change.count != 0 &&
		       a_change.count >= -MAX_ITEM_COUNT && a_change.count <= MAX_ITEM_COUNT;
	}

	// kContainerChanged
	inline std::vector<std::uint8_t> Encode(const IndexedContainerChange& a_msg)
	{
		Writer w{ MessageType::kContainerChanged };
		w.U32(a_msg.index);
		w.U32(a_msg.playerId);
		w.U32(a_msg.change.container);
		w.U32(a_msg.change.item);
		w.U32(static_cast<std::uint32_t>(a_msg.change.count));
		return w.Data();
	}

	inline std::optional<IndexedContainerChange> DecodeIndexedContainerChange(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		IndexedContainerChange msg;
		msg.index = r.U32();
		msg.playerId = r.U32();
		msg.change.container = r.U32();
		msg.change.item = r.U32();
		msg.change.count = static_cast<std::int32_t>(r.U32());
		if (!r.Ok() || !r.AtEnd() || !IsValid(msg.change)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const RefPickedUp& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		return w.Data();
	}

	inline std::optional<RefPickedUp> DecodeRefPickedUp(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RefPickedUp msg;
		msg.refId = r.U32();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const RefState& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		w.U8(a_msg.open);
		w.U8(a_msg.locked);
		w.U32(a_msg.playerId);
		return w.Data();
	}

	inline std::optional<RefState> DecodeRefState(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RefState msg;
		msg.refId = r.U32();
		msg.open = r.U8();
		msg.locked = r.U8();
		msg.playerId = r.U32();
		if (!r.Ok() || !r.AtEnd() || msg.open > RefState::kUnknown || msg.locked > RefState::kUnknown) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Equipment& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		const auto count = (std::min)(a_msg.items.size(), MAX_EQUIPMENT);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			w.U32(a_msg.items[i]);
		}
		// The mods of the items sent, one entry each (one for an item left off, or a second for the same item, is not).
		const auto sent = std::span(a_msg.items).first(count);
		std::vector<const ItemMods*> mods;
		for (const auto& entry : a_msg.mods) {
			if (mods.size() < MAX_EQUIPMENT && std::ranges::contains(sent, entry.item) &&
				std::ranges::none_of(mods, [&](const ItemMods* a_other) { return a_other->item == entry.item; })) {
				mods.push_back(&entry);
			}
		}
		w.U8(static_cast<std::uint8_t>(mods.size()));
		for (const auto entry : mods) {
			w.U32(entry->item);
			w.U16(entry->templateIndex);
			const auto modCount = (std::min)(entry->mods.size(), MAX_ITEM_MODS);
			w.U8(static_cast<std::uint8_t>(modCount));
			for (std::size_t i = 0; i < modCount; ++i) {
				w.U32(entry->mods[i].mod);
				w.U8(entry->mods[i].index);
				w.U8(entry->mods[i].rank);
			}
		}
		return w.Data();
	}

	inline std::optional<Equipment> DecodeEquipment(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Equipment msg;
		msg.playerId = r.U32();
		const auto count = r.U8();
		if (count > MAX_EQUIPMENT) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			msg.items.push_back(r.U32());
		}
		const auto modded = r.U8();
		if (modded > MAX_EQUIPMENT) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < modded && r.Ok(); ++i) {
			ItemMods entry;
			entry.item = r.U32();
			entry.templateIndex = r.U16();
			const auto modCount = r.U8();
			// Only for an item the list names, once.
			if (modCount > MAX_ITEM_MODS || !std::ranges::contains(msg.items, entry.item) ||
				std::ranges::any_of(msg.mods, [&](const ItemMods& a_other) { return a_other.item == entry.item; })) {
				return std::nullopt;
			}
			for (std::uint8_t m = 0; m < modCount && r.Ok(); ++m) {
				ItemMod mod;
				mod.mod = r.U32();
				mod.index = r.U8();
				mod.rank = r.U8();
				entry.mods.push_back(mod);
			}
			msg.mods.push_back(std::move(entry));
		}
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	namespace detail
	{
		inline void WriteWorkshopItem(Writer& w, const WorkshopItem& a_msg)
		{
			w.U32(a_msg.playerId);
			w.U32(a_msg.refId);
			w.U8(a_msg.op);
			w.U32(a_msg.base);
			w.U32(a_msg.workshop);
			for (const float v : a_msg.position) {
				w.F32(v);
			}
			for (const float v : a_msg.rotation) {
				w.F32(v);
			}
			w.F32(a_msg.scale);
		}

		// False if the item is not one we would accept.
		inline bool ReadWorkshopItem(Reader& r, WorkshopItem& msg)
		{
			msg.playerId = r.U32();
			msg.refId = r.U32();
			msg.op = r.U8();
			msg.base = r.U32();
			msg.workshop = r.U32();
			bool finite = true;
			for (float& v : msg.position) {
				v = r.F32();
				finite = finite && InWorld(v);  // position and rotation alike: both must be sane numbers
			}
			for (float& v : msg.rotation) {
				v = r.F32();
				finite = finite && std::isfinite(v);
			}
			msg.scale = r.F32();
			finite = finite && std::isfinite(msg.scale) && msg.scale > 0.0f && msg.scale <= 100.0f;
			return r.Ok() && finite && msg.refId != 0 && msg.op <= WorkshopOp::kScrapped && (msg.op != WorkshopOp::kPlaced || (msg.base != 0 && (msg.base >> 24) != 0xFF));
		}
	}

	inline std::vector<std::uint8_t> Encode(const WorkshopItem& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		detail::WriteWorkshopItem(w, a_msg);
		return w.Data();
	}

	inline std::optional<WorkshopItem> DecodeWorkshopItem(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		WorkshopItem msg;
		if (!detail::ReadWorkshopItem(r, msg) || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// kWorkshopItems: up to MAX_ACTORS_PER_PACKET items in one packet (a joiner's catch-up).
	inline std::vector<std::uint8_t> Encode(const std::vector<WorkshopItem>& a_msg)
	{
		Writer     w{ MessageType::kWorkshopItems };
		const auto count = (std::min)(a_msg.size(), MAX_ACTORS_PER_PACKET);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			detail::WriteWorkshopItem(w, a_msg[i]);
		}
		return w.Data();
	}

	inline std::optional<std::vector<WorkshopItem>> DecodeWorkshopItems(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_ACTORS_PER_PACKET) {
			return std::nullopt;
		}
		std::vector<WorkshopItem> items(count);
		for (auto& item : items) {
			if (!detail::ReadWorkshopItem(r, item)) {
				return std::nullopt;
			}
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return items;
	}

	inline std::vector<std::uint8_t> Encode(const Face& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U8(a_msg.female ? 1 : 0);
		w.U32(a_msg.race);
		const auto parts = (std::min)(a_msg.headParts.size(), MAX_FACE_HEAD_PARTS);
		w.U8(static_cast<std::uint8_t>(parts));
		for (std::size_t i = 0; i < parts; ++i) {
			w.U32(a_msg.headParts[i]);
		}
		w.U32(a_msg.hairColor);
		w.U32(a_msg.facialHairColor);
		w.U32(a_msg.faceDetails);
		for (const float v : a_msg.weight) {
			w.F32(v);
		}
		const auto sliders = (std::min)(a_msg.sliders.size(), MAX_FACE_SLIDERS);
		w.U16(static_cast<std::uint16_t>(sliders));
		for (std::size_t i = 0; i < sliders; ++i) {
			w.F32(a_msg.sliders[i]);
		}
		const auto faceMorphs = (std::min)(a_msg.morphs.size(), MAX_FACE_MORPHS);
		w.U16(static_cast<std::uint16_t>(faceMorphs));
		for (std::size_t i = 0; i < faceMorphs; ++i) {
			w.U32(a_msg.morphs[i].id);
			w.F32(a_msg.morphs[i].value);
		}
		const auto bones = (std::min)(a_msg.bones.size(), MAX_FACE_BONES);
		w.U16(static_cast<std::uint16_t>(bones));
		for (std::size_t i = 0; i < bones; ++i) {
			const auto& b = a_msg.bones[i];
			w.U32(b.region);
			for (const float v : b.position) {
				w.F32(v);
			}
			for (const float v : b.rotation) {
				w.F32(v);
			}
			for (const float v : b.scale) {
				w.F32(v);
			}
		}
		const auto tints = (std::min)(a_msg.tints.size(), MAX_FACE_TINTS);
		w.U8(static_cast<std::uint8_t>(tints));
		for (std::size_t i = 0; i < tints; ++i) {
			const auto& t = a_msg.tints[i];
			w.U16(t.templateId);
			w.U8(t.type);
			w.U8(t.value);
			w.U32(t.color);
			w.U16(t.swatch);
		}
		for (const auto v : a_msg.bodyTint) {
			w.U8(v);
		}
		// A name too long to travel whole is left out rather than cut to another morph's name.
		std::vector<const BodyMorph*> morphs;
		for (const auto& m : a_msg.bodyMorphs) {
			if (morphs.size() < MAX_BODY_MORPHS && !m.name.empty() && m.name.size() <= MAX_BODY_MORPH_NAME) {
				morphs.push_back(&m);
			}
		}
		w.U8(static_cast<std::uint8_t>(morphs.size()));
		for (const auto m : morphs) {
			w.Str(m->name, MAX_BODY_MORPH_NAME);
			w.F32(m->value);
		}
		return w.Data();
	}

	inline std::optional<Face> DecodeFace(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Face msg;
		msg.playerId = r.U32();
		msg.female = r.U8() != 0;
		msg.race = r.U32();
		const auto parts = r.U8();
		if (parts > MAX_FACE_HEAD_PARTS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < parts && r.Ok(); ++i) {
			msg.headParts.push_back(r.U32());
		}
		msg.hairColor = r.U32();
		msg.facialHairColor = r.U32();
		msg.faceDetails = r.U32();
		for (float& v : msg.weight) {
			v = r.F32();
		}
		const auto sliders = r.U16();
		if (sliders > MAX_FACE_SLIDERS) {
			return std::nullopt;
		}
		for (std::uint16_t i = 0; i < sliders && r.Ok(); ++i) {
			msg.sliders.push_back(r.F32());
		}
		const auto faceMorphs = r.U16();
		if (faceMorphs > MAX_FACE_MORPHS) {
			return std::nullopt;
		}
		for (std::uint16_t i = 0; i < faceMorphs && r.Ok(); ++i) {
			FaceMorph m;
			m.id = r.U32();
			m.value = r.F32();
			msg.morphs.push_back(m);
		}
		const auto bones = r.U16();
		if (bones > MAX_FACE_BONES) {
			return std::nullopt;
		}
		for (std::uint16_t i = 0; i < bones && r.Ok(); ++i) {
			FaceBone b;
			b.region = r.U32();
			for (float& v : b.position) {
				v = r.F32();
			}
			for (float& v : b.rotation) {
				v = r.F32();
			}
			for (float& v : b.scale) {
				v = r.F32();
			}
			msg.bones.push_back(b);
		}
		const auto tints = r.U8();
		if (tints > MAX_FACE_TINTS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < tints && r.Ok(); ++i) {
			FaceTint t;
			t.templateId = r.U16();
			t.type = r.U8();
			t.value = r.U8();
			t.color = r.U32();
			t.swatch = r.U16();
			if (t.type > 2) {
				return std::nullopt;
			}
			msg.tints.push_back(t);
		}
		for (auto& v : msg.bodyTint) {
			v = r.U8();
		}
		const auto morphs = r.U8();
		if (morphs > MAX_BODY_MORPHS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < morphs && r.Ok(); ++i) {
			BodyMorph m;
			m.name = r.Str(MAX_BODY_MORPH_NAME);
			m.value = r.F32();
			if (r.Ok() && (m.name.empty() || !std::isfinite(m.value))) {
				return std::nullopt;
			}
			m.value = std::clamp(m.value, -MAX_BODY_MORPH_VALUE, MAX_BODY_MORPH_VALUE);
			msg.bodyMorphs.push_back(std::move(m));
		}
		if (!r.Ok() || !r.AtEnd() || msg.race == 0) {
			return std::nullopt;
		}
		const auto finite = [](float a_v) { return std::isfinite(a_v); };
		if (!std::ranges::all_of(msg.weight, finite) || !std::ranges::all_of(msg.sliders, finite) ||
			!std::ranges::all_of(msg.morphs, [&](const FaceMorph& m) { return finite(m.value); }) ||
			!std::ranges::all_of(msg.bones, [&](const FaceBone& b) { return std::ranges::all_of(b.position, finite) && std::ranges::all_of(b.rotation, finite) && std::ranges::all_of(b.scale, finite); })) {
			return std::nullopt;
		}
		// Body weight is a mix of three, each 0..1 in the game's own menus; it is what every receiver applies
		// (Face::MakeBase), so a modified game's 7 or -3 is brought back into range rather than deforming bodies.
		for (float& v : msg.weight) {
			v = std::clamp(v, 0.0f, 1.0f);
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcEquipment& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.refId);
		const auto count = (std::min)(a_msg.items.size(), MAX_EQUIPMENT);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			w.U32(a_msg.items[i]);
		}
		return w.Data();
	}

	inline std::optional<NpcEquipment> DecodeNpcEquipment(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcEquipment msg;
		msg.playerId = r.U32();
		msg.refId = r.U32();
		const auto count = r.U8();
		if (count > MAX_EQUIPMENT) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			msg.items.push_back(r.U32());
		}
		if (!r.Ok() || !r.AtEnd() || !IsActorRef(msg.refId)) {
			return std::nullopt;
		}
		return msg;
	}

	// Generic list packets: a count byte followed by fixed-size entries.
	template <class T, class WriteFn>
	std::vector<std::uint8_t> EncodeList(MessageType a_type, const std::vector<T>& a_items, WriteFn a_write)
	{
		Writer     w{ a_type };
		const auto count = (std::min)(a_items.size(), MAX_ACTORS_PER_PACKET);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			a_write(w, a_items[i]);
		}
		return w.Data();
	}

	template <class T, class ReadFn>
	std::optional<std::vector<T>> DecodeList(std::span<const std::uint8_t> a_data, ReadFn a_read)
	{
		Reader r{ a_data };
		r.U8();
		const auto count = r.U8();
		if (count > MAX_ACTORS_PER_PACKET) {
			return std::nullopt;
		}
		std::vector<T> items;
		items.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			items.push_back(a_read(r));
		}
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return items;
	}

	inline std::vector<std::uint8_t> Encode(const std::vector<ActorClaim>& a_msg)
	{
		return EncodeList(MessageType::kClaimActors, a_msg, [](Writer& w, const ActorClaim& a) { w.U32(a.refId); w.U8(std::to_underlying(a.reason)); w.U8(a.essential ? 1 : 0); });
	}

	inline auto DecodeClaims(std::span<const std::uint8_t> a_data)
	{
		bool flagsKnown = true;  // the essential byte is 0 or 1
		auto claims = DecodeList<ActorClaim>(a_data, [&](Reader& r) {
			ActorClaim a;
			a.refId = r.U32();
			a.reason = static_cast<ClaimReason>(r.U8());
			const auto essential = r.U8();
			flagsKnown = flagsKnown && essential <= 1;
			a.essential = essential != 0;
			return a;
		});
		if (claims && (!flagsKnown || !std::ranges::all_of(*claims, [](const ActorClaim& a) { return a.reason <= ClaimReason::kCompanion; }))) {
			return decltype(claims){};
		}
		return claims;
	}

	// kReleaseActors
	inline std::vector<std::uint8_t> EncodeRelease(const std::vector<std::uint32_t>& a_msg)
	{
		return EncodeList(MessageType::kReleaseActors, a_msg, [](Writer& w, std::uint32_t a) { w.U32(a); });
	}

	inline auto DecodeRelease(std::span<const std::uint8_t> a_data)
	{
		return DecodeList<std::uint32_t>(a_data, [](Reader& r) { return r.U32(); });
	}

	inline std::vector<std::uint8_t> Encode(const std::vector<ActorOwner>& a_msg)
	{
		return EncodeList(MessageType::kActorOwners, a_msg, [](Writer& w, const ActorOwner& a) { w.U32(a.refId); w.U32(a.playerId); });
	}

	inline auto DecodeOwners(std::span<const std::uint8_t> a_data)
	{
		return DecodeList<ActorOwner>(a_data, [](Reader& r) { ActorOwner a; a.refId = r.U32(); a.playerId = r.U32(); return a; });
	}

	// kActorStates (client -> server) or kActorStatesRelay (server -> client).
	inline std::vector<std::uint8_t> Encode(const std::vector<ActorState>& a_msg, MessageType a_type)
	{
		return EncodeList(a_type, a_msg, [](Writer& w, const ActorState& a) {
			w.U32(a.refId);
			w.U32(a.cell);
			w.U32(a.worldspace);
			w.F32(a.x);
			w.F32(a.y);
			w.F32(a.z);
			w.F32(a.heading);
			w.F32(a.speed);
			w.U16(a.moveMode);
			w.U8(a.flags);
			w.U8(a.direction);
			w.U8(a.gait);
			w.U8(a.gaitRate);
			w.U8(static_cast<std::uint8_t>(a.aimPitch));
		});
	}

	inline std::optional<std::vector<ActorState>> DecodeActorStates(std::span<const std::uint8_t> a_data)
	{
		auto states = DecodeList<ActorState>(a_data, [](Reader& r) {
			ActorState a;
			a.refId = r.U32();
			a.cell = r.U32();
			a.worldspace = r.U32();
			a.x = r.F32();
			a.y = r.F32();
			a.z = r.F32();
			a.heading = r.F32();
			a.speed = r.F32();
			a.moveMode = r.U16();
			a.flags = r.U8();
			a.direction = r.U8();
			a.gait = r.U8();
			a.gaitRate = r.U8();
			a.aimPitch = static_cast<std::int8_t>(r.U8());
			return a;
		});
		if (states && !std::ranges::all_of(*states, [](const ActorState& a) { return a.IsFinite(); })) {
			return std::nullopt;
		}
		return states;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerHit& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.F32(a_msg.damage);
		w.U8(a_msg.byPlayer ? 1 : 0);
		w.U32(a_msg.attacker);
		return w.Data();
	}

	inline std::optional<PlayerHit> DecodePlayerHit(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerHit msg;
		msg.playerId = r.U32();
		msg.damage = r.F32();
		msg.byPlayer = r.U8() != 0;
		msg.attacker = r.U32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.damage) || msg.damage < 0.0f ||
			(msg.byPlayer ? msg.attacker != 0 : !IsActorRef(msg.attacker))) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Shot& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.refId);
		w.U8(a_msg.action);
		w.U32(a_msg.sound);
		return w.Data();
	}

	inline std::optional<Shot> DecodeShot(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Shot msg;
		msg.playerId = r.U32();
		msg.refId = r.U32();
		msg.action = r.U8();
		msg.sound = r.U32();
		if (!r.Ok() || !r.AtEnd() || (msg.refId != 0 && !IsActorRef(msg.refId)) || msg.action > ShotAction::kMax) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerStatus& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.location);
		w.U32(a_msg.cell);
		w.U8(a_msg.health);
		w.U16(a_msg.level);
		w.U8(static_cast<std::uint8_t>((a_msg.downed ? 1 : 0) | (a_msg.opening ? 2 : 0) | (a_msg.light ? 4 : 0) | (a_msg.powerArmor ? 8 : 0) | (a_msg.parked ? 16 : 0) |
		                                   (a_msg.seat ? 32 : 0)));
		w.U32(a_msg.powerArmorFrame);
		w.U32(a_msg.powerArmorFrameRef);
		if (a_msg.parked) {
			const auto& parked = *a_msg.parked;
			w.U32(parked.base);
			w.U32(parked.ref);
			w.U32(parked.cell);
			w.U32(parked.worldspace);
			w.F32(parked.x);
			w.F32(parked.y);
			w.F32(parked.z);
			w.F32(parked.heading);
		}
		if (a_msg.seat) {
			const auto& seat = *a_msg.seat;
			w.U32(seat.ref);
			w.U32(seat.base);
			w.F32(seat.x);
			w.F32(seat.y);
			w.F32(seat.z);
		}
		return w.Data();
	}

	inline std::optional<PlayerStatus> DecodePlayerStatus(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerStatus msg;
		msg.playerId = r.U32();
		msg.location = r.U32();
		msg.cell = r.U32();
		msg.health = r.U8();
		msg.level = r.U16();
		const auto flags = r.U8();
		msg.downed = (flags & 1) != 0;
		msg.opening = (flags & 2) != 0;
		msg.light = (flags & 4) != 0;
		msg.powerArmor = (flags & 8) != 0;
		msg.powerArmorFrame = r.U32();
		msg.powerArmorFrameRef = r.U32();
		if ((flags & 16) != 0) {
			ParkedFrame parked;
			parked.base = r.U32();
			parked.ref = r.U32();
			parked.cell = r.U32();
			parked.worldspace = r.U32();
			parked.x = r.F32();
			parked.y = r.F32();
			parked.z = r.F32();
			parked.heading = r.F32();
			msg.parked = parked;
		}
		if ((flags & 32) != 0) {
			Seat seat;
			seat.ref = r.U32();
			seat.base = r.U32();
			seat.x = r.F32();
			seat.y = r.F32();
			seat.z = r.F32();
			msg.seat = seat;
		}
		// A frame is parked, and a seat is told, only while they are out of power armor (a frame is furniture too, but
		// a frame's own path handles that).
		if (!r.Ok() || !r.AtEnd() || msg.health > 100 || (msg.powerArmorFrameRef != 0 && !IsShareableRef(msg.powerArmorFrameRef)) ||
			(msg.parked && (msg.powerArmor || !msg.parked->Valid())) || (msg.seat && (msg.powerArmor || !msg.seat->Valid()))) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const WorldTime& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.F32(a_msg.gameHour);
		w.F32(a_msg.daysPassed);
		w.U32(a_msg.worldspace);
		w.U32(a_msg.weather);
		return w.Data();
	}

	inline std::optional<WorldTime> DecodeWorldTime(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		WorldTime msg;
		msg.playerId = r.U32();
		msg.gameHour = r.F32();
		msg.daysPassed = r.F32();
		msg.worldspace = r.U32();
		msg.weather = r.U32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.gameHour) || msg.gameHour < 0.0f || msg.gameHour >= 24.0f ||
			!std::isfinite(msg.daysPassed) || msg.daysPassed < 0.0f || msg.daysPassed > 1.0e6f) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Ping& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.cell);
		w.U32(a_msg.worldspace);
		w.F32(a_msg.x);
		w.F32(a_msg.y);
		w.F32(a_msg.z);
		return w.Data();
	}

	inline std::optional<Ping> DecodePing(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Ping msg;
		msg.playerId = r.U32();
		msg.cell = r.U32();
		msg.worldspace = r.U32();
		msg.x = r.F32();
		msg.y = r.F32();
		msg.z = r.F32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.x) || !std::isfinite(msg.y) || !std::isfinite(msg.z)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Line& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.speaker);
		w.Str(a_msg.text, MAX_LINE_LENGTH);
		w.U32(a_msg.info);
		w.U8(a_msg.response);
		w.U32(a_msg.voiceType);
		return w.Data();
	}

	inline std::optional<Line> DecodeLine(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Line msg;
		msg.playerId = r.U32();
		msg.speaker = r.U32();
		msg.text = r.Str(MAX_LINE_LENGTH);
		msg.info = r.U32();
		msg.response = r.U8();
		msg.voiceType = r.U32();
		if (!r.Ok() || !r.AtEnd() || msg.text.empty() || (msg.speaker != 0 && !IsShareableRef(msg.speaker))) {
			return std::nullopt;
		}
		// A voiced line names its info and its response together, and a voice type only with them.
		if ((msg.info == 0) != (msg.response == 0) || (msg.info != 0 && !IsShareableRef(msg.info)) ||
			(msg.voiceType != 0 && (msg.info == 0 || !IsShareableRef(msg.voiceType)))) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Heartbeat& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.token);
		return w.Data();
	}

	inline std::optional<Heartbeat> DecodeHeartbeat(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Heartbeat msg;
		msg.token = r.U32();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const XpGain& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.F32(a_msg.xp);
		return w.Data();
	}

	inline std::optional<XpGain> DecodeXpGain(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		XpGain msg;
		msg.playerId = r.U32();
		msg.xp = r.F32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.xp) || msg.xp <= 0.0f || msg.xp > MAX_XP_SHARE) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const MarkersFound& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		const auto count = (std::min)(a_msg.markers.size(), MAX_MARKERS_PER_PACKET);
		w.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			w.U32(a_msg.markers[i].refId);
			w.U8(a_msg.markers[i].flags);
		}
		return w.Data();
	}

	inline std::optional<MarkersFound> DecodeMarkers(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		MarkersFound msg;
		msg.playerId = r.U32();
		const auto count = r.U16();
		if (count > MAX_MARKERS_PER_PACKET) {
			return std::nullopt;
		}
		for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
			MapMarker marker;
			marker.refId = r.U32();
			marker.flags = r.U8() & 0x03;
			if (IsShareableRef(marker.refId) && marker.flags != 0) {
				msg.markers.push_back(marker);
			}
		}
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const VoiceData& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.Bytes(a_msg.data);
		return w.Data();
	}

	inline std::optional<VoiceData> DecodeVoice(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		VoiceData msg;
		msg.playerId = r.U32();
		msg.data = r.Bytes(MAX_VOICE_BYTES);
		if (!r.Ok() || !r.AtEnd() || msg.data.empty()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Revive& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		return w.Data();
	}

	inline std::optional<Revive> DecodeRevive(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Revive msg;
		msg.playerId = r.U32();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const QuestDone& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.quest);
		return w.Data();
	}

	inline std::optional<QuestDone> DecodeQuestDone(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		QuestDone msg;
		msg.playerId = r.U32();
		msg.quest = r.U32();
		if (!r.Ok() || !r.AtEnd() || !IsShareableRef(msg.quest)) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const QuestStage& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.quest);
		w.U16(a_msg.stage);
		w.U32(a_msg.playerId);
		return w.Data();
	}

	inline std::optional<QuestStage> DecodeQuestStage(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		QuestStage msg;
		msg.quest = r.U32();
		msg.stage = r.U16();
		msg.playerId = r.U32();
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const WorldState& a_msg)
	{
		Writer w{ MessageType::kWorldState };
		const auto writeIds = [&](const std::vector<std::uint32_t>& a_ids) {
			const auto count = (std::min)(a_ids.size(), MAX_WORLD_STATE_ACTORS);
			w.U32(static_cast<std::uint32_t>(count));
			for (std::size_t i = 0; i < count; ++i) {
				w.U32(a_ids[i]);
			}
		};
		writeIds(a_msg.deadActors);
		w.U32(a_msg.containerFirstIndex);
		const auto changes = (std::min)(a_msg.containerChanges.size(), MAX_WORLD_STATE_ACTORS);
		w.U32(static_cast<std::uint32_t>(changes));
		for (std::size_t i = 0; i < changes; ++i) {
			const auto& change = a_msg.containerChanges[i];
			w.U32(change.change.container);
			w.U32(change.change.item);
			w.U32(static_cast<std::uint32_t>(change.change.count));
			w.U32(change.playerId);
		}
		writeIds(a_msg.pickedUp);
		const auto states = (std::min)(a_msg.refStates.size(), MAX_WORLD_STATE_ACTORS);
		w.U32(static_cast<std::uint32_t>(states));
		for (std::size_t i = 0; i < states; ++i) {
			w.U32(a_msg.refStates[i].refId);
			w.U8(a_msg.refStates[i].open);
			w.U8(a_msg.refStates[i].locked);
		}
		const auto quests = (std::min)(a_msg.questStages.size(), MAX_WORLD_STATE_ACTORS);
		w.U32(static_cast<std::uint32_t>(quests));
		for (std::size_t i = 0; i < quests; ++i) {
			w.U32(a_msg.questStages[i].quest);
			w.U16(a_msg.questStages[i].stage);
		}
		return w.Data();
	}

	inline std::optional<WorldState> DecodeWorldState(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		WorldState msg;
		const auto readIds = [&](std::vector<std::uint32_t>& a_out) {
			const auto count = r.U32();
			if (count > MAX_WORLD_STATE_ACTORS) {
				return false;
			}
			a_out.reserve(count);
			for (std::uint32_t i = 0; i < count && r.Ok(); ++i) {
				a_out.push_back(r.U32());
			}
			return r.Ok();
		};
		if (!readIds(msg.deadActors)) {
			return std::nullopt;
		}
		msg.containerFirstIndex = r.U32();
		const auto changes = r.U32();
		if (changes > MAX_WORLD_STATE_ACTORS) {
			return std::nullopt;
		}
		for (std::uint32_t i = 0; i < changes && r.Ok(); ++i) {
			IndexedContainerChange change;
			change.index = msg.containerFirstIndex + i;
			change.change.container = r.U32();
			change.change.item = r.U32();
			change.change.count = static_cast<std::int32_t>(r.U32());
			change.playerId = r.U32();
			msg.containerChanges.push_back(change);
		}
		if (!r.Ok() || !readIds(msg.pickedUp)) {
			return std::nullopt;
		}
		const auto states = r.U32();
		if (states > MAX_WORLD_STATE_ACTORS) {
			return std::nullopt;
		}
		for (std::uint32_t i = 0; i < states && r.Ok(); ++i) {
			RefState state;
			state.refId = r.U32();
			state.open = r.U8();
			state.locked = r.U8();
			if (state.open > RefState::kUnknown || state.locked > RefState::kUnknown) {
				return std::nullopt;
			}
			msg.refStates.push_back(state);
		}
		const auto quests = r.U32();
		if (!r.Ok() || quests > MAX_WORLD_STATE_ACTORS) {
			return std::nullopt;
		}
		for (std::uint32_t i = 0; i < quests && r.Ok(); ++i) {
			QuestStage stage;
			stage.quest = r.U32();
			stage.stage = r.U16();
			msg.questStages.push_back(stage);
		}
		if (!r.Ok() || !r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}
}
