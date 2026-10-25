#include "Test.h"

#include "game/DrawMatch.h"

using namespace std::chrono_literals;
using DrawMatch::Do;
using DrawMatch::Next;
using DrawMatch::Track;

namespace
{
	const auto START = DrawMatch::Clock::time_point{} + 1h;
	const auto NONE = DrawMatch::Clock::time_point{};
}

TEST("DrawMatch: a stand-in that matches its friend is asked nothing")
{
	Track track;
	CHECK(Next(track, false, false, false, NONE, START) == Do::kNothing);
	CHECK(Next(track, true, true, false, NONE, START + 1s) == Do::kNothing);
	CHECK(track.asks == 0);
}

TEST("DrawMatch: a draw the stand-in never takes is asked a few times a second apart, then let be")
{
	Track track;
	int   draws = 0;
	for (int ms = 0; ms < 60000; ms += 16) {  // a minute of frames
		draws += Next(track, true, false, false, NONE, START + std::chrono::milliseconds(ms)) == Do::kDraw;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
	CHECK(track.gaveUp);
	CHECK(track.giveUps == 1);
	// Asks a second apart.
	Track spaced;
	CHECK(Next(spaced, true, false, false, NONE, START) == Do::kDraw);
	CHECK(Next(spaced, true, false, false, NONE, START + 500ms) == Do::kNothing);
	CHECK(Next(spaced, true, false, false, NONE, START + 1s) == Do::kDraw);
}

TEST("DrawMatch: a stand-in put away again after each draw stops being drawn")
{
	Track track;
	int   draws = 0;
	auto  now = START;
	for (int i = 0; i < 30; ++i) {
		draws += Next(track, true, false, false, NONE, now) == Do::kDraw;
		now += 500ms;
		CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);  // the draw took, for a moment
		now += 600ms;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
	CHECK(track.giveUps == 1);
}

TEST("DrawMatch: the friend drawing or holstering again, or a gear change, gives the asks back")
{
	Track track;
	auto  now = START;
	for (int i = 0; i < 10; ++i, now += 1s) {
		(void)Next(track, true, false, false, NONE, now);
	}
	CHECK(track.gaveUp);
	// They holster: the stand-in's weapon is away already, nothing to ask; then they draw again.
	CHECK(Next(track, false, false, false, NONE, now) == Do::kNothing);
	CHECK(!track.gaveUp);
	now += 1s;
	CHECK(Next(track, true, false, false, NONE, now) == Do::kDraw);
	for (int i = 0; i < 10; ++i) {
		now += 1s;
		(void)Next(track, true, false, false, NONE, now);
	}
	CHECK(track.gaveUp);
	// Its gear changed: a redraw, which holsters first when drawn, and draws when not.
	now += 1s;
	CHECK(Next(track, true, false, true, NONE, now) == Do::kDraw);
	now += 1s;
	CHECK(Next(track, true, true, true, NONE, now) == Do::kHolster);
}

TEST("DrawMatch: no ask before the caller's wait is over (an equip or a get-up playing)")
{
	Track track;
	CHECK(Next(track, true, false, true, START + 1s, START) == Do::kNothing);
	CHECK(Next(track, true, false, true, START + 1s, START + 1s) == Do::kDraw);
}

TEST("DrawMatch: a state that held a while gives the next mismatch its asks back")
{
	Track track;
	auto  now = START;
	for (int i = 0; i < 3; ++i, now += 1s) {
		CHECK(Next(track, true, false, false, NONE, now) == Do::kDraw);
	}
	CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);
	now += DrawMatch::STABLE;
	CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);
	CHECK(track.tries == 0);
	// Put away by something (sitting down, say): drawn again, with every ask.
	int draws = 0;
	for (int i = 0; i < 10; ++i) {
		now += 1s;
		draws += Next(track, true, false, false, NONE, now) == Do::kDraw;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
}
