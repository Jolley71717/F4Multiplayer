#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

// How long a friend's line stays up in the game's subtitle area (src/game/Conversations.cpp): as long as its
// recording plays, read from the voice file itself, with a floor so a short line can still be read, and for a
// line with no recording, the time it takes to read it. No game types: tests/SubtitleTimeTests.cpp covers it.
namespace SubtitleTime
{
	using namespace std::chrono_literals;

	inline constexpr std::chrono::milliseconds MIN_SHOW = 2500ms;   // no line goes sooner
	inline constexpr std::chrono::milliseconds TAIL = 500ms;        // after the recording ends, as the eye catches up
	inline constexpr std::chrono::milliseconds MAX_TEXT = 12000ms;  // a line with no recording, however long
	inline constexpr std::chrono::milliseconds MAX_SHOW = 30000ms;  // a recording that claims more is not believed
	inline constexpr std::chrono::milliseconds READ_BASE = 1500ms;  // reading a line: this, ...
	inline constexpr std::chrono::milliseconds READ_PER_CHAR = 50ms;  // ... and this a character (20 a second)

	namespace detail
	{
		inline std::uint32_t U32(std::span<const std::uint8_t> a_data, std::size_t a_at)
		{
			return static_cast<std::uint32_t>(a_data[a_at]) | (static_cast<std::uint32_t>(a_data[a_at + 1]) << 8) |
			       (static_cast<std::uint32_t>(a_data[a_at + 2]) << 16) | (static_cast<std::uint32_t>(a_data[a_at + 3]) << 24);
		}

		inline std::uint16_t U16(std::span<const std::uint8_t> a_data, std::size_t a_at)
		{
			return static_cast<std::uint16_t>(a_data[a_at] | (a_data[a_at + 1] << 8));
		}

		inline bool Tag(std::span<const std::uint8_t> a_data, std::size_t a_at, const char (&a_tag)[5])
		{
			return a_at + 4 <= a_data.size() && std::memcmp(a_data.data() + a_at, a_tag, 4) == 0;
		}

		// A RIFF file's length: WAVE (PCM) by its data size and byte rate; XWMA by its packets' decoded size
		// (the last entry of 'dpds') and the PCM rate it decodes to, or else by its data size and byte rate.
		inline std::optional<double> Riff(std::span<const std::uint8_t> a_data)
		{
			if (a_data.size() < 12 || !Tag(a_data, 0, "RIFF")) {
				return std::nullopt;
			}
			const bool xwma = Tag(a_data, 8, "XWMA");
			if (!xwma && !Tag(a_data, 8, "WAVE")) {
				return std::nullopt;
			}
			std::uint16_t format = 0, channels = 0, bits = 0;
			std::uint32_t rate = 0, byteRate = 0, dataSize = 0, decoded = 0;
			bool          haveFormat = false, haveData = false;
			std::size_t   at = 12;
			while (at + 8 <= a_data.size()) {
				const auto size = U32(a_data, at + 4);
				const auto body = at + 8;
				const auto inFile = std::min<std::size_t>(size, a_data.size() - body);
				if (Tag(a_data, at, "fmt ") && inFile >= 16) {
					format = U16(a_data, body);
					channels = U16(a_data, body + 2);
					rate = U32(a_data, body + 4);
					byteRate = U32(a_data, body + 8);
					bits = U16(a_data, body + 14);
					haveFormat = true;
				} else if (Tag(a_data, at, "dpds") && inFile >= 4 && inFile == size) {
					decoded = U32(a_data, body + (inFile / 4 - 1) * 4);  // cumulative: the last is the whole
				} else if (Tag(a_data, at, "data")) {
					dataSize = size;  // its declared size: the data itself need not have been read
					haveData = true;
				}
				const auto next = body + static_cast<std::size_t>(size) + (size & 1);  // chunks are word aligned
				if (next <= at) {
					break;
				}
				at = next;
			}
			if (!haveFormat) {
				return std::nullopt;
			}
			if (xwma && decoded > 0 && rate > 0 && channels > 0) {
				const auto bytesPerSample = (bits == 0 ? 16u : bits) / 8u;
				if (bytesPerSample > 0) {
					return static_cast<double>(decoded) / (static_cast<double>(rate) * channels * bytesPerSample);
				}
			}
			if (haveData && byteRate > 0 && (xwma || format == 1)) {
				return static_cast<double>(dataSize) / byteRate;
			}
			return std::nullopt;
		}
	}

