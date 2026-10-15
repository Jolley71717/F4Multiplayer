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

TEST("shot loops: one shooter's loops can be ended at once, leaving the others running")
{
	Loops      loops;
	const auto now = Loops::Clock::now();
	loops.Add(7, 1, now);
	loops.Add(7, 2, now);
	loops.Add(9, 3, now);
	int ended = 0;
	CHECK(loops.EndFor(7, [&](int) { ++ended; }) == 2);
	CHECK(ended == 2);
	CHECK(loops.Active() == 1);
	CHECK(loops.EndFor(7, [](int) {}) == 0);
	CHECK(loops.EndDue(now + Loops::SHOT_LENGTH, [](int) {}) == 1);
}

TEST("shot loops: a shot sound that runs on far longer than a gunshot is ended, one that ends by itself is let be")
{
	// A sound mapping the shooter's game named can be another sound here (a plugin elsewhere in the load order), and a
	// sound can loop as a whole file, which the engine does not call a loop: neither would ever stop.
	Loops            loops;
	std::vector<int> ended;
	loops.Watch(7, 1, t0);
	loops.Watch(7, 2, t0 + 1s);
	CHECK(loops.Watched() == 2);
	CHECK(loops.Active() == 0);  // not a loop: a next shot starts a sound of its own
	CHECK(!loops.Extend(7, t0 + 1s));
	CHECK(loops.EndLong(t0 + Loops::LONGEST_SHOT - 1ms, [&](int a_h) { ended.push_back(a_h); }) == 0);
	CHECK(loops.EndLong(t0 + Loops::LONGEST_SHOT, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK((ended == std::vector<int>{ 1 }));
	CHECK(loops.EndLong(t0 + 1s + Loops::LONGEST_SHOT, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK((ended == std::vector<int>{ 1, 2 }));
	CHECK(loops.Watched() == 0);
}

TEST("shot loops: a semi-automatic volley never keeps more than a few seconds of shots watched")
{
	// 5 shots a second for a minute, ended (or let be) as each comes of age.
	Loops       loops;
	std::size_t ended = 0;
	auto        now = t0;
	for (int i = 0; i < 300; ++i) {
		loops.Watch(7, i, now);
		ended += loops.EndLong(now, [](int) {});
		now += 200ms;
	}
	CHECK(loops.Watched() <= 31);
	ended += loops.EndLong(now + Loops::LONGEST_SHOT, [](int) {});
	CHECK(ended == 300);
}

TEST("shot loops: a shooter's watched shots are ended with their loops when their actor goes")
{
	Loops      loops;
	const auto now = Loops::Clock::now();
	loops.Watch(7, 1, now);
	loops.Watch(9, 2, now);
	loops.Add(7, 3, now);
	std::vector<int> ended;
	CHECK(loops.EndFor(7, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK(loops.EndWatchedFor(7, [&](int a_h) { ended.push_back(a_h); }) == 1);
	CHECK((ended == std::vector<int>{ 3, 1 }));
	CHECK(loops.Watched() == 1);
	CHECK(loops.EndWatchedFor(7, [](int) {}) == 0);
}
