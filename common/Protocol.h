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
	inline constexpr std::uint16_t VERSION = 2;
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

		// server -> client
		kWelcome = 101,
		kReject = 102,
		kPlayerJoined = 103,
		kPlayerLeft = 104,
		kPlayerStates = 105,
	};

	enum StateFlags : std::uint8_t
	{
		kNone = 0,
		kRunning = 1 << 0,
		kSneaking = 1 << 1,
		kWeaponDrawn = 1 << 2,
		kInAir = 1 << 3,
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
	};

	struct PlayerLeft
	{
		std::uint32_t playerId = 0;
	};

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
		return w.Data();
	}

	inline std::optional<PlayerJoined> DecodePlayerJoined(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerJoined msg;
		msg.playerId = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
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
}
