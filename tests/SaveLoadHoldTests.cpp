#include "Test.h"

#include "game/SaveLoadHold.h"

#include <atomic>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using SaveLoadHold::Clock;
using SaveLoadHold::Hold;

namespace
{
	constexpr auto LIMIT = 60s;
	const auto     START = Clock::time_point{} + 1h;
}

TEST("save/load hold: frames work while nothing is under way")
{
	Hold hold{ LIMIT };
	CHECK(!hold.Held(START));
	{
		const SaveLoadHold::Frame frame{ hold, START };
		CHECK(static_cast<bool>(frame));
	}
	CHECK(hold.WaitForFrames(0ms));  // left again
}

TEST("save/load hold: no frame works from the start of a save or load to its end, and the held frames are counted")
{
	Hold       hold{ LIMIT };
	const auto number = hold.Begin(START);
	CHECK(hold.Held(START + 1s));
	for (int i = 0; i < 3; ++i) {
		const SaveLoadHold::Frame frame{ hold, START + 1s };
		CHECK(!frame);
	}
	const auto held = hold.End(number);
	REQUIRE(held.has_value());
	CHECK(*held == 3u);
	CHECK(!hold.Held(START + 2s));
	const SaveLoadHold::Frame frame{ hold, START + 2s };
	CHECK(static_cast<bool>(frame));
}

TEST("save/load hold: a hold whose end never came stops frames only for its limit")
{
	Hold hold{ LIMIT };
	hold.Begin(START);
	CHECK(hold.Held(START + LIMIT - 1s));
	CHECK(!hold.Held(START + LIMIT));
	const SaveLoadHold::Frame frame{ hold, START + LIMIT };
	CHECK(static_cast<bool>(frame));
}

TEST("save/load hold: an old load's end leaves a newer hold on")
{
	Hold       hold{ LIMIT };
	const auto first = hold.Begin(START);
	const auto second = hold.Begin(START + 1s);  // loaded again before the first load's own work ran
	CHECK(second != first);
	CHECK(hold.Newest() == second);
	CHECK(!hold.End(first).has_value());
	CHECK(hold.Held(START + 2s));
	CHECK(hold.End(second).has_value());
	CHECK(!hold.Held(START + 2s));
}

TEST("save/load hold: an end with no hold on (a new game) ends nothing")
{
	Hold hold{ LIMIT };
	CHECK(!hold.End(hold.Newest()).has_value());
	const auto number = hold.Begin(START);
	CHECK(hold.End(number).has_value());
	CHECK(!hold.End(number).has_value());  // twice
}

TEST("save/load hold: work handed over runs on the frame's side, held or not, in order")
{
	Hold hold{ LIMIT };
	hold.Begin(START);
	std::vector<int> ran;
	hold.Post([&] { ran.push_back(1); });
	hold.Post([&] { ran.push_back(2); });
	CHECK(!hold.WaitForFrames(0ms));  // not run yet
	CHECK(hold.RunPosted() == 2u);
	CHECK((ran == std::vector<int>{ 1, 2 }));
	CHECK(hold.RunPosted() == 0u);
	CHECK(hold.WaitForFrames(0ms));
}

TEST("save/load hold: the end of a hold handed over runs even after a piece that throws")
{
	Hold       hold{ LIMIT };
	const auto number = hold.Begin(START);
	hold.Post([] { throw std::runtime_error("teardown"); });
	hold.Post([&] { hold.End(number); });
	bool threw = false;
	try {
		hold.RunPosted();
	} catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);
	CHECK(!hold.Held(START));
	CHECK(hold.WaitForFrames(0ms));  // not left counted as working
}

// A save made on the main thread after a load's message but before the frame after it: the load's own
// work (the session starting over) runs before the save's (the stand-ins taken out), not after it.
TEST("save/load hold: a save before the frame after a load runs the load's work first")
{
	Hold             hold{ LIMIT };
	std::vector<int> ran;
	const auto       load = hold.Begin(START);
	hold.Post([&] {
		ran.push_back(1);
		hold.End(load);
	});
	const auto save = hold.Begin(START + 1s);
	bool       counted = false;
	hold.RunNow([&] {
		ran.push_back(2);
		counted = !hold.WaitForFrames(0ms);  // counted as working, as handed-over work is
	});
	CHECK((ran == std::vector<int>{ 1, 2 }));
	CHECK(counted);
	CHECK(hold.RunPosted() == 0u);  // not run twice
	CHECK(hold.Held(START + 1s));   // the load's end leaves the save's hold on
	CHECK(hold.End(save).has_value());
	CHECK(hold.WaitForFrames(0ms));
}

