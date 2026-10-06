#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Wire protocol shared by the game plugin, the server and test tools.
// All values are little-endian. Every packet starts with a one-byte MessageType.
namespace Protocol
{
	inline constexpr std::uint32_t MAGIC = 0x504D3446;  // "F4MP"
	inline constexpr std::uint16_t VERSION = 16;
	inline constexpr std::uint16_t DEFAULT_PORT = 7779;
	inline constexpr std::size_t   MAX_NAME_LENGTH = 32;
	inline constexpr std::size_t   MAX_REASON_LENGTH = 200;
	inline constexpr std::size_t   MAX_PASSWORD_LENGTH = 64;

	// The local player's reference. Never shared: each game has its own.
	inline constexpr std::uint32_t PLAYER_REF_ID = 0x14;

	// References are shared by form ID, which matches for everyone because the load order must.
	// Runtime-created references (0xFF......) differ per game, and the player is per game.
	[[nodiscard]] constexpr bool IsShareableRef(std::uint32_t a_id)
	{
		return a_id != 0 && a_id != PLAYER_REF_ID && (a_id >> 24) != 0xFF;
	}

	// Largest item count a single container change may move.
	inline constexpr std::int32_t MAX_ITEM_COUNT = 1'000'000;

	// ENet channels
	inline constexpr std::uint8_t CHANNEL_RELIABLE = 0;
	inline constexpr std::uint8_t CHANNEL_STATE = 1;  // unreliable, sequenced
	inline constexpr std::uint8_t CHANNEL_COUNT = 2;

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
		kHeartbeatAck = 122,
		kQuestDone = 123,         // another player completed a quest
		kRevived = 124,           // another player helped you up
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

		[[nodiscard]] bool IsFinite() const
		{
			return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
			       std::isfinite(heading) && std::isfinite(speed);
		}
	};

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
	};

	struct Welcome
	{
		std::uint32_t playerId = 0;
		std::uint64_t sessionId = 0;  // random per server run
		bool          friendlyFire = false;  // players can hurt each other
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

	// Actors are identified by reference form ID, which is the same for everyone because the
	// load order must match. Only references from plugin files (not runtime-created ones,
	// whose IDs start with 0xFF and differ per game) are synced.
	struct ActorDeath
	{
		std::uint32_t refId = 0;
		std::uint32_t playerId = 0;  // who reported it (set by the server)
		bool          killed = false;  // that player killed it
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

		bool operator==(const RefState&) const = default;
	};

	inline constexpr std::size_t MAX_EQUIPMENT = 32;

	// Base forms of a player's equipped armor and weapons. playerId is ignored client -> server.
	struct Equipment
	{
		std::uint32_t              playerId = 0;
		std::vector<std::uint32_t> items;
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
	};

	struct ActorOwner
	{
		std::uint32_t refId = 0;
		std::uint32_t playerId = 0;  // 0 = nobody
	};

	struct ActorState
	{
		std::uint32_t refId = 0;
		std::uint32_t cell = 0;        // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;  // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;
		float         speed = 0;
		std::uint16_t moveMode = 0;
		std::uint8_t  flags = kNone;  // StateFlags

		[[nodiscard]] bool IsFinite() const
		{
			return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(heading) && std::isfinite(speed);
		}
	};

	// kPlayerHit: playerId is the victim; byPlayer = the sender hit them (friendly fire), not an
	// NPC in the sender's world. kPlayerDamaged: playerId is the sender of the kPlayerHit.
	struct PlayerHit
	{
		std::uint32_t playerId = 0;
		float         damage = 0.0f;
		bool          byPlayer = false;
	};

	// A weapon shot, for the firing animation. refId 0 = the player themselves, otherwise an NPC
	// they run. playerId is ignored client -> server.
	struct Shot
	{
		std::uint32_t playerId = 0;
		std::uint32_t refId = 0;
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

		bool operator==(const PlayerStatus&) const = default;
	};

	struct WorldTime
	{
		float         gameHour = 0.0f;  // 0..24
		std::uint32_t worldspace = 0;   // where the weather applies (0 = inside)
		std::uint32_t weather = 0;      // TESWeather form ID, or 0
	};

	struct Ping
	{
		std::uint32_t playerId = 0;
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0, y = 0, z = 0;
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

	struct QuestStage
	{
		std::uint32_t quest = 0;
		std::uint16_t stage = 0;

		bool operator==(const QuestStage&) const = default;
	};

	// Everything that happened this session, sent on join and after loading a save. Large states
	// are split over several packets; each is applied on top of the previous ones.
	struct WorldState
	{
		std::vector<std::uint32_t>    deadActors;
		std::uint32_t                 containerFirstIndex = 0;  // session index of containerChanges[0]
		std::vector<ContainerChange>  containerChanges;  // in order
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
		return state;
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
		return w.Data();
	}

	inline std::optional<Hello> DecodeHello(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Hello msg;
		msg.magic = r.U32();
		msg.version = r.U16();
		msg.contentHash = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		msg.password = r.Str(MAX_PASSWORD_LENGTH);
		msg.appearance = r.U32();
		msg.world.sessionId = r.U64();
		msg.world.containerFrom = r.U32();
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
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
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
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
		if (!r.Ok() || !r.AtEnd()) {
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
		if (!r.Ok() || !r.AtEnd()) {
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
		return EncodeList(MessageType::kClaimActors, a_msg, [](Writer& w, const ActorClaim& a) { w.U32(a.refId); w.U8(std::to_underlying(a.reason)); });
	}

	inline auto DecodeClaims(std::span<const std::uint8_t> a_data)
	{
		auto claims = DecodeList<ActorClaim>(a_data, [](Reader& r) { ActorClaim a; a.refId = r.U32(); a.reason = static_cast<ClaimReason>(r.U8()); return a; });
		if (claims && !std::ranges::all_of(*claims, [](const ActorClaim& a) { return a.reason <= ClaimReason::kCompanion; })) {
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
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.damage) || msg.damage < 0.0f) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Shot& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.playerId);
		w.U32(a_msg.refId);
		return w.Data();
	}

	inline std::optional<Shot> DecodeShot(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Shot msg;
		msg.playerId = r.U32();
		msg.refId = r.U32();
		if (!r.Ok() || !r.AtEnd() || (msg.refId != 0 && !IsShareableRef(msg.refId))) {
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
		w.U8(a_msg.downed ? 1 : 0);
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
		msg.downed = r.U8() != 0;
		if (!r.Ok() || !r.AtEnd() || msg.health > 100) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const WorldTime& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.F32(a_msg.gameHour);
		w.U32(a_msg.worldspace);
		w.U32(a_msg.weather);
		return w.Data();
	}

	inline std::optional<WorldTime> DecodeWorldTime(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		WorldTime msg;
		msg.gameHour = r.F32();
		msg.worldspace = r.U32();
		msg.weather = r.U32();
		if (!r.Ok() || !r.AtEnd() || !std::isfinite(msg.gameHour) || msg.gameHour < 0.0f || msg.gameHour >= 24.0f) {
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
		return w.Data();
	}

	inline std::optional<QuestStage> DecodeQuestStage(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		QuestStage msg;
		msg.quest = r.U32();
		msg.stage = r.U16();
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
			w.U32(change.container);
			w.U32(change.item);
			w.U32(static_cast<std::uint32_t>(change.count));
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
			ContainerChange change;
			change.container = r.U32();
			change.item = r.U32();
			change.count = static_cast<std::int32_t>(r.U32());
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
