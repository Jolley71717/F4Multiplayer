#include "Test.h"

#include "HangDetector.h"

using namespace std::chrono_literals;
using HangWatch::Detector;
using HangWatch::Sample;
using Event = HangWatch::Detector::Event;

namespace
{
	// The defaults: a hang after 15 s, 60 s on a loading screen, and Windows' five seconds before
	// it calls a window not responding.
	Detector MakeDetector() { return Detector{ 15s, 60s, 5s }; }

	constexpr Sample STALLED{ .responsive = false };
	constexpr Sample STALLED_LOADING{ .responsive = false, .loading = true };
	constexpr Sample ANSWERS{};
}

TEST("hang watch: a game that keeps answering never hangs")
{
	auto d = MakeDetector();
	auto now = HangWatch::Clock::time_point{} + 1h;
	for (int i = 0; i < 600; ++i) {
		now += 1s;
		CHECK(d.Tick(now, ANSWERS).event == Event::kNone);
		CHECK(d.Tick(now, Sample{ .loading = true }).event == Event::kNone);
	}
	CHECK(!d.Hung());
}

TEST("hang watch: a stall is reported once at the limit, and once when the game answers again")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	CHECK(d.Tick(start, ANSWERS).event == Event::kNone);

	// First seen not responding at +1 s: it has been stalled for five seconds already.
	CHECK(d.Tick(start + 1s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 10s, STALLED).event == Event::kNone);  // 14 s
	const auto hang = d.Tick(start + 11s, STALLED);              // 15 s
	CHECK(hang.event == Event::kHang);
	CHECK(hang.stalled == 15s);
	CHECK(d.Hung());
	for (auto t = 12s; t < 60s; t += 1s) {
		CHECK(d.Tick(start + t, STALLED).event == Event::kNone);
	}

	const auto back = d.Tick(start + 60s, ANSWERS);
	CHECK(back.event == Event::kRecovered);
	CHECK(back.stalled == 64s);
	CHECK(!d.Hung());
	CHECK(d.Tick(start + 61s, ANSWERS).event == Event::kNone);

	// The next freeze is a new hang.
	CHECK(d.Tick(start + 70s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 80s, STALLED).event == Event::kHang);
}

TEST("hang watch: short stalls never add up to a hang")
{
	auto d = MakeDetector();
	auto now = HangWatch::Clock::time_point{} + 1h;
	for (int i = 0; i < 20; ++i) {
		// Nine looks at a game not responding (up to 13 s stalled), then one answer.
		for (int s = 0; s < 9; ++s) {
			now += 1s;
			CHECK(d.Tick(now, STALLED).event == Event::kNone);
		}
		now += 1s;
		CHECK(d.Tick(now, ANSWERS).event == Event::kNone);
	}
}

TEST("hang watch: a loading screen gets its own, longer limit")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	CHECK(d.Tick(start, STALLED_LOADING).event == Event::kNone);
	CHECK(d.Tick(start + 30s, STALLED_LOADING).event == Event::kNone);  // 35 s: a slow load, not a hang
	CHECK(d.Tick(start + 54s, STALLED_LOADING).event == Event::kNone);
	CHECK(d.Tick(start + 55s, STALLED_LOADING).event == Event::kHang);  // 60 s
	CHECK(d.Tick(start + 56s, ANSWERS).event == Event::kRecovered);
}

TEST("hang watch: a paused game (the PC slept, a debugger) starts the stall over")
{
	auto       d = MakeDetector();
	const auto start = HangWatch::Clock::time_point{} + 1h;
	CHECK(d.Tick(start, STALLED).event == Event::kNone);         // 5 s
	CHECK(d.Tick(start + 8s, STALLED).event == Event::kNone);    // 13 s
	// The watch thread wakes up minutes later: the main thread had no time to answer yet.
	CHECK(d.Tick(start + 300s, Sample{ .responsive = false, .paused = true }).event == Event::kNone);
	CHECK(d.Tick(start + 314s, STALLED).event == Event::kNone);  // 14 s since the pause
	CHECK(d.Tick(start + 315s, STALLED).event == Event::kHang);

	// A pause in the middle of a reported hang doesn't report it again.
	CHECK(d.Tick(start + 900s, Sample{ .responsive = false, .paused = true }).event == Event::kNone);
	CHECK(d.Tick(start + 930s, STALLED).event == Event::kNone);
	CHECK(d.Tick(start + 931s, ANSWERS).event == Event::kRecovered);
}

TEST("hang watch: dump names sort by time, and only the newest three are kept")
{
	using HangWatch::DumpName;
	CHECK(DumpName(2026, 10, 11, 9, 5, 3) == "F4Multiplayer-hang-2026-10-11_09-05-03.dmp");
	CHECK(DumpName(2026, 10, 11, 9, 5, 3) < DumpName(2026, 10, 11, 10, 0, 0));
	CHECK(DumpName(2026, 9, 30, 23, 59, 59) < DumpName(2026, 10, 1, 0, 0, 0));

	const std::vector<std::string> files{
		"F4Multiplayer.log",
		DumpName(2026, 10, 11, 12, 0, 0),
		DumpName(2026, 10, 9, 8, 0, 0),
		"F4Multiplayer-hang-2026-10-10_08-00-00.dmp.tmp",  // the game was closed while it was written
		DumpName(2026, 10, 11, 9, 0, 0),
		"crash-2026-10-01-12-00-00.dmp",  // someone else's
		DumpName(2026, 10, 10, 8, 0, 0),
		"F4Multiplayer-hang-notes.txt",
	};
	auto remove = HangWatch::DumpsToRemove(files, 3);
	std::ranges::sort(remove);
	const std::vector<std::string> expected{
		DumpName(2026, 10, 9, 8, 0, 0),
		"F4Multiplayer-hang-2026-10-10_08-00-00.dmp.tmp",
	};
	CHECK(remove == expected);
	CHECK(HangWatch::DumpsToRemove(files, 10).size() == 1);  // only the half-written one
	CHECK(HangWatch::DumpsToRemove({}, 3).empty());
}