TEST("save/load hold: a save's work runs even after earlier work that throws")
{
	Hold hold{ LIMIT };
	bool ran = false;
	hold.Post([] { throw std::runtime_error("after the load"); });
	bool threw = false;
	try {
		hold.RunNow([&] { ran = true; });
	} catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(ran);
	CHECK(threw);
	CHECK(hold.WaitForFrames(0ms));  // not left counted as working
}

TEST("save/load hold: work taken back is not run by the frame's side")
{
	Hold hold{ LIMIT };
	int  ran = 0;
	hold.Post([&] { ++ran; });
	auto back = hold.TakeBack();
	REQUIRE(back.size() == 1u);
	CHECK(hold.RunPosted() == 0u);
	CHECK(ran == 0);
	CHECK(hold.WaitForFrames(0ms));
	back.front()();
	CHECK(ran == 1);
}

TEST("save/load hold: a load waits for the frame already working, and only for it")
{
	Hold              hold{ LIMIT };
	std::atomic<bool> entered{ false };
	std::atomic<bool> finish{ false };
	std::thread       frameThread([&] {
		const SaveLoadHold::Frame frame{ hold, START };
		entered = true;
		while (!finish) {
			std::this_thread::yield();
		}
	});
	while (!entered) {
		std::this_thread::yield();
	}
	hold.Begin(START);
	CHECK(!hold.WaitForFrames(20ms));  // the frame that entered before the load is still working
	finish = true;
	CHECK(hold.WaitForFrames(5s));
	frameThread.join();
}

TEST("save/load hold: the load's thread waits while the frame's thread runs what it handed over")
{
	Hold                         hold{ LIMIT };
	std::atomic<bool>            stop{ false };
	std::atomic<int>             ran{ 0 };
	std::atomic<std::thread::id> ranOn{};
	std::thread                  frameThread([&] {
		while (!stop) {
			hold.RunPosted();
			const SaveLoadHold::Frame frame{ hold, Clock::now() };
			std::this_thread::yield();
		}
	});
	hold.Begin(Clock::now());
	hold.Post([&] {
		++ran;
		ranOn = std::this_thread::get_id();
	});
	CHECK(hold.WaitForFrames(5s));
	CHECK(ran == 1);
	CHECK(ranOn.load() == frameThread.get_id());
	stop = true;
	frameThread.join();
}

// The race the hold is for: a frame that looked for a load just before it began must not still be
// working once the load's thread goes on.
TEST("save/load hold: no frame's work overlaps a load")
{
	Hold              hold{ LIMIT };
	std::atomic<bool> stop{ false };
	std::atomic<int>  working{ 0 };
	std::atomic<int>  frames{ 0 };
	std::thread       frameThread([&] {
		while (!stop) {
			hold.RunPosted();
			const SaveLoadHold::Frame frame{ hold, Clock::now() };
			if (frame) {
				++working;
				std::this_thread::yield();
				++frames;
				--working;
			}
		}
	});
	int overlaps = 0;
	int timeouts = 0;
	for (int i = 0; i < 500; ++i) {
		// Some frames between the loads, so that a load often begins with one working.
		const int  before = frames;
		const auto until = Clock::now() + 10ms;
		while (frames == before && Clock::now() < until) {
			std::this_thread::yield();
		}
		const auto number = hold.Begin(Clock::now());
		if (!hold.WaitForFrames(5s)) {
			++timeouts;
		}
		for (int j = 0; j < 20; ++j) {
			overlaps += working.load() != 0;
			std::this_thread::yield();
		}
		hold.End(number);
	}
	stop = true;
	frameThread.join();
	CHECK(timeouts == 0);
	CHECK(overlaps == 0);
	CHECK(frames > 0);
}
