#include "Test.h"

#include "game/Perf.h"

using namespace std::chrono_literals;

TEST("perf: parts are named once, part 0 is the frame, the last part takes the overflow")
{
	Perf::Recorder r;
	const auto     a = r.Zone("A");
	CHECK(a == 1);
	CHECK(r.Zone("B") == 2);
	CHECK(r.Zone("A") == a);
	static std::array<std::string, Perf::MAX_ZONES> names;  // Zone keeps the pointers
	for (std::size_t i = 3; i < Perf::MAX_ZONES; ++i) {
		names[i] = std::format("part{}", i);
		CHECK(r.Zone(names[i].c_str()) == i);
	}
	CHECK(r.Zone("one too many") == Perf::MAX_ZONES - 1);
}

TEST("perf: median, 99th percentile and worst of each part")
{
	Perf::Recorder r;
	const auto     a = r.Zone("A");
	for (int i = 0; i < 100; ++i) {
		r.Add(a, 100us);
		r.Add(a, 100us);  // two scopes in one frame add up
		r.EndFrame(i == 99 ? 5ms : 1ms, 16ms);
	}
	const auto frame = r.Of(0);
	CHECK(frame.frames == 100);
	CHECK(frame.p50 == 1.0f);
	CHECK(frame.worst == 5.0f);
	const auto part = r.Of(a);
	CHECK(part.p50 == 0.2f);
	CHECK(part.worst == 0.2f);
	CHECK(r.Of(r.Zone("B")).worst == 0.0f);
}

TEST("perf: hitches count, and those where our work took half the frame")
{
	Perf::Recorder r;
	r.EndFrame(1ms, 0ms);     // the first frame has no interval
	r.EndFrame(1ms, 16ms);
	r.EndFrame(10ms, 150ms);  // a hitch, not ours
	r.EndFrame(90ms, 150ms);  // a hitch, ours
	const auto text = r.Describe();
	CHECK(text.find("frames=4") != std::string::npos);
	CHECK(text.find("hitches=2") != std::string::npos);
	CHECK(text.find("ours_in_hitches=1") != std::string::npos);
	CHECK(text.find("worst_ever=90.0ms") != std::string::npos);
}

TEST("perf: the ring keeps the last frames only, and Reset starts over")
{
	Perf::Recorder r;
	const auto     a = r.Zone("A");
	r.Add(a, 50ms);
	r.EndFrame(50ms, 16ms);
	for (std::size_t i = 0; i < Perf::FRAMES; ++i) {
		r.Add(a, 1ms);
		r.EndFrame(1ms, 16ms);
	}
	CHECK(r.Of(a).frames == Perf::FRAMES);
	CHECK(r.Of(a).worst == 1.0f);  // the 50 ms frame has left the ring
	CHECK(r.Describe().find("worst_ever=50.0ms") != std::string::npos);
	CHECK(r.Describe().find("| A 1.00/1.00/1.00") != std::string::npos);
	r.Reset();
	CHECK(r.Of(0).frames == 0);
	CHECK(r.Describe().find("| A") == std::string::npos);
	CHECK(r.Zone("A") == a);
}
