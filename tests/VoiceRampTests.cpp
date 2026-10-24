#include "Test.h"

#include "game/VoiceRamp.h"

#include <cstdlib>
#include <vector>

using namespace std::chrono_literals;

TEST("VoiceRamp: a piece after silence starts at silence and rises to its own level, with no step")
{
	std::vector<std::int16_t> piece(800, 20000);
	VoiceRamp::FadeIn(piece);
	CHECK(piece[0] == 0);
	for (std::size_t i = 1; i < VoiceRamp::RAMP_SAMPLES; ++i) {
		CHECK(piece[i] >= piece[i - 1]);
		CHECK(std::abs(piece[i] - piece[i - 1]) <= 20000 / static_cast<int>(VoiceRamp::RAMP_SAMPLES) + 1);
	}
	for (std::size_t i = VoiceRamp::RAMP_SAMPLES; i < piece.size(); ++i) {
		CHECK(piece[i] == 20000);  // the rest untouched
	}
	std::vector<std::int16_t> negative(200, -32768);
	VoiceRamp::FadeIn(negative);
	CHECK(negative[0] == 0);
	CHECK(negative[VoiceRamp::RAMP_SAMPLES] == -32768);
}

TEST("VoiceRamp: a piece shorter than the ramp is faded over its own length")
{
	std::vector<std::int16_t> piece(10, 1000);
	VoiceRamp::FadeIn(piece);
	CHECK(piece[0] == 0);
	CHECK(piece[9] == 900);
	std::vector<std::int16_t> none;
	VoiceRamp::FadeIn(none);  // nothing to do, nothing read
	CHECK(none.empty());
}

TEST("VoiceRamp: an empty queue soon after the last piece is an underrun, not a new sentence")
{
	CHECK(VoiceRamp::Underrun(true, true, 40ms));
	CHECK(!VoiceRamp::Underrun(false, true, 40ms));  // still playing
	CHECK(!VoiceRamp::Underrun(true, false, 40ms));  // the first piece we hear of them
	CHECK(!VoiceRamp::Underrun(true, true, 5s));     // they stopped talking, and start again
}
