#include "Test.h"

#include "game/StandInHold.h"

#include <numbers>

namespace
{
	constexpr float pi = std::numbers::pi_v<float>;
}

TEST("stand-in hold: headings are compared the short way round")
{
	CHECK(StandInHold::AngleBetween(1.0f, 1.0f) == 0.0f);
	CHECK(std::abs(StandInHold::AngleBetween(1.0f, 1.5f) - 0.5f) < 0.0001f);
	CHECK(std::abs(StandInHold::AngleBetween(1.5f, 1.0f) - 0.5f) < 0.0001f);
	CHECK(std::abs(StandInHold::AngleBetween(0.0f, pi) - pi) < 0.0001f);
	// Across north, and with a blended heading past a full turn or below zero.
	CHECK(std::abs(StandInHold::AngleBetween(0.01f, 6.27f) - 0.0232f) < 0.001f);
	CHECK(StandInHold::AngleBetween(-0.1f, 2.0f * pi - 0.1f) < 0.001f);
	CHECK(std::abs(StandInHold::AngleBetween(1.0f, 1.0f + 4.0f * pi + 0.5f) - 0.5f) < 0.001f);
	CHECK(std::abs(StandInHold::AngleBetween(-3.0f, 3.0f) - (2.0f * pi - 6.0f)) < 0.001f);
}

TEST("stand-in hold: a turn keeps its direction, the short way round")
{
	CHECK(std::abs(StandInHold::TurnBetween(1.0f, 1.5f) - 0.5f) < 0.0001f);
	CHECK(std::abs(StandInHold::TurnBetween(1.5f, 1.0f) + 0.5f) < 0.0001f);
	// Across north both ways: 6.27 to 0.01 is a small turn clockwise, 0.01 to 6.27 the same back.
	CHECK(std::abs(StandInHold::TurnBetween(6.27f, 0.01f) - 0.0232f) < 0.001f);
	CHECK(std::abs(StandInHold::TurnBetween(0.01f, 6.27f) + 0.0232f) < 0.001f);
	// Whole turns in between do not count.
	CHECK(std::abs(StandInHold::TurnBetween(1.0f, 1.0f + 4.0f * pi + 0.5f) - 0.5f) < 0.001f);
	CHECK(std::abs(StandInHold::TurnBetween(1.0f, 1.0f - 4.0f * pi - 0.5f) + 0.5f) < 0.001f);
	// Halfway between two headings across north is north, not south.
	const float from = 2.0f * pi - 0.2f;
	const float half = from + StandInHold::TurnBetween(from, 0.2f) * 0.5f;
	CHECK(StandInHold::AngleBetween(half, 0.0f) < 0.001f);
}

TEST("stand-in hold: a stand-in pulled off its spot on alternate frames is counted")
{
	// The friend stands still; on every other frame the stand-in is found 18 units away.
	StandInHold::Check check;
	for (int i = 0; i < 100; ++i) {
		check.Add(0.0f, 0.0f, i % 2 ? 18.0f : 0.0f);
	}
	CHECK(check.frames == 100);
	CHECK(check.off == 50);
	CHECK(check.worst == 18.0f);
}

TEST("stand-in hold: moving, or settling by a unit, is not counted")
{
	StandInHold::Check check;
	CHECK(!check.Add(12.0f, 300.0f, 15.0f));  // running: the target moved, the stand-in follows it
	CHECK(!check.Add(0.0f, 0.0f, 0.6f));      // standing, the feet settled on the ground
	CHECK(!check.Add(0.2f, 0.0f, 2.0f));
	CHECK(check.Add(0.0f, 0.0f, 2.8f));
	CHECK(check.frames == 3);
	CHECK(check.off == 1);
	CHECK(check.worst == 2.8f);
}

TEST("stand-in hold: a friend held still only because their next state is late is not counted")
{
	// Their last state said they run: the target holds there until the next one, but the stand-in runs on.
	StandInHold::Check check;
	CHECK(!check.Add(0.0f, 300.0f, 6.0f));
	CHECK(!check.Add(0.0f, 0.1f, 3.0f));
	CHECK(check.frames == 0);
	CHECK(check.off == 0);
}
