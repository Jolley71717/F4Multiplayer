#include "Test.h"

#include "game/StandInYield.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>

// A friend's stand-in steps aside for our player pressed against it (StandInYield::Frame), called once a frame as
// RemotePlayers does.
namespace
{
	using namespace std::chrono_literals;
	using StandInYield::Clock;
	using StandInYield::Point;
	using StandInYield::State;
	using StandInYield::Us;

	constexpr float CLEAR = StandInSpace::CLEAR;
	constexpr float BODIES = 56.0f;  // two bodies touching, as measured in a game
	constexpr auto  FRAME = 16ms;

	const auto start = Clock::time_point{} + 1h;

	// What a run of frames did.
	struct Run
	{
		int   steps = 0;      // times it began to step aside
		int   returns = 0;    // times it began to go back
		float furthest = 0.0f;  // the most it was drawn in one frame
		Point drawn;          // where it was drawn last
		Clock::time_point now = start;
	};

	float Ground(const Point& a_a, const Point& a_b)
	{
		return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y);
	}

	// a_frames frames with nothing changing.
	void Hold(Run& a_run, State& a_state, const Point& a_friend, const Us& a_us, int a_frames, bool a_allowed = true, float a_clear = CLEAR)
	{
		for (int i = 0; i < a_frames; ++i) {
			a_run.now += FRAME;
			const Point before = a_state.last ? a_run.drawn : a_friend;
			a_run.drawn = StandInYield::Frame(a_state, a_friend, a_us, a_clear, a_allowed, a_run.now);
			a_run.steps += a_state.stepped;
			a_run.returns += a_state.returned;
			a_run.furthest = std::max(a_run.furthest, Ground(before, a_run.drawn));
		}
	}

	constexpr int FramesIn(std::chrono::milliseconds a_time)
	{
		return static_cast<int>(a_time / FRAME);
	}
}

TEST("stand-in yield: pushed at dead-centre without moving, it steps a lane to the right of our way after the wait, not before")
{
	State       state;
	Run         run;
	const Point there{ 100.0f, 200.0f + BODIES, 30.0f };
	const Us    us{ .at = { 100.0f, 200.0f, 30.0f }, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };

	Hold(run, state, there, us, FramesIn(StandInYield::WEDGED) - 1);
	CHECK(run.steps == 0);
	CHECK(run.drawn.x == there.x);
	CHECK(run.drawn.y == there.y);

	Hold(run, state, there, us, 4);
	CHECK(run.steps == 1);
	CHECK(state.aside);

	Hold(run, state, there, us, FramesIn(1s));
	CHECK(run.steps == 1);  // not asked again once it is out of our way
	CHECK(std::abs(run.drawn.x - (there.x + StandInYield::Lane(CLEAR))) < 0.01f);  // facing +y, our right is +x
	CHECK(std::abs(run.drawn.y - there.y) < 0.01f);
	CHECK(run.drawn.z == there.z);
	CHECK(Ground(run.drawn, us.at) > BODIES);  // clear of our body
	// drawn there, not put there at once
	CHECK(run.furthest <= StandInYield::SLIDE * 0.016f + 0.01f);
}

TEST("stand-in yield: it steps to the side of our way it is already on")
{
	State       state;
	Run         run;
	const Point there{ -10.0f, 55.0f, 0.0f };  // a little to our left
	const Us    us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };

	Hold(run, state, there, us, FramesIn(2s));
	CHECK(run.steps == 1);
	CHECK(std::abs(run.drawn.x - -StandInYield::Lane(CLEAR)) < 0.01f);  // a lane to the left of the line we push along
	CHECK(std::abs(run.drawn.y - there.y) < 0.01f);
}

