#include "Test.h"

#include "game/VoiceLevel.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace VoiceLevel;

TEST("voice level: decibels from samples: silence, nothing, and full scale")
{
	CHECK(Dbfs({}) == SILENT_DB);
	const std::array<std::int16_t, 4> silent{};
	CHECK(Dbfs(silent) == SILENT_DB);
	const std::array<std::int16_t, 4> full{ 32767, -32768, 32767, -32768 };
	CHECK(std::abs(Dbfs(full)) < 0.01f);
	const std::array<std::int16_t, 4> tenth{ 3277, -3277, 3277, -3277 };
	CHECK(std::abs(Dbfs(tenth) + 20.0f) < 0.01f);
}

TEST("voice level: brackets light with the level and fade over FADE")
{
	const auto start = Clock::now();
	Meter      meter;
	meter.Start(start);
	CHECK((meter.At(start) == Look{ FADED, FADED }));
	meter.Piece(SPEECH_DB - 1.0f, start);
	CHECK((meter.At(start) == Look{ FADED, FADED }));
	meter.Piece(SPEECH_DB, start);
	CHECK((meter.At(start) == Look{ 1.0f, FADED }));
	meter.Piece(LOUD_DB, start);
	CHECK((meter.At(start) == Look{ 1.0f, 1.0f }));
	const auto half = meter.At(start + FADE / 2);
	CHECK(half.inner > FADED && half.inner < 1.0f);
	CHECK(half.outer == half.inner);
	CHECK((meter.At(start + FADE) == Look{ FADED, FADED }));
}

TEST("voice level: a new press starts the count over")
{
	const auto start = Clock::now();
	Meter      meter;
	meter.Start(start);
	meter.Piece(SILENT_DB, start);
	CHECK(meter.Describe() == "pieces=1 mean=-120 loudest=-120 dB");
	meter.Start(start);
	CHECK(meter.Describe() == "pieces=0 mean=-120 loudest=-120 dB");
}
