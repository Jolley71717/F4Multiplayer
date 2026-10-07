#include "Test.h"

#include "game/ShotLoops.h"

#include <vector>

// The looping-gunshot bookkeeping behind WeaponFire: a replayed automatic-weapon shot starts a
// sound that would loop forever; it must be ended after one shot's worth, and only then.
namespace
{
	using Loops = ShotLoops<int>;
	using namespace std::chrono_literals;

	const auto t0 = Loops::Clock::time_point{} + 1h;
}

TEST("shot loops: a loop is ended once it is a shot's worth old, not before")
{
	Loops            loops;
	std::vector<int> ended;
	loops.Add(1, t0);
	CHECK(loops.EndDue(t0 + 50ms, [&](int a_h) { ended.push_back(a_h); }) == 0);
	CHECK(loops.Active() == 1);
	CHECK(ended.empty());
	CHECK(loops.EndDue(t0 + Loops::SHOT_LENGTH, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK(loops.Active() == 0);
	REQUIRE(ended.size() == 1);
	CHECK(ended[0] == 1);
}

TEST("shot loops: each loop is ended exactly once, in its own time")
{
	Loops            loops;
	std::vector<int> ended;
	loops.Add(1, t0);
	loops.Add(2, t0 + 100ms);
	loops.Add(3, t0 + 200ms);
	CHECK(loops.EndDue(t0 + 150ms, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK((ended == std::vector<int>{ 1 }));
	CHECK(loops.Active() == 2);
	CHECK(loops.EndDue(t0 + 150ms, [&](int a_h) { ended.push_back(a_h); }) == 0);  // not twice
	CHECK(loops.EndDue(t0 + 1s, [&](int a_h) { ended.push_back(a_h); }) == 2);
	CHECK((ended == std::vector<int>{ 1, 2, 3 }));
	CHECK(loops.Active() == 0);
}

TEST("shot loops: a burst of replayed shots never leaves a loop running")
{
	// 20 shots a second for 3 seconds (an automatic weapon), then silence.
	Loops            loops;
	std::size_t      ended = 0;
	auto             now = t0;
	for (int i = 0; i < 60; ++i) {
		loops.Add(i, now);
		ended += loops.EndDue(now, [](int) {});
		now += 50ms;
	}
	CHECK(loops.Active() <= 3);  // only the newest shots are still sounding
	ended += loops.EndDue(now + 1s, [](int) {});
	CHECK(ended == 60);
	CHECK(loops.Active() == 0);
}

TEST("shot loops: nothing to end when nothing was added")
{
	Loops loops;
	CHECK(loops.EndDue(t0, [](int) {}) == 0);
	CHECK(loops.Active() == 0);
}
