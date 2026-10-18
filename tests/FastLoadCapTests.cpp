#include "Test.h"

#include "game/FastLoadCap.h"

using namespace std::chrono_literals;
using FastLoadCap::Cap;
using FastLoadCap::Change;
using FastLoadCap::Clock;

namespace
{
	const auto START = Clock::time_point{} + 1h;
}

TEST("fast load cap: a normal load has vsync off from the screen's open to its close")
{
	Cap cap;
	CHECK(!cap.Off());
	CHECK(cap.Opened(true, 20s, START) == Change::kOff);
	CHECK(cap.Off());
	CHECK(cap.OffFor(START + 4s) == 4s);
	// The watch ticks once a second meanwhile: nothing to do before the limit.
	for (auto now = START + 1s; now < START + 8s; now += 1s) {
		CHECK(cap.Tick(now, false) == Change::kNone);
	}
	CHECK(cap.Closed() == Change::kOnClosed);
	CHECK(!cap.Off());
	CHECK(cap.OffFor(START + 8s) == 0s);
	// Closed again (or a tick after): nothing is put back twice.
	CHECK(cap.Closed() == Change::kNone);
	CHECK(cap.Tick(START + 9s, false) == Change::kNone);
}

TEST("fast load cap: a loading screen up for the limit gets vsync back while it stays up, once")
{
	Cap cap;
	CHECK(cap.Opened(true, 20s, START) == Change::kOff);
	CHECK(cap.Tick(START + 19s, false) == Change::kNone);
	CHECK(cap.Tick(START + 20s, false) == Change::kOnLimit);
	CHECK(!cap.Off());
	// The screen stays up: the ticks go on with nothing to do, and its close puts nothing back a second time.
	CHECK(cap.Tick(START + 21s, false) == Change::kNone);
	CHECK(cap.Tick(START + 12min, false) == Change::kNone);
	CHECK(cap.Closed() == Change::kNone);
}

TEST("fast load cap: a new loading screen starts the uncapped stretch again")
{
	Cap cap;
	CHECK(cap.Opened(true, 20s, START) == Change::kOff);
	CHECK(cap.Tick(START + 20s, false) == Change::kOnLimit);
	CHECK(cap.Closed() == Change::kNone);
	// The next one, a minute on: off again, for its own 20 s.
	CHECK(cap.Opened(true, 20s, START + 1min) == Change::kOff);
	CHECK(cap.Tick(START + 1min + 19s, false) == Change::kNone);
	CHECK(cap.Tick(START + 1min + 20s, false) == Change::kOnLimit);
}

TEST("fast load cap: a second open event with no close heard keeps vsync off and counts from the new one")
{
	Cap cap;
	CHECK(cap.Opened(true, 20s, START) == Change::kOff);
	CHECK(cap.Opened(true, 20s, START + 15s) == Change::kNone);  // off already: nothing to flip
	CHECK(cap.Off());
	CHECK(cap.Tick(START + 20s, false) == Change::kNone);  // 5 s into the new stretch
	CHECK(cap.Tick(START + 35s, false) == Change::kOnLimit);
}

TEST("fast load cap: the frame watch finding the game stuck puts vsync back at once")
{
	Cap cap;
	CHECK(cap.Opened(true, 20s, START) == Change::kOff);
	CHECK(cap.Tick(START + 5s, false) == Change::kNone);
	CHECK(cap.Tick(START + 6s, true) == Change::kOnStalled);
	CHECK(!cap.Off());
	// Still stalled on the next ticks, and the screen's close later: nothing more to do.
	CHECK(cap.Tick(START + 7s, true) == Change::kNone);
	CHECK(cap.Closed() == Change::kNone);
	// The stall reported before a screen opens does not touch the next screen: only a stall while it is up.
	CHECK(cap.Opened(true, 20s, START + 1min) == Change::kOff);
	CHECK(cap.Tick(START + 1min + 1s, false) == Change::kNone);
	CHECK(cap.Off());
}

TEST("fast load cap: with fast loading off nothing is flipped, and the limit is this screen's setting")
{
	Cap cap;
	CHECK(cap.Opened(false, 20s, START) == Change::kNone);
	CHECK(!cap.Off());
	CHECK(cap.Tick(START + 1min, true) == Change::kNone);
	CHECK(cap.Closed() == Change::kNone);
	// Turned on in the menu, with a limit of its own, before the next screen.
	CHECK(cap.Opened(true, 5s, START + 2min) == Change::kOff);
	CHECK(cap.Tick(START + 2min + 4s, false) == Change::kNone);
	CHECK(cap.Tick(START + 2min + 5s, false) == Change::kOnLimit);
	// A limit of 0: back at the first tick, as good as off.
	CHECK(cap.Opened(true, 0s, START + 3min) == Change::kOff);
	CHECK(cap.Tick(START + 3min, false) == Change::kOnLimit);
}

TEST("fast load cap: which changes put vsync back, and their names")
{
	CHECK(!FastLoadCap::RestoresVsync(Change::kNone));
	CHECK(!FastLoadCap::RestoresVsync(Change::kOff));
	CHECK(FastLoadCap::RestoresVsync(Change::kOnClosed));
	CHECK(FastLoadCap::RestoresVsync(Change::kOnLimit));
	CHECK(FastLoadCap::RestoresVsync(Change::kOnStalled));
	CHECK(std::string_view{ FastLoadCap::Name(Change::kOnLimit) } == "the loading screen has been up for the limit");
	CHECK(std::string_view{ FastLoadCap::Name(Change::kNone) } == "none");
}