TEST("stand-in yield: the way we push counts, not the way we look")
{
	const float quarter = std::numbers::pi_v<float> / 2.0f;
	const Point there{ BODIES, 0.0f, 0.0f };  // along +x from us

	// facing +y and pushing to our right: at it
	{
		State state;
		Run   run;
		Hold(run, state, there, { .at = {}, .heading = 0.0f, .inputX = 1.0f, .inputY = 0.0f }, FramesIn(2s));
		CHECK(run.steps == 1);
		CHECK(std::abs(run.drawn.x - there.x) < 0.01f);
		CHECK(std::abs(run.drawn.y - -StandInYield::Lane(CLEAR)) < 0.01f);  // pushing along +x, our way's right is -y
	}
	// facing it and pushing ahead: the same
	{
		State state;
		Run   run;
		Hold(run, state, there, { .at = {}, .heading = quarter, .inputX = 0.0f, .inputY = 1.0f }, FramesIn(2s));
		CHECK(run.steps == 1);
		CHECK(std::abs(run.drawn.x - there.x) < 0.01f);
		CHECK(std::abs(run.drawn.y - -StandInYield::Lane(CLEAR)) < 0.01f);
	}
	// facing it and pushing back, away from it: nothing
	{
		State state;
		Run   run;
		Hold(run, state, there, { .at = {}, .heading = quarter, .inputX = 0.0f, .inputY = -1.0f }, FramesIn(2s));
		CHECK(run.steps == 0);
		CHECK(run.drawn.x == there.x);
		CHECK(run.drawn.y == there.y);
	}
	// facing it and pushing sideways, along it: nothing
	{
		State state;
		Run   run;
		Hold(run, state, there, { .at = {}, .heading = quarter, .inputX = 1.0f, .inputY = 0.0f }, FramesIn(2s));
		CHECK(run.steps == 0);
	}
}

TEST("stand-in yield: nothing steps aside when we do not push, are not against it, are on another floor, or it does not follow its friend")
{
	const Us pushing{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };
	// standing by it
	{
		State state;
		Run   run;
		Hold(run, state, { 0.0f, BODIES, 0.0f }, { .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 0.0f }, FramesIn(3s));
		CHECK(run.steps == 0);
	}
	// a stick barely moved
	{
		State state;
		Run   run;
		Hold(run, state, { 0.0f, BODIES, 0.0f }, { .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 0.3f }, FramesIn(3s));
		CHECK(run.steps == 0);
	}
	// something else holds us: it is further off than two bodies touching
	{
		State state;
		Run   run;
		Hold(run, state, { 0.0f, StandInYield::Touch(CLEAR) + 1.0f, 0.0f }, pushing, FramesIn(3s));
		CHECK(run.steps == 0);
	}
	// on the floor above
	{
		State state;
		Run   run;
		Hold(run, state, { 0.0f, BODIES, StandInSpace::TALL + 10.0f }, pushing, FramesIn(3s));
		CHECK(run.steps == 0);
	}
	// down, in furniture, or during a loading screen
	{
		State state;
		Run   run;
		Hold(run, state, { 0.0f, BODIES, 0.0f }, pushing, FramesIn(3s), false);
		CHECK(run.steps == 0);
		CHECK(run.drawn.x == 0.0f);
		CHECK(run.drawn.y == BODIES);
	}
}

TEST("stand-in yield: a player who moves, sliding round it, is not wedged")
{
	State       state;
	Run         run;
	const Point there{ 0.0f, BODIES, 0.0f };
	Us          us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };

	// 12 units a second to the side, pushing at it all the while: against it for the whole 2 s
	for (int i = 0; i < FramesIn(2s); ++i) {
		us.at.x += 0.2f;
		Hold(run, state, there, us, 1);
	}
	CHECK(Ground(us.at, there) < StandInYield::Touch(CLEAR));
	CHECK(run.steps == 0);
	CHECK(run.drawn.x == there.x);

	// and once we stop moving, still pushing, it steps aside: to our left, where it is by now
	Hold(run, state, there, us, FramesIn(StandInYield::WEDGED) + 2);
	CHECK(run.steps == 1);
	Hold(run, state, there, us, FramesIn(1s));
	CHECK(run.drawn.x < there.x);
}

TEST("stand-in yield: we walk on through where the friend stands, and it goes back once we are clear of their spot")
{
	State       state;
	Run         run;
	const Point there{ 0.0f, BODIES, 0.0f };
	Us          us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };
	float       closest = 1000.0f;

	// We walk at 300 units a second and no closer to where it is drawn than two bodies touching (dead-centre, nothing
	// slides us round it).
	for (int i = 0; i < FramesIn(4s); ++i) {
		Hold(run, state, there, us, 1);
		const Point next{ us.at.x, us.at.y + 300.0f * 0.016f, us.at.z };
		if (Ground(next, run.drawn) >= BODIES) {
			us.at = next;
		}
		closest = std::min(closest, Ground(us.at, run.drawn));
	}
	CHECK(run.steps == 1);
	CHECK(run.returns == 1);
	CHECK(us.at.y > there.y + StandInYield::Release(CLEAR));  // past them
	CHECK(closest >= BODIES - 0.01f);                         // never through its body
	CHECK(!state.aside);
	CHECK(!StandInYield::Off(state));
	CHECK(run.drawn.x == there.x);  // back where they stand
	CHECK(run.drawn.y == there.y);
	CHECK(run.furthest <= StandInYield::SLIDE * 0.016f + 0.01f);  // and drawn back, not put back
}

