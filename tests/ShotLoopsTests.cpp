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
	loops.Add(1, 1, t0);
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
	loops.Add(1, 1, t0);
	loops.Add(2, 2, t0 + 100ms);
	loops.Add(3, 3, t0 + 200ms);
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
		loops.Add(static_cast<Loops::Key>(i), i, now);
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

TEST("shot loops: a shooter's next shot keeps their loop running instead of starting another")
{
	Loops            loops;
	std::vector<int> ended;
	loops.Add(7, 1, t0);
	CHECK(loops.Extend(7, t0 + 100ms));
	CHECK(loops.EndDue(t0 + 150ms, [&](int a_h) { ended.push_back(a_h); }) == 0);  // 120 ms from the extension, not the start
	CHECK(loops.Active() == 1);
	CHECK(loops.EndDue(t0 + 100ms + Loops::SHOT_LENGTH, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK((ended == std::vector<int>{ 1 }));
}

TEST("shot loops: nothing to extend for another shooter, or once the loop has ended")
{
	Loops loops;
	loops.Add(7, 1, t0);
	CHECK(!loops.Extend(8, t0 + 10ms));
	loops.EndDue(t0 + 1s, [](int) {});
	CHECK(!loops.Extend(7, t0 + 1s));
	CHECK(loops.Active() == 0);
}

TEST("shot loops: an automatic burst is one sound from the first shot to a shot's worth after the last")
{
	// 10 shots a second for 2 seconds from one shooter: one loop, extended by every shot.
	Loops       loops;
	std::size_t ended = 0;
	int         started = 0;
	auto        now = t0;
	for (int i = 0; i < 20; ++i) {
		if (!loops.Extend(7, now)) {
			loops.Add(7, ++started, now);
		}
		ended += loops.EndDue(now, [](int) {});
		now += 100ms;
	}
	CHECK(started == 1);
	CHECK(loops.Active() == 1);
	CHECK(loops.EndDue(now + Loops::SHOT_LENGTH, [](int) {}) == 1);
	CHECK(ended == 0);
}
