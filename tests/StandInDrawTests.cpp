// A stand-in in our own cell that is not drawn (game/StandInDraw.h): asked to build its 3D again after DRAW_WAIT, placed again
// after that, at most MAX_REPLACES times, then left alone and said once; drawn again, it starts over.

#include "Test.h"

#include "game/StandInDraw.h"

using namespace StandInDraw;
using namespace std::chrono_literals;

namespace
{
	const auto t0 = Clock::time_point{} + 1h;

	Facts Expected(bool a_has3D, bool a_culled = false, float a_fade = 1.0f)
	{
		return { .expected = true, .has3D = a_has3D, .culled = a_culled, .fade = a_fade };
	}
}

TEST("StandInDraw: drawn is a loaded 3D that is neither culled nor faded out")
{
	CHECK(Drawn(Expected(true)));
	CHECK(!Drawn(Expected(false)));
	CHECK(!Drawn(Expected(true, true)));
	CHECK(!Drawn(Expected(true, false, 0.0f)));
	CHECK(Drawn(Expected(true, false, 0.2f)));
}

TEST("StandInDraw: no 3D for DRAW_WAIT is a rebuild, then a fresh stand-in twice, then left alone and said once")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, Expected(false), t0) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + DRAW_WAIT) == Then::kRebuild);
	// the rebuild gets DRAW_WAIT of its own
	CHECK(Frame(track, Expected(false), t0 + DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + 2 * DRAW_WAIT) == Then::kReplace);
	CHECK(track.replaces == 1);
	// the fresh one, watched afresh: its own wait, its own rebuild, then the second replacement
	Placed(track);
	auto t = t0 + 2 * DRAW_WAIT + 100ms;
	CHECK(Frame(track, Expected(false), t) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, Expected(false), t + 2 * DRAW_WAIT) == Then::kReplace);
	CHECK(track.replaces == 2);
	Placed(track);
	t = t + 2 * DRAW_WAIT + 100ms;
	CHECK(Frame(track, Expected(false), t) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, Expected(false), t + 2 * DRAW_WAIT) == Then::kGiveUp);
	CHECK(Frame(track, Expected(false), t + 3 * DRAW_WAIT) == Then::kNothing);
	CHECK(track.replaces == 2);
}

TEST("StandInDraw: a culled or faded-out 3D counts as not drawn")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, Expected(true, true), t0) == Then::kNothing);
	CHECK(Frame(track, Expected(true, true), t0 + DRAW_WAIT) == Then::kRebuild);
	Track faded;
	Placed(faded);
	CHECK(Frame(faded, Expected(true, false, 0.0f), t0) == Then::kNothing);
	CHECK(Frame(faded, Expected(true, false, 0.0f), t0 + DRAW_WAIT) == Then::kRebuild);
}

TEST("StandInDraw: drawn again, the spell is over; not expected, nothing is counted")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, Expected(false), t0) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, Expected(false), t0 + 2 * DRAW_WAIT) == Then::kReplace);
	Placed(track);
	// the fresh one is drawn: replaces forgotten
	CHECK(Frame(track, Expected(true), t0 + 2 * DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(track.replaces == 0);
	// drawn, then not expected a while (a load, power armor), then undrawn: the wait starts from when it is expected again
	CHECK(Frame(track, { .expected = false }, t0 + 3 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, { .expected = false }, t0 + 10 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + 10 * DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + 11 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, Expected(false), t0 + 11 * DRAW_WAIT + 1s) == Then::kRebuild);
}
