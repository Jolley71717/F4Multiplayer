#include "Test.h"

#include "game/StandInTime.h"
#include "game/VatsTime.h"

#include <cmath>
#include <limits>

namespace
{
	bool Near(double a_left, double a_right, double a_slack = 1e-4) { return std::abs(a_left - a_right) <= a_slack; }

	// A stand-in walked for a_seconds of real time at 60 fps while our game runs at a_ourRate and its friend's at
	// a_senderRate. Its friend's states are stamped on real time, so its ground covered is speed times real seconds;
	// its legs travel speed times the time its graph was stepped. The graph's time for a_seconds, and the world's.
	struct Walk
	{
		double graph = 0.0;
		double world = 0.0;
	};

	Walk Simulate(double a_seconds, float a_ourRate, float a_senderRate = 1.0f, float a_frame = 1.0f / 60.0f)
	{
		Walk walk;
		const int frames = static_cast<int>(a_seconds / a_frame + 0.5);
		for (int i = 0; i < frames; ++i) {
			const float real = a_frame;
			const float world = real * a_ourRate;  // BSTimer.delta: the frame time with the multiplier in it
			const float step = world;                // what the engine's job hands Actor::UpdateAnimation (GetUpdateMult ~1)
			walk.graph += StandInTime::Step(step, world, real, a_senderRate);
			walk.world += world;
		}
		return walk;
	}
}

TEST("stand-in time: at full speed its graph is stepped exactly as the engine steps it")
{
	CHECK(StandInTime::Scale(0.016f, 0.016f) == 1.0f);
	CHECK(StandInTime::Step(0.016f, 0.016f, 0.016f) == 0.016f);
	const auto walk = Simulate(10.0, 1.0f);
	CHECK(walk.graph == walk.world);
}

TEST("stand-in time: our game on Jet alone, its legs play on real time, so its steps cover the ground it covers")
{
	CHECK(Near(StandInTime::Scale(0.005f, 0.016667f), 3.3333f, 1e-3));
	const auto walk = Simulate(10.0, 0.3f);
	CHECK(Near(walk.graph, 10.0, 1e-3));       // legs: 10 s of walk for 10 s of real ground
	CHECK(Near(walk.world, 3.0, 1e-3));        // the world's time: what its legs got before, a glide at 0.3x
}

TEST("stand-in time: in VATS without real time (0.04) its legs still play on real time, under the step cap")
{
	CHECK(Near(StandInTime::Scale(0.04f * 0.016667f, 0.016667f), 25.0f, 1e-2));
	const auto walk = Simulate(5.0, 0.04f);
	CHECK(Near(walk.graph, 5.0, 1e-3));
}

TEST("stand-in time: all games slowed together (a group slow motion), its legs stay on the slowed time")
{
	// The friend's states came at their slowed pace: k = their rate / ours = 1.
	CHECK(StandInTime::Scale(0.005f, 0.016667f, 0.3f) == 1.0f);
	const auto walk = Simulate(10.0, 0.3f, 0.3f);
	CHECK(walk.graph == walk.world);
	CHECK(Near(walk.graph, 3.0, 1e-3));
}

TEST("stand-in time: the friends' rate is the group's VATS slow motion while it is in effect, full speed else")
{
	// The hook's rate (RealTime::FriendsRate): with the host's group speed on and anyone in VATS every game runs at it,
	// so a stand-in's step is the engine's own; with nobody in VATS, or the group speed off, a slowed game is ours alone.
	CHECK(VatsTime::FriendsRate(VatsTime::Group(0.3f, false, 1)) == 0.3f);
	CHECK(VatsTime::FriendsRate(VatsTime::Group(0.3f, false, 0)) == 1.0f);
	CHECK(VatsTime::FriendsRate(VatsTime::Group(0.0f, true, 1)) == 1.0f);
	const float real = 1.0f / 60.0f;
	CHECK(StandInTime::Step(real * 0.3f, real * 0.3f, real, VatsTime::FriendsRate(0.3f)) == real * 0.3f);
	CHECK(Near(StandInTime::Step(real * 0.3f, real * 0.3f, real, VatsTime::FriendsRate(0.0f)), real));
}