	// The length in seconds of a voice file's recording, from its bytes: a .fuz (the lip sync, then the
	// recording as XWMA), a bare .xwm or a .wav. The file may be cut short after the chunks before its
	// audio. Nothing for anything else, or a length that can't be one (none, or over 10 minutes).
	inline std::optional<double> VoiceSeconds(std::span<const std::uint8_t> a_data)
	{
		auto audio = a_data;
		if (a_data.size() >= 12 && detail::Tag(a_data, 0, "FUZE")) {
			const auto lip = detail::U32(a_data, 8);  // after the magic and the version
			if (lip > a_data.size() - 12) {
				return std::nullopt;
			}
			audio = a_data.subspan(12 + lip);
		}
		const auto seconds = detail::Riff(audio);
		if (!seconds || !std::isfinite(*seconds) || *seconds <= 0.0 || *seconds > 600.0) {
			return std::nullopt;
		}
		return seconds;
	}

	// How much of a voice file VoiceSeconds needs past its lip sync: the RIFF chunks before the audio (fmt, the
	// packet table 'dpds', the 'data' chunk's header). A packet table is 4 bytes a packet, a few KB for a song.
	inline constexpr std::size_t HEADER_AUDIO = 64u << 10;
	inline constexpr std::size_t HEADER_PROBE = 12;  // a .fuz's magic, version and lip sync size

	// How many bytes from its start a voice file is read for its length, from its first HEADER_PROBE bytes: a .fuz's
	// lip sync and HEADER_AUDIO, or HEADER_AUDIO for a bare .xwm or .wav.
	inline std::size_t HeaderBytes(std::span<const std::uint8_t> a_start)
	{
		if (a_start.size() >= HEADER_PROBE && detail::Tag(a_start, 0, "FUZE")) {
			return HEADER_PROBE + static_cast<std::size_t>(detail::U32(a_start, 8)) + HEADER_AUDIO;
		}
		return HEADER_AUDIO;
	}

	// The characters of a UTF-8 text (continuation bytes not counted).
	inline std::size_t Characters(std::string_view a_text)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_text, [](char a_c) { return (static_cast<unsigned char>(a_c) & 0xC0) != 0x80; }));
	}

	// A line with nothing to read: empty, or only spaces (some idle lines, Codsworth's 000475BB among them, are a single
	// space). The game shows no subtitle for one.
	inline bool Blank(std::string_view a_text)
	{
		return std::ranges::all_of(a_text, [](char a_c) { return a_c == ' ' || a_c == '\t' || a_c == '\r' || a_c == '\n'; });
	}

	// How long to read a line.
	inline std::chrono::milliseconds Reading(std::string_view a_text)
	{
		return READ_BASE + READ_PER_CHAR * static_cast<std::int64_t>(std::min<std::size_t>(Characters(a_text), 10000));
	}

	// How long a line stays up: while its recording plays and a little after (never under MIN_SHOW), or,
	// with no recording, as long as it takes to read (MIN_SHOW to MAX_TEXT).
	inline std::chrono::milliseconds ShowFor(std::optional<double> a_voiceSeconds, std::string_view a_text)
	{
		if (a_voiceSeconds && std::isfinite(*a_voiceSeconds) && *a_voiceSeconds > 0.0) {
			const auto voice = std::chrono::milliseconds(static_cast<std::int64_t>(std::min(*a_voiceSeconds, 600.0) * 1000.0 + 0.5));
			return std::clamp(voice + TAIL, MIN_SHOW, MAX_SHOW);
		}
		return std::clamp(Reading(a_text), MIN_SHOW, MAX_TEXT);
	}
}
