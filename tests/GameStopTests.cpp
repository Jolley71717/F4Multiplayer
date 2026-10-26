// How long our game has been stopped by a menu (GameStop::Clock, as NpcSync::Frame advances it each frame).

#include "Test.h"

#include "game/GameStop.h"

using namespace std::chrono_literals;

TEST("game stop: a stop's length counts from the frame it began")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	CHECK(clock.Advance(t0, false) == 0s);  // playing: no stop
	CHECK(clock.Advance(t0 + 1s, true) == 0s);  // the menu's first frame
	CHECK(clock.Advance(t0 + 1500ms, true) == 500ms);
	CHECK(clock.Advance(t0 + 7s, true) == 6s);
}

TEST("game stop: a frame of play ends it, and the next stop counts afresh")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	clock.Advance(t0, true);
	CHECK(clock.Advance(t0 + 4s, true) == 4s);
	CHECK(clock.Advance(t0 + 4100ms, false) == 0s);  // the menu closed for a frame
	CHECK(clock.Advance(t0 + 4200ms, true) == 0s);   // a new stop: four seconds before it don't count
	CHECK(clock.Advance(t0 + 6200ms, true) == 2s);
}

TEST("game stop: a load resets it")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	clock.Advance(t0, true);
	CHECK(clock.Advance(t0 + 3s, true) == 3s);
	clock.Reset();  // a loading screen
	CHECK(clock.Advance(t0 + 20s, true) == 0s);  // stopped after the load: counted from here, not from before it
	CHECK(clock.Advance(t0 + 21s, true) == 1s);
}