TEST("stand-in time: a friend slowed alone has its legs slowed to their pace on our full-speed game")
{
	CHECK(Near(StandInTime::Scale(0.016667f, 0.016667f, 0.3f), 0.3f, 1e-4));
	const auto walk = Simulate(10.0, 1.0f, 0.3f);
	CHECK(Near(walk.graph, 3.0, 1e-3));
}

TEST("stand-in time: a paused game stays paused, and no step is made from nothing")
{
	CHECK(StandInTime::Scale(0.0f, 0.016f) == 1.0f);
	CHECK(StandInTime::Step(0.0f, 0.0f, 0.016f) == 0.0f);   // paused: delta 0, the engine's step 0
	CHECK(StandInTime::Step(0.0f, 0.005f, 0.016f) == 0.0f); // a zero step stays zero whatever the scale
	CHECK(StandInTime::Step(-0.01f, 0.005f, 0.016f) == -0.01f);
	CHECK(StandInTime::Step(0.016f, 0.016f, 0.0f) == 0.016f);  // no real time read: left alone
}

TEST("stand-in time: a scaled step is held to the cap, and an engine step already longer is never shortened")
{
	// A long hitch slowed 25 times: 0.2 s of world is 5 s scaled, held to MAX_STEP.
	CHECK(StandInTime::Step(0.2f, 0.2f, 5.0f) == StandInTime::MAX_STEP);
	// 0.1 s at 0.3: 0.333 s scaled, held to MAX_STEP.
	CHECK(StandInTime::Step(0.1f, 0.1f, 0.3333f) == StandInTime::MAX_STEP);
	// The engine's own 0.4 s step (longer than the cap) scaled up stays 0.4, not cut to 0.25.
	CHECK(StandInTime::Step(0.4f, 0.4f, 1.2f) == 0.4f);
	// A friend slowed alone shortens it (k < 1): no cap in play.
	CHECK(Near(StandInTime::Step(0.4f, 0.4f, 0.4f, 0.5f), 0.2f, 1e-6));
}

TEST("stand-in time: the engine's per-actor factor in its step is kept, only the world's slowdown is undone")
{
	// The engine's step for an actor is BSTimer.delta times VATS::GetUpdateMult(actor): a factor of 0.5 stays in it.
	const float world = 0.005f, real = 0.016667f;
	CHECK(Near(StandInTime::Step(world * 0.5f, world, real), real * 0.5f, 1e-5));
}

TEST("stand-in time: a time that is not one (NaN, infinite, negative) leaves the step as the engine made it")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(StandInTime::Scale(nan, 0.016f) == 1.0f);
	CHECK(StandInTime::Scale(0.016f, nan) == 1.0f);
	CHECK(StandInTime::Scale(inf, 0.016f) == 1.0f);
	CHECK(StandInTime::Scale(0.016f, inf) == 1.0f);
	CHECK(StandInTime::Scale(-0.005f, 0.016f) == 1.0f);
	CHECK(StandInTime::Scale(0.005f, 0.016f, nan) == 1.0f);
	CHECK(StandInTime::Scale(0.005f, 0.016f, 0.0f) == 1.0f);
	CHECK(StandInTime::Scale(std::numeric_limits<float>::denorm_min(), 1.0f) == 1.0f);  // k overflows: left alone
	CHECK(StandInTime::Step(0.016f, nan, 0.016f) == 0.016f);
	CHECK(std::isnan(StandInTime::Step(nan, 0.005f, 0.016f)));
}

TEST("stand-in time: a scale within a thousandth of 1 is 1, so a game at full speed never has its steps rounded")
{
	CHECK(StandInTime::Scale(0.016f, 0.016f * 1.0005f) == 1.0f);
	CHECK(StandInTime::Scale(0.016f, 0.016f * 0.9995f) == 1.0f);
	CHECK(StandInTime::Scale(0.016f, 0.016f * 1.01f) != 1.0f);
}

static_assert(StandInTime::Step(0.005f, 0.005f, 0.005f) == 0.005f);
static_assert(StandInTime::Scale(0.0f, 0.016f) == 1.0f);
