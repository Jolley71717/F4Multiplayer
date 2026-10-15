#include "Test.h"

#include "Protocol.h"
#include "game/StandInHold.h"
#include "game/StateBuffer.h"

#include <cmath>
#include <numbers>
#include <optional>

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

TEST("stand-in hold: a friend whose states stop coming while they run is not counted, however long they are late")
{
	// Their states come over the wire as the server relays them, into the buffer the stand-in is put by, once a frame
	// at 60 fps; what is counted goes by the speed the friend said, as RemotePlayers' CheckHold asks it. Past the
	// newest state the buffer stops drawing them (their speed in the drawn state goes to 0 after a moment), but they
	// said they run: the stand-in running on a few units is not something else moving it.
	using namespace std::chrono_literals;
	using StateBuffer::Clock;
	constexpr float run = 400.0f;
	StateBuffer::Buffer buffer;
	StandInHold::Check  check;
	const auto          start = Clock::time_point{} + 1h;
	std::uint32_t       tick = 0;
	const auto          send = [&](Clock::time_point a_now, float a_x, float a_speed) {
		Protocol::PlayerState state;
		state.sequence = tick + 1;
		state.worldspace = 0x3C;
		state.x = a_x;
		state.speed = a_speed;
		const auto batch = Protocol::DecodePlayerStates(Protocol::Encode(Protocol::PlayerStates{ ++tick, { { 1, state } } }));
		REQUIRE(batch);
		buffer.Push(batch->players[0].state, a_now);
	};
	std::optional<float> last;
	bool                 stoppedDrawn = false;  // a frame drawn standing (speed 0) while they said they run
	const auto           frame = [&](Clock::time_point a_now, float a_off) {
		buffer.Frame(a_now);
		const auto sampled = buffer.Sample(a_now);
		REQUIRE(sampled);
		const float moved = last ? std::abs(sampled->state.x - *last) : 0.0f;
		last = sampled->state.x;
		stoppedDrawn |= sampled->held && sampled->state.speed == 0.0f && sampled->OwnSpeed() > 0.0f;
		check.Add(moved, sampled->OwnSpeed(), a_off);
	};
	// A second of running, a state every 33 ms.
	auto now = start;
	for (; now < start + 1s; now += 16ms) {
		if ((now - start) % 33ms < 16ms) {
			send(now, run * std::chrono::duration<float>(now - start).count(), run);
		}
		frame(now, 0.0f);
	}
	// Then nothing for two seconds (their connection hitches), the stand-in running on 6 units a frame.
	for (const auto end = now + 2s; now < end; now += 16ms) {
		frame(now, 6.0f);
	}
	CHECK(stoppedDrawn);
	CHECK(check.frames == 0);
	CHECK(check.off == 0);
	// They stop and say so: those frames count, and one off its spot is counted off.
	for (const auto end = now + 500ms; now < end; now += 16ms) {
		if ((now - start) % 33ms < 16ms) {
			send(now, 400.0f, 0.0f);
		}
		frame(now, 0.0f);
	}
	const auto still = check.frames;
	CHECK(still > 0);
	frame(now, 6.0f);
	CHECK(check.frames == still + 1);
	CHECK(check.off == 1);
}
