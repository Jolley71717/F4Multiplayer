#include "Test.h"

#include "game/FrameWatch.h"

#include <string_view>

using namespace std::chrono_literals;
using FrameWatch::Clock;
using FrameWatch::Limits;
using FrameWatch::Outcome;
using FrameWatch::Sample;
using FrameWatch::Stop;
using FrameWatch::Watch;
using Event = FrameWatch::Watch::Event;

namespace
{
	constexpr Limits LIMITS{ .lately = 3s, .stall = 15s, .longStall = 120s, .again = 60s };
	const auto       START = Clock::time_point{} + 1h;

	// The chain as it is in play: every link ran this moment, the last frame message ran a frame.
	Sample Healthy(Clock::time_point a_now, std::uint32_t a_drawn)
	{
		return { .now = a_now, .drawn = a_drawn, .quitting = false, .hooked = true, .task = a_now, .dispatched = a_now, .worked = a_now, .last = Outcome::kWorked };
	}

	// The game drawing on at 60 fps with the task posting, the window taking each message, and the last one ending as a_last
	// (not in game work), since a_since.
	Sample Stuck(Clock::time_point a_now, Clock::time_point a_since, Outcome a_last)
	{
		auto sample = Healthy(a_now, static_cast<std::uint32_t>((a_now - START) / 16ms));
		sample.worked = a_since;
		sample.last = a_last;
		return sample;
	}
}

TEST("frame watch: the chain is walked link by link, and the first one that has not run lately is the stop")
{
	using FrameWatch::Diagnose;
	auto sample = Healthy(START, 100);
	CHECK(Diagnose(sample, 3s) == Stop::kNone);
	sample.worked = START - 3s;  // just too long ago
	CHECK(Diagnose(sample, 3s) == Stop::kUnknown);  // the window ran a frame lately, yet nothing came of it
	sample.last = Outcome::kSkipped;
	CHECK(Diagnose(sample, 3s) == Stop::kInFrame);
	sample.last = Outcome::kDeferred;
	CHECK(Diagnose(sample, 3s) == Stop::kDeferred);
	sample.last = Outcome::kLoadingScreen;
	CHECK(Diagnose(sample, 3s) == Stop::kLoadingScreen);
	sample.last = Outcome::kHeld;
	CHECK(Diagnose(sample, 3s) == Stop::kHeld);
	sample.last = Outcome::kQuit;
	CHECK(Diagnose(sample, 3s) == Stop::kQuitting);
	sample.last = Outcome::kNone;
	CHECK(Diagnose(sample, 3s) == Stop::kUnknown);
	// Earlier links outrank the outcome.
	sample.dispatched = START - 10s;
	CHECK(Diagnose(sample, 3s) == Stop::kNotDispatched);
	sample.task = START - 10s;
	CHECK(Diagnose(sample, 3s) == Stop::kNoTask);
	sample.hooked = false;
	CHECK(Diagnose(sample, 3s) == Stop::kNoWindow);
	sample.quitting = true;
	CHECK(Diagnose(sample, 3s) == Stop::kQuitting);
	// Links never reached (the default times) count as not lately, without wrapping.
	CHECK(Diagnose(Sample{ .now = START, .hooked = true }, 3s) == Stop::kNoTask);
}

TEST("frame watch: with the task dead, the frames the watch's own posts run do not hide it")
{
	auto sample = Healthy(START, 100);
	sample.task = START - 5s;
	CHECK(FrameWatch::Diagnose(sample, 3s) == Stop::kNoTask);
	CHECK(FrameWatch::Rearmable(Stop::kNoTask));
	CHECK(FrameWatch::Rearmable(Stop::kNotDispatched));
	CHECK(!FrameWatch::Rearmable(Stop::kDeferred));
	CHECK(!FrameWatch::Rearmable(Stop::kInFrame));
}

TEST("frame watch: what is nothing to report, what holds frames by design, and the names")
{
	using FrameWatch::ByDesign;
	using FrameWatch::Reportable;
	for (const auto stop : { Stop::kNone, Stop::kIdle, Stop::kQuitting, Stop::kNoWindow }) {
		CHECK(!Reportable(stop));
	}
	for (const auto stop : { Stop::kNoTask, Stop::kNotDispatched, Stop::kInFrame, Stop::kDeferred, Stop::kLoadingScreen, Stop::kHeld, Stop::kUnknown }) {
		CHECK(Reportable(stop));
	}
	CHECK(ByDesign(Stop::kLoadingScreen));
	CHECK(ByDesign(Stop::kHeld));
	CHECK(!ByDesign(Stop::kDeferred));
	CHECK(std::string_view{ FrameWatch::Name(Stop::kDeferred) } == "deferred");
	CHECK(std::string_view{ FrameWatch::Name(Stop::kNotDispatched) } == "not dispatched");
	CHECK(std::string_view{ FrameWatch::Name(static_cast<Stop>(200)) } == "unknown");
}

