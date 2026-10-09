#include "Test.h"

#include "game/FrameStats.h"

using namespace std::chrono_literals;

TEST("frame stats: mean, worst and hitches over a window, reset by Take")
{
	FrameStats::Tracker t;
	auto now = std::chrono::steady_clock::time_point{} + 1s;
	t.Frame(now);  // only sets the clock
	for (int i = 0; i < 9; ++i) {
		now += 16ms;
		t.Frame(now);
	}
	now += 160ms;  // one hitch
	t.Frame(now);
	const auto w = t.Take();
	CHECK(w.frames == 10);
	CHECK(w.hitches == 1);
	CHECK(w.WorstMs() > 159.0f && w.WorstMs() < 161.0f);
	CHECK(w.MeanMs() > 30.0f && w.MeanMs() < 31.0f);  // (9*16 + 160) / 10 = 30.4
	CHECK(w.Describe().find("hitches=1") != std::string::npos);
	CHECK(t.Take().frames == 0);
}
