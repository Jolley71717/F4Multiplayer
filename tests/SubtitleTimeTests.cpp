#include "Test.h"
#include "game/SubtitleTime.h"

#include <string>
#include <vector>

namespace
{
	using Bytes = std::vector<std::uint8_t>;

	void Put32(Bytes& a_out, std::uint32_t a_value)
	{
		for (int i = 0; i < 4; ++i) {
			a_out.push_back(static_cast<std::uint8_t>(a_value >> (8 * i)));
		}
	}

	void Put16(Bytes& a_out, std::uint16_t a_value)
	{
		a_out.push_back(static_cast<std::uint8_t>(a_value));
		a_out.push_back(static_cast<std::uint8_t>(a_value >> 8));
	}

	void PutTag(Bytes& a_out, const char* a_tag)
	{
		a_out.insert(a_out.end(), a_tag, a_tag + 4);
	}

	void Chunk(Bytes& a_out, const char* a_tag, const Bytes& a_body)
	{
		PutTag(a_out, a_tag);
		Put32(a_out, static_cast<std::uint32_t>(a_body.size()));
		a_out.insert(a_out.end(), a_body.begin(), a_body.end());
		if (a_body.size() & 1) {
			a_out.push_back(0);
		}
	}

	Bytes Format(std::uint16_t a_tag, std::uint16_t a_channels, std::uint32_t a_rate, std::uint32_t a_byteRate, std::uint16_t a_bits)
	{
		Bytes fmt;
		Put16(fmt, a_tag);
		Put16(fmt, a_channels);
		Put32(fmt, a_rate);
		Put32(fmt, a_byteRate);
		Put16(fmt, static_cast<std::uint16_t>(a_channels * a_bits / 8));
		Put16(fmt, a_bits);
		Put16(fmt, 0);
		return fmt;
	}

	Bytes Riff(const char* a_kind, const Bytes& a_chunks)
	{
		Bytes out;
		PutTag(out, "RIFF");
		Put32(out, static_cast<std::uint32_t>(4 + a_chunks.size()));
		PutTag(out, a_kind);
		out.insert(out.end(), a_chunks.begin(), a_chunks.end());
		return out;
	}

	// An XWMA recording: 44.1 kHz mono, 16-bit once decoded; its packets decode to a_decoded bytes in all.
	Bytes Xwma(std::uint32_t a_decoded, std::uint32_t a_dataSize = 4000, std::uint32_t a_byteRate = 6000)
	{
		Bytes chunks;
		Chunk(chunks, "fmt ", Format(0x0161, 1, 44100, a_byteRate, 16));
		Bytes dpds;
		Put32(dpds, a_decoded / 3);
		Put32(dpds, a_decoded / 2);
		Put32(dpds, a_decoded);
		Chunk(chunks, "dpds", dpds);
		Chunk(chunks, "data", Bytes(a_dataSize, 0xAB));
		return Riff("XWMA", chunks);
	}

	Bytes Fuz(const Bytes& a_lip, const Bytes& a_audio)
	{
		Bytes out;
		PutTag(out, "FUZE");
		Put32(out, 1);
		Put32(out, static_cast<std::uint32_t>(a_lip.size()));
		out.insert(out.end(), a_lip.begin(), a_lip.end());
		out.insert(out.end(), a_audio.begin(), a_audio.end());
		return out;
	}

	bool Near(std::optional<double> a_value, double a_expected)
	{
		return a_value && *a_value > a_expected - 0.001 && *a_value < a_expected + 0.001;
	}
}

TEST("SubtitleTime: a .fuz recording lasts as long as its packets decode to")
{
	const auto audio = Xwma(44100 * 2 * 3);  // 3 s of 16-bit mono at 44.1 kHz
	CHECK(Near(SubtitleTime::VoiceSeconds(audio), 3.0));
	CHECK(Near(SubtitleTime::VoiceSeconds(Fuz(Bytes(37, 0x11), audio)), 3.0));  // after its lip sync (an odd size)
	CHECK(Near(SubtitleTime::VoiceSeconds(Fuz({}, audio)), 3.0));             // or with none
}