TEST("stand-in yield: it follows its friend from where it is drawn aside, and goes back when they walk away from us")
{
	State state;
	Run   run;
	Point there{ 0.0f, BODIES, 0.0f };
	const Us us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };

	Hold(run, state, there, us, FramesIn(2s));
	CHECK(run.steps == 1);
	there.y += 10.0f;  // they shift a little: it is beside their new spot
	Hold(run, state, there, us, 1);
	CHECK(std::abs(run.drawn.x - (there.x + StandInYield::Lane(CLEAR))) < 0.01f);
	CHECK(std::abs(run.drawn.y - there.y) < 0.01f);
	CHECK(run.returns == 0);

	there.y += 200.0f;  // and walk off
	Hold(run, state, there, us, FramesIn(1s));
	CHECK(run.returns == 1);
	CHECK(run.drawn.x == there.x);
	CHECK(run.drawn.y == there.y);
}

TEST("stand-in yield: when it stops following its friend it is at their spot at once")
{
	State       state;
	Run         run;
	const Point there{ 0.0f, BODIES, 0.0f };
	const Us    us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };

	Hold(run, state, there, us, FramesIn(2s));
	CHECK(StandInYield::Off(state));
	Hold(run, state, there, us, 1, false);  // they sit down, or one of us goes down
	CHECK(run.returns == 1);
	CHECK(!state.aside);
	CHECK(!StandInYield::Off(state));
	CHECK(run.drawn.x == there.x);
	CHECK(run.drawn.y == there.y);
	Hold(run, state, there, us, 5, false);
	CHECK(run.returns == 1);  // said once
}

TEST("stand-in yield: a long frame draws it no further than a tenth of a second's worth")
{
	State       state;
	const Point there{ 0.0f, BODIES, 0.0f };
	const Us    us{ .at = {}, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f };
	auto        now = start;

	(void)StandInYield::Frame(state, there, us, CLEAR, true, now);
	now += StandInYield::WEDGED;
	const auto first = StandInYield::Frame(state, there, us, CLEAR, true, now);  // a hitch of the whole wait: it steps aside
	CHECK(state.aside);
	CHECK(Ground(first, there) > 0.0f);
	CHECK(Ground(first, there) <= StandInYield::SLIDE * StandInYield::MAX_STEP + 0.01f);
	now += 5s;
	const auto second = StandInYield::Frame(state, there, us, CLEAR, true, now);
	CHECK(Ground(second, there) > Ground(first, there));
	CHECK(Ground(second, first) <= StandInYield::SLIDE * StandInYield::MAX_STEP + 0.01f);
}

TEST("stand-in yield: pushed at again where it is drawn aside, it is never drawn further than two lanes from its friend")
{
	State       state;
	Run         run;
	const Point there{};
	const float lane = StandInYield::Lane(CLEAR);
	// drawn 140 units along +x already, and we push at it there from within reach of their spot
	state.aside = true;
	state.wantX = 140.0f;
	state.offX = 140.0f;
	const Us us{ .at = { 79.0f, 0.0f, 0.0f }, .heading = 0.0f, .inputX = 1.0f, .inputY = 0.0f };

	Hold(run, state, there, us, FramesIn(3s));
	CHECK(run.steps >= 1);
	CHECK(std::hypot(state.wantX, state.wantY) <= 2.0f * lane + 0.01f);
	CHECK(Ground(run.drawn, there) <= 2.0f * lane + 0.01f);
}

TEST("stand-in yield: numbers that are not numbers leave it where its friend stands")
{
	State       state;
	Run         run;
	const Point there{ 0.0f, BODIES, 0.0f };
	const float nan = std::numeric_limits<float>::quiet_NaN();

	Hold(run, state, there, { .at = { nan, 0.0f, 0.0f }, .heading = 0.0f, .inputX = 0.0f, .inputY = 1.0f }, FramesIn(2s));
	CHECK(run.steps == 0);
	CHECK(run.drawn.x == there.x);
	Hold(run, state, there, { .at = {}, .heading = nan, .inputX = 0.0f, .inputY = 1.0f }, FramesIn(2s));
	CHECK(run.steps == 0);
	Hold(run, state, there, { .at = {}, .heading = 0.0f, .inputX = nan, .inputY = 1.0f }, FramesIn(2s));
	CHECK(run.steps == 0);
	CHECK(run.drawn.y == there.y);
	CHECK(!StandInYield::Off(state));
}