TEST("frame watch: frames that run report nothing, tick after tick")
{
	Watch watch{ LIMITS };
	for (int i = 0; i < 100; ++i) {
		const auto now = START + i * 1s;
		const auto result = watch.Tick(Healthy(now, static_cast<std::uint32_t>(i * 60)));
		CHECK(result.event == Event::kNone);
		CHECK(result.stop == Stop::kNone);
		CHECK(!result.past);
	}
}

// The host's stall after a coc: the game drew on, every frame message was put off inside the engine's pump, and
// nothing said so for a quarter of an hour.
TEST("frame watch: a stop while the game draws is reported at the limit with its cause, again every so often, and its end")
{
	Watch      watch{ LIMITS };
	const auto lastWork = START;
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	std::vector<std::pair<int, Stop>> reports;
	Watch::Result                     last;
	for (int s = 1; s <= 200; ++s) {
		const auto now = START + s * 1s;
		last = watch.Tick(Stuck(now, lastWork, Outcome::kDeferred));
		if (last.event == Event::kStalled) {
			reports.emplace_back(s, last.stop);
		}
		CHECK(last.event != Event::kResumed);
		CHECK(last.stop == (s <= 2 ? Stop::kNone : Stop::kDeferred));  // the last work is lately for 3 s
		CHECK(last.past == (s >= 17));  // counted from the tick after the work stopped being lately (3 s), 15 s on
	}
	// Reported once the stop has lasted its limit, then every `again`.
	REQUIRE(reports.size() == 4u);
	CHECK(reports[0].first == 17);
	CHECK(reports[1].first == 77);
	CHECK(reports[2].first == 137);
	CHECK(reports[3].first == 197);
	for (const auto& [at, stop] : reports) {
		CHECK(stop == Stop::kDeferred);
	}
	CHECK(last.stalled == 198s);
	CHECK(last.drawn > 0);
	// Game work runs again: said once, with how long it was stopped.
	const auto resumed = watch.Tick(Healthy(START + 201s, 20000));
	CHECK(resumed.event == Event::kResumed);
	CHECK(resumed.stop == Stop::kNone);
	CHECK(resumed.stalled == 198s);
	CHECK(resumed.drawn > 0);
	CHECK(!resumed.past);
	CHECK(watch.Tick(Healthy(START + 202s, 20060)).event == Event::kNone);
	// A later stop is a new one, reported afresh.
	Watch::Result again;
	for (int s = 203; s <= 230; ++s) {
		again = watch.Tick(Stuck(START + s * 1s, START + 202s, Outcome::kSkipped));
		if (again.event == Event::kStalled) {
			break;
		}
	}
	CHECK(again.event == Event::kStalled);
	CHECK(again.stop == Stop::kInFrame);
	CHECK(again.stalled == 15s);
}

TEST("frame watch: a stop that ends before its limit is never reported, and resets the count")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	for (int s = 1; s <= 14; ++s) {
		const auto result = watch.Tick(Stuck(START + s * 1s, START, Outcome::kSkipped));
		CHECK(result.event == Event::kNone);
		CHECK(!result.past);
	}
	CHECK(watch.Tick(Healthy(START + 15s, 900)).event == Event::kNone);  // no kResumed: nothing was reported
	// The count starts over: another 14 s of it is still under the limit.
	for (int s = 16; s <= 29; ++s) {
		CHECK(watch.Tick(Stuck(START + s * 1s, START + 15s, Outcome::kSkipped)).event == Event::kNone);
	}
}

TEST("frame watch: a loading screen, a save or a load hold frames by design and get the long limit")
{
	for (const auto outcome : { Outcome::kLoadingScreen, Outcome::kHeld }) {
		Watch watch{ LIMITS };
		CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
		Watch::Result result;
		int           reportedAt = 0;
		for (int s = 1; s <= 130; ++s) {
			result = watch.Tick(Stuck(START + s * 1s, START, outcome));
			if (result.event == Event::kStalled && reportedAt == 0) {
				reportedAt = s;
			}
		}
		CHECK(reportedAt == 122);  // 120 s, counted from the tick after the work stopped being lately
		CHECK(result.stop == (outcome == Outcome::kLoadingScreen ? Stop::kLoadingScreen : Stop::kHeld));
	}
}