TEST("SubtitleTime: a recording with no packet table goes by its data size and byte rate")
{
	Bytes chunks;
	Chunk(chunks, "fmt ", Format(0x0161, 1, 44100, 6000, 16));
	Chunk(chunks, "data", Bytes(9000, 0));
	CHECK(Near(SubtitleTime::VoiceSeconds(Riff("XWMA", chunks)), 1.5));

	Bytes pcm;  // a loose .wav: 22.05 kHz mono 16-bit, 1 s
	Chunk(pcm, "fmt ", Format(1, 1, 22050, 44100, 16));
	Chunk(pcm, "data", Bytes(44100, 0));
	CHECK(Near(SubtitleTime::VoiceSeconds(Riff("WAVE", pcm)), 1.0));
}

TEST("SubtitleTime: a file read only up to its audio still gives the length")
{
	auto audio = Xwma(44100 * 2 * 5, 20000);
	audio.resize(audio.size() - 19000);  // the data chunk cut short: its declared size still counts
	CHECK(Near(SubtitleTime::VoiceSeconds(audio), 5.0));
}

TEST("SubtitleTime: no length from what isn't a recording")
{
	CHECK(!SubtitleTime::VoiceSeconds({}));
	const Bytes junk(64, 0x5A);
	CHECK(!SubtitleTime::VoiceSeconds(junk));
	CHECK(!SubtitleTime::VoiceSeconds(Fuz(Bytes(8, 0), Bytes(8, 0))));
	auto lying = Fuz(Bytes(8, 0), Xwma(44100 * 2));
	lying[8] = 0xFF;  // a lip size past the end of the file
	lying[9] = 0xFF;
	CHECK(!SubtitleTime::VoiceSeconds(lying));
	CHECK(!SubtitleTime::VoiceSeconds(Xwma(0, 0)));                // nothing in it
	CHECK(!SubtitleTime::VoiceSeconds(Xwma(0xFFFFFFF0u, 4000, 6000)));  // over 10 minutes: not believed
	Bytes compressed;  // a .wav that isn't PCM and has no packet table
	Chunk(compressed, "fmt ", Format(2, 1, 22050, 11000, 4));
	Chunk(compressed, "data", Bytes(11000, 0));
	CHECK(!SubtitleTime::VoiceSeconds(Riff("WAVE", compressed)));
	Bytes noFormat;
	Chunk(noFormat, "data", Bytes(100, 0));
	CHECK(!SubtitleTime::VoiceSeconds(Riff("XWMA", noFormat)));
}

TEST("SubtitleTime: a voiced line stays up while it plays and a little after, never too briefly")
{
	using namespace std::chrono_literals;
	CHECK(SubtitleTime::ShowFor(4.0, "Hi") == 4500ms);
	CHECK(SubtitleTime::ShowFor(0.4, "Hey.") == SubtitleTime::MIN_SHOW);
	CHECK(SubtitleTime::ShowFor(12.25, "A long speech") == 12750ms);
	CHECK(SubtitleTime::ShowFor(500.0, "x") == SubtitleTime::MAX_SHOW);
	CHECK(SubtitleTime::ShowFor(2.2, std::string(300, 'a')) == 2700ms);  // the recording's length, not the text's
}

TEST("SubtitleTime: a line with no recording stays up as long as it takes to read")
{
	using namespace std::chrono_literals;
	CHECK(SubtitleTime::ShowFor(std::nullopt, "Ok") == SubtitleTime::MIN_SHOW);
	CHECK(SubtitleTime::ShowFor(std::nullopt, std::string(60, 'a')) == 4500ms);  // 1.5 s + 60 x 50 ms
	CHECK(SubtitleTime::ShowFor(std::nullopt, std::string(5000, 'a')) == SubtitleTime::MAX_TEXT);
	CHECK(SubtitleTime::ShowFor(0.0, std::string(60, 'a')) == 4500ms);  // no length is no recording
	CHECK(SubtitleTime::ShowFor(-1.0, std::string(60, 'a')) == 4500ms);
}

TEST("SubtitleTime: reading time counts characters, not UTF-8 bytes")
{
	CHECK(SubtitleTime::Characters("abc") == 3);
	CHECK(SubtitleTime::Characters("\xC3\xA9t\xC3\xA9") == 3);           // été
	CHECK(SubtitleTime::Characters("\xE4\xBD\xA0\xE5\xA5\xBD") == 2);    // 你好
	CHECK(SubtitleTime::Reading(std::string(20, 'a')) == SubtitleTime::Reading("\xE4\xBD\xA0" + std::string(19, 'a')));
}
