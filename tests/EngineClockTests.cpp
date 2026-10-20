#include "Test.h"

#include "game/EngineClock.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using EngineClock::Clock;
	const auto start = Clock::time_point{} + 1h;

	double Ms(Clock::duration a_duration) { return std::chrono::duration<double, std::milli>(a_duration).count(); }

	// A game that skips frames unevenly, as the rig's VM does: frames 16, 31 or 46 ms long, each frame's actor update
	// running after 0 to 12 ms of other work. The engine moves the player by their speed times the frame's time.
	struct Run
	{
		struct Update
		{
			Clock::time_point wall;
			Clock::time_point at;
			double            x;
		};
		std::vector<Update> updates;
		double              worstOff = 0.0;  // ms the timeline was from the wall clock
	};

	Run Simulate(unsigned a_seed, double a_rate = 1.0)
	{
		std::mt19937                            random{ a_seed };
		std::uniform_int_distribution<int>      frames{ 1, 3 };
		std::uniform_real_distribution<double>  work{ 0.0, 12.0 };
		constexpr double                        SPEED = 0.4;  // units a millisecond: a run
		EngineClock::Timeline                   timeline;
		Run                                     run;
		double                                  frameStart = 0.0, x = 0.0;
		float                                   frameTime = 0.016f;
		for (int i = 0; i < 2000; ++i) {
			const double length = 15.5 * frames(random);
			frameStart += length;
			frameTime = static_cast<float>(length / 1000.0 * a_rate);  // the engine's measure of the frame
			x += SPEED * length * a_rate;
			const auto wall = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(frameStart + work(random)));
			const auto at = timeline.Advance(wall, frameTime);
			run.updates.push_back({ wall, at, x });
			if (i > 0) {
				run.worstOff = (std::max)(run.worstOff, std::abs(Ms(wall - at)));
			}
		}
		return run;
	}

	// The worst step, as how far it is off the run's speed (0.1 = 10%), with each update stamped by a_stamp.
	template <class F>
	double WorstStep(const Run& a_run, F a_stamp, double a_speed)
	{
		double worst = 0.0;
		for (std::size_t i = 1; i < a_run.updates.size(); ++i) {
			const double dt = Ms(a_stamp(a_run.updates[i]) - a_stamp(a_run.updates[i - 1]));
			if (dt > 0.0) {
				worst = (std::max)(worst, std::abs((a_run.updates[i].x - a_run.updates[i - 1].x) / dt / a_speed - 1.0));
			}
		}
		return worst;
	}
}

TEST("engine clock: on frames skipped unevenly, the player's even run steps evenly on its time, where on the moments its updates ran it stood and leapt")
{
	for (unsigned seed = 1; seed <= 3; ++seed) {
		const auto run = Simulate(seed);
		const double onTime = WorstStep(run, [](const Run::Update& a_update) { return a_update.at; }, 0.4);
		const double onWall = WorstStep(run, [](const Run::Update& a_update) { return a_update.wall; }, 0.4);
		CHECK(onTime < 0.03);
		CHECK(onWall > 0.3);
		CHECK(run.worstOff < 20.0);  // the timeline stays near the wall clock
	}
}

TEST("engine clock: an engine whose frame times run a percent slow is kept near the wall clock without a jump")
{
	const auto run = Simulate(4, 0.99);
	CHECK(run.worstOff < 40.0);
	CHECK(run.worstOff < Ms(EngineClock::SNAP));
	// Steps stay even: the pull adds well under a percent each frame.
	CHECK(WorstStep(run, [](const Run::Update& a_update) { return a_update.at; }, 0.4 * 0.99) < 0.05);
}

TEST("engine clock: the first update, a long stall and a game slowed far off the wall clock all set it to the wall clock")
{
	EngineClock::Timeline timeline;
	CHECK(timeline.Advance(start, 0.016f) == start);  // the first
	CHECK(std::abs(Ms(timeline.Advance(start + 16ms, 0.016f) - (start + 16ms))) < 0.01);
	// A 2 s stall the engine hands on as such, and one it clamps to 100 ms.
	CHECK(timeline.Advance(start + 2016ms, 2.0f) == start + 2016ms);
	CHECK(timeline.Advance(start + 4032ms, 0.1f) == start + 4032ms);
	// Slowed to a tenth (VATS): within a few frames it is more than SNAP behind, and set to the wall clock.
	auto wall = start + 4032ms;
	bool set = false;
	for (int i = 0; i < 10 && !set; ++i) {
		wall += 16ms;
		set = timeline.Advance(wall, 0.0016f) == wall;
	}
	CHECK(set);
	// A frame time that is not a number is not one.
	wall += 16ms;
	CHECK(timeline.Advance(wall, std::nanf("")) == wall);
}

TEST("engine clock: frames step when the timer's mark changes, once however often asked, and the frame before is the last one's time")
{
	EngineClock::Frames frames;
	auto wall = start;
	const auto first = frames.Now(wall, 1, 0.033f);
	CHECK(first == wall);
	CHECK(frames.Previous(wall, 1, 0.033f) == first);  // no frame before yet
	wall += 5ms;
	CHECK(frames.Now(wall, 1, 0.033f) == first);  // asked again in the same frame
	wall += 30ms;
	const auto second = frames.Now(wall, 2, 0.033f);
	CHECK(std::abs(Ms(second - first) - 33.0) < 1.0);
	CHECK(frames.Previous(wall + 3ms, 2, 0.033f) == first);
	wall += 31ms;
	const auto third = frames.Now(wall, 3, 0.031f);
	CHECK(frames.Previous(wall, 3, 0.031f) == second);
	CHECK(third > second);
}

TEST("engine clock: after a loading screen nobody asked through, the frame before is not the time before it")
{
	// Stamped with the time before a two-second load, our first state after it went out two seconds old: the friend's
	// game took their clock to have jumped, and drew them that much behind until it caught up.
	EngineClock::Frames frames;
	auto wall = start;
	for (std::uint64_t mark = 1; mark <= 30; ++mark) {
		wall += 33ms;
		(void)frames.Now(wall, mark, 0.033f);
	}
	wall += 2s;  // loading: the timer may go on marking, but nothing asks
	const auto previous = frames.Previous(wall, 31, 0.033f);
	CHECK(std::abs(Ms(wall - previous)) < 1.0);
	wall += 33ms;
	CHECK(std::abs(Ms(frames.Previous(wall, 32, 0.033f) - previous)) < 1.0);
}

TEST("engine clock: a timer that stops marking frames gives the wall clock, and starts again from it")
{
	EngineClock::Frames frames;
	auto wall = start;
	for (std::uint64_t mark = 1; mark <= 10; ++mark) {
		wall += 33ms;
		(void)frames.Now(wall, mark, 0.033f);
	}
	wall += EngineClock::STOPPED + 50ms;
	CHECK(frames.Now(wall, 10, 0.033f) == wall);
	CHECK(frames.Previous(wall, 10, 0.033f) == wall);
	wall += 33ms;
	CHECK(frames.Now(wall, 11, 0.033f) == wall);
}