TEST("frame watch: a game that neither draws nor runs the task is idle, not stopped, and the count waits")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	// Paused in the background: the renderer's count stands still and the task is silent.
	for (int s = 1; s <= 100; ++s) {
		auto sample = Stuck(START + s * 1s, START - 10s, Outcome::kWorked);
		sample.drawn = 0;
		sample.task = START - 10s;
		const auto result = watch.Tick(sample);
		CHECK(result.event == Event::kNone);
		CHECK(result.stop == Stop::kIdle);
		CHECK(!result.past);
	}
	// Back, drawing again with the main thread inside the engine's pump: the stop counts from here.
	int reportedAt = 0;
	for (int s = 101; s <= 130; ++s) {
		const auto result = watch.Tick(Stuck(START + s * 1s, START, Outcome::kDeferred));
		if (result.event == Event::kStalled) {
			reportedAt = s;
			break;
		}
	}
	CHECK(reportedAt == 115);
}

TEST("frame watch: a paused game that still serves its connection is not idle; one that only runs the task is alive")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	// Paused with the task running and frames of ours running from its fallback: nothing to report.
	for (int s = 1; s <= 30; ++s) {
		auto sample = Healthy(START + s * 1s, 0);
		CHECK(watch.Tick(sample).stop == Stop::kNone);
	}
	// The task runs but the window never gets the message, with no frame drawn: counted, as the game is alive.
	Watch::Result result;
	for (int s = 31; s <= 60; ++s) {
		auto sample = Healthy(START + s * 1s, 0);
		sample.dispatched = START + 30s;
		sample.worked = START + 30s;
		result = watch.Tick(sample);
		if (result.event == Event::kStalled) {
			break;
		}
	}
	CHECK(result.event == Event::kStalled);
	CHECK(result.stop == Stop::kNotDispatched);
	CHECK(result.drawn == 0);
	CHECK(result.past);
}

TEST("frame watch: the game quitting, or no window to run on, is never reported and never re-armed")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	for (int s = 1; s <= 100; ++s) {
		auto sample = Stuck(START + s * 1s, START, Outcome::kQuit);
		sample.quitting = s % 2 == 0;
		sample.hooked = s % 2 != 0 ? false : true;
		const auto result = watch.Tick(sample);
		CHECK(result.event == Event::kNone);
		CHECK(result.stop == (s % 2 == 0 ? Stop::kQuitting : Stop::kNoWindow));
		CHECK(!result.past);
	}
}

TEST("frame watch: a stop that changes its cause is reported with the cause of the moment, and keeps its count")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	// The main thread inside a frame of ours for ten seconds, then inside the engine's pump.
	for (int s = 1; s <= 10; ++s) {
		CHECK(watch.Tick(Stuck(START + s * 1s, START, Outcome::kSkipped)).event == Event::kNone);
	}
	Watch::Result result;
	for (int s = 11; s <= 20; ++s) {
		result = watch.Tick(Stuck(START + s * 1s, START, Outcome::kDeferred));
		if (result.event == Event::kStalled) {
			break;
		}
	}
	CHECK(result.event == Event::kStalled);
	CHECK(result.stop == Stop::kDeferred);
	CHECK(result.stalled == 15s);
}

TEST("frame watch: once past its limit a re-armable stop asks for a re-arm every tick, not only when it is reported")
{
	Watch watch{ LIMITS };
	CHECK(watch.Tick(Healthy(START, 0)).event == Event::kNone);
	int rearms = 0;
	int reports = 0;
	for (int s = 1; s <= 100; ++s) {
		auto sample = Stuck(START + s * 1s, START - 10s, Outcome::kWorked);
		sample.task = START - 10s;  // the F4SE task died a while ago
		const auto result = watch.Tick(sample);
		CHECK(result.stop == Stop::kNoTask);
		rearms += result.past && FrameWatch::Rearmable(result.stop);
		reports += result.event == Event::kStalled;
	}
	CHECK(rearms == 100 - 14);  // from the 15th second of it on
	CHECK(reports == 2);
}
