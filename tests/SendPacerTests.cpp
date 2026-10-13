#include "Test.h"
#include "game/SendPacer.h"

#include <algorithm>
#include <random>

using namespace std::chrono_literals;
using SendPacer::Clock;

namespace
{
	std::vector<int> Numbers(int a_from, int a_count)
	{
		std::vector<int> out(static_cast<std::size_t>(a_count));
		for (int i = 0; i < a_count; ++i) {
			out[static_cast<std::size_t>(i)] = a_from + i;
		}
		return out;
	}
}

TEST("send pacer: a burst goes up to the limit at once, and the rest waits its turn, in order")
{
	SendPacer::Queue<int> line{ 200, 8192 };
	for (int i = 0; i < 1500; ++i) {
		REQUIRE(line.Push(i));
	}
	const auto t0 = Clock::now();
	CHECK(line.TakeDue(t0) == Numbers(0, 200));
	CHECK(line.TakeDue(t0 + 500ms).empty());
	CHECK(line.TakeDue(t0 + 999ms).empty());
	CHECK(line.TakeDue(t0 + 1s) == Numbers(200, 200));
	CHECK(line.Size() == 1100);
}

TEST("send pacer: in no second, wherever it starts, do more than the limit go, and everything goes in the end")
{
	SendPacer::Queue<int> line{ 200, 8192 };
	for (int i = 0; i < 1500; ++i) {
		REQUIRE(line.Push(i));
	}
	// Taken every frame, frames of uneven length (a hitch now and then), as the game does.
	std::mt19937                   dice{ 7 };
	std::vector<Clock::time_point> sent;
	std::vector<int>               order;
	auto                           now = Clock::now();
	for (int frame = 0; frame < 100000 && !line.Empty(); ++frame) {
		for (const auto item : line.TakeDue(now)) {
			sent.push_back(now);
			order.push_back(item);
		}
		now += std::chrono::milliseconds(std::uniform_int_distribution<int>(1, 120)(dice));
	}
	CHECK(order == Numbers(0, 1500));
	for (std::size_t i = 0; i < sent.size(); ++i) {
		const auto inSecond = std::count_if(sent.begin() + static_cast<std::ptrdiff_t>(i), sent.end(), [&](Clock::time_point a_t) { return a_t - sent[i] < 1s; });
		if (inSecond > 200) {
			Test::Fail(__FILE__, __LINE__, std::format("{} sent within a second of send #{}", inSecond, i));
			break;
		}
	}
	// Not much slower than the limit allows, either: 1500 at 200 a second is seven and a half seconds.
	CHECK(sent.back() - sent.front() < 9s);
}

TEST("send pacer: after a quiet second a new burst goes at once")
{
	SendPacer::Queue<int> line{ 200, 8192 };
	const auto            t0 = Clock::now();
	for (int i = 0; i < 300; ++i) {
		line.Push(i);
	}
	CHECK(line.TakeDue(t0).size() == 200);
	CHECK(line.TakeDue(t0 + 1s).size() == 100);
	for (int i = 0; i < 150; ++i) {
		line.Push(i);
	}
	CHECK(line.TakeDue(t0 + 1500ms).size() == 100);  // the 100 sent at 1 s still count
	CHECK(line.TakeDue(t0 + 2s).size() == 50);
	for (int i = 0; i < 50; ++i) {
		line.Push(i);
	}
	CHECK(line.TakeDue(t0 + 3500ms).size() == 50);
}

TEST("send pacer: past its bound the line drops what comes, and out of a session it all goes at once, unsent and uncounted")
{
	SendPacer::Queue<int> line{ 200, 10 };
	for (int i = 0; i < 10; ++i) {
		CHECK(line.Push(i));
	}
	CHECK(!line.Push(10));
	CHECK(line.Size() == 10);
	CHECK(line.TakeAll() == Numbers(0, 10));
	CHECK(line.Empty());
	// Nothing was sent: the whole limit is still there.
	for (int i = 0; i < 10; ++i) {
		line.Push(i);
	}
	CHECK(line.TakeDue(Clock::now()).size() == 10);
}

TEST("send pacer: leaving the session clears the line, but what just went still counts")
{
	SendPacer::Queue<int> line{ 200, 8192 };
	const auto            t0 = Clock::now();
	for (int i = 0; i < 300; ++i) {
		line.Push(i);
	}
	CHECK(line.TakeDue(t0).size() == 200);
	line.Clear();
	CHECK(line.Empty());  // the hundred left waiting went with it
	line.Push(1);
	CHECK(line.TakeDue(t0 + 100ms).empty());
	CHECK(line.TakeDue(t0 + 1s) == std::vector<int>{ 1 });
}

// The workshop's line leaves most of the server's 500 events a second to everything else, and holds the
// biggest scrap-all.
static_assert(SendPacer::WORKSHOP_PER_SECOND * 2 <= 500);
static_assert(SendPacer::WORKSHOP_QUEUED >= 4000);
