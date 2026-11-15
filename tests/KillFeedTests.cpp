// The kill feed's limits (src/game/KillFeed.h): a victim's kill is shown once though two games report it, and a big
// fight shows no more than a few lines at a time.

#include "Test.h"

#include "game/KillFeed.h"

#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;

namespace
{
	using KillFeed::Clock;

	constexpr std::uint32_t GUARD = 0x00095E8A;
	constexpr std::uint32_t RAIDER = 0x0001F301;
}

TEST("kill feed: a kill shown for our own report of a friend's blow is not shown again when that friend's report crosses it")
{
	KillFeed::Book feed;
	const auto     t0 = Clock::now();

	// The game that runs the guard shows the line as it reports the death; the friend's own report of the kill arrives
	// a moment later, and again at the end of a robot's ten-second fall.
	CHECK(feed.Show(GUARD, t0));
	CHECK(!feed.Show(GUARD, t0 + 200ms));

	// Another victim is another kill.
	CHECK(feed.Show(RAIDER, t0 + 1s));

	CHECK(!feed.Show(GUARD, t0 + KillFeed::SHOWN_ONCE));
	// Later than that it is a new death under the id (a spawn's id may name another spawn).
	CHECK(feed.Show(GUARD, t0 + KillFeed::SHOWN_ONCE + 1ms));
}

TEST("kill feed: no more than three lines in five seconds, and a kill the window had no room for is not shown late")
{
	KillFeed::Book feed;
	const auto     t0 = Clock::now();

	CHECK(feed.Show(1, t0));
	CHECK(feed.Show(2, t0 + 1s));
	CHECK(feed.Show(3, t0 + 2s));
	CHECK(!feed.Show(4, t0 + 3s));
	CHECK(!feed.Show(5, t0 + 4900ms));

	// The window began with the first line: five seconds on there is room again.
	CHECK(feed.Show(6, t0 + KillFeed::WINDOW));
	CHECK(feed.Show(7, t0 + KillFeed::WINDOW));
	CHECK(feed.Show(8, t0 + KillFeed::WINDOW));
	CHECK(!feed.Show(9, t0 + KillFeed::WINDOW));

	// The fourth kill's second report, with room in the window again: still no line, it counted as shown.
	CHECK(!feed.Show(4, t0 + 2 * KillFeed::WINDOW));
}
