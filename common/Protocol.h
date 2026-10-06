#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Wire protocol shared by the game plugin, the server and test tools.
// All values are little-endian. Every packet starts with a one-byte MessageType.
namespace Protocol
{
	inline constexpr std::uint32_t MAGIC = 0x504D3446;  // "F4MP"
	inline constexpr std::uint16_t VERSION = 8;
	inline constexpr std::uint16_t DEFAULT_PORT = 7779;
	inline constexpr std::size_t   MAX_NAME_LENGTH = 32;
	inline constexpr std::size_t   MAX_REASON_LENGTH = 200;
	inline constexpr std::size_t   MAX_PASSWORD_LENGTH = 64;

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

		// server -> client
		kWelcome = 101,
		kReject = 102,
		kPlayerJoined = 103,
		kPlayerLeft = 104,
		kPlayerStates = 105,
		kActorDied = 106,     // an actor died in another player's world
		kWorldState = 107,    // sent on join: everything that already happened this session
		kActorHealth = 108,   // an actor's health changed in another player's world
		kContainerChanged = 109,  // another player changed a container's contents
		kRefPickedUp = 110,       // another player picked up this world item
		kRefStateChanged = 111,   // a door/container's open or lock state changed in another player's world
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

	struct Hello
	{
		std::uint32_t magic = MAGIC;
		std::uint16_t version = VERSION;
		std::uint32_t contentHash = 0;  // hash of the client's load order; all players must match
		std::string   name;
		std::string   password;
		std::uint32_t appearance = 0;  // NPC base form others should use for this player (0 = their default)
	};

	struct Welcome
	{
		std::uint32_t playerId = 0;
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

	struct WorldState
	{
		std::vector<std::uint32_t>    deadActors;
		std::vector<ContainerChange>  containerChanges;  // in order
		std::vector<std::uint32_t>    pickedUp;
		std::vector<RefState>         refStates;  // latest per reference
	};

	inline constexpr std::size_t MAX_WORLD_STATE_ACTORS = 60000;

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
		if (!r.Ok()) {
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
		return w.Data();
	}

	inline std::optional<Welcome> DecodeWelcome(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Welcome msg;
		msg.playerId = r.U32();
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
			msg.players.push_back(entry);
		}
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}
	inline std::vector<std::uint8_t> Encode(const ActorDeath& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		return w.Data();
	}

	inline std::optional<ActorDeath> DecodeActorDeath(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		ActorDeath msg;
		msg.refId = r.U32();
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
			msg.refStates.push_back(state);
		}
		if (!r.Ok()) {
			return std::nullopt;
		}
		return msg;
	}
}
