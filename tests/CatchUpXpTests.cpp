// Catch-up XP (common/CatchUpXp.h): the factors a player's level among the group's gives their own XP and their share
// of friends' kill XP.

#include "Test.h"

#include "CatchUpXp.h"

#include <array>
#include <cstdint>
#include <vector>

using CatchUpXp::Factors;
using CatchUpXp::Of;

namespace
{
	bool Near(float a_value, float a_expected)
	{
		return a_value > a_expected - 0.001f && a_value < a_expected + 0.001f;
	}
}

TEST("catch-up xp: off, alone, or with nobody heard from, nothing changes")
{
	const std::array<std::uint16_t, 2> two{ 10, 30 };
	CHECK(Of(0, 10, two) == Factors{});
	const std::array<std::uint16_t, 1> one{ 10 };
	CHECK((Of(10, 10, one) == Factors{ .ownGain = 1.0f, .share = 1.0f, .average = 10, .behind = 0 }));
	const std::array<std::uint16_t, 2> unheard{ 0, 0 };
	CHECK(Of(10, 10, unheard) == Factors{});
	// Our own level not known yet: nothing either.
	CHECK(Of(10, 0, two) == Factors{});
}

TEST("catch-up xp: the average is the mean of the levels heard, rounded to the nearest, a half up")
{
	const std::array<std::uint16_t, 2> pair{ 10, 11 };
	CHECK(CatchUpXp::Average(pair) == 11);
	const std::array<std::uint16_t, 3> three{ 10, 20, 0 };
	CHECK(CatchUpXp::Average(three) == 15);
	const std::array<std::uint16_t, 3> odd{ 10, 20, 31 };
	CHECK(CatchUpXp::Average(odd) == 20);
	CHECK(!CatchUpXp::Average(std::array<std::uint16_t, 0>{}));
}

TEST("catch-up xp: behind the average, own gains grow by the percent per level and shares stay whole")
{
	const std::array<std::uint16_t, 2> levels{ 10, 30 };  // average 20
	const auto                         behind = Of(10, 10, levels);
	CHECK(behind.average == 20);
	CHECK(behind.behind == 10);
	CHECK(Near(behind.ownGain, 2.0f));  // 10 levels at 10% = +100%, the cap
	CHECK(Near(behind.share, 1.0f));
	const auto one = Of(10, 19, levels);
	CHECK(one.behind == 1);
	CHECK(Near(one.ownGain, 1.1f));
	CHECK(Near(one.share, 1.0f));
	const auto five = Of(5, 10, levels);
	CHECK(Near(five.ownGain, 1.5f));
}

TEST("catch-up xp: a gain never more than doubles, however far behind")
{
	const std::array<std::uint16_t, 2> levels{ 1, 99 };  // average 50
	const auto                         far = Of(10, 1, levels);
	CHECK(far.behind == 49);
	CHECK(Near(far.ownGain, 2.0f));
	CHECK(Near(Of(100, 1, levels).ownGain, 2.0f));
	CHECK(Near(Of(255, 49, levels).ownGain, 2.0f));
}

TEST("catch-up xp: ahead of the average, own gains stay as the game gives them and shares shrink, to nothing at most")
{
	const std::array<std::uint16_t, 2> levels{ 10, 30 };  // average 20
	const auto                         ahead = Of(10, 30, levels);
	CHECK(ahead.behind == -10);
	CHECK(Near(ahead.ownGain, 1.0f));
	CHECK(Near(ahead.share, 0.0f));
	const auto five = Of(10, 25, levels);
	CHECK(Near(five.ownGain, 1.0f));
	CHECK(Near(five.share, 0.5f));
	const std::array<std::uint16_t, 2> wide{ 10, 90 };
	const auto                         farAhead = Of(10, 90, wide);
	CHECK(Near(farAhead.share, 0.0f));
	CHECK(farAhead.share >= 0.0f);
}

TEST("catch-up xp: at the average nothing changes, and the gap is read from the player's own level among the group's")
{
	const std::array<std::uint16_t, 3> levels{ 10, 20, 30 };  // average 20
	const auto                         at = Of(10, 20, levels);
	CHECK(at.behind == 0);
	CHECK(Near(at.ownGain, 1.0f));
	CHECK(Near(at.share, 1.0f));
	CHECK(CatchUpXp::Percent(at) == 0);
	CHECK(CatchUpXp::Percent(Of(10, 12, levels)) == 80);
	CHECK(CatchUpXp::Percent(Of(10, 25, levels)) == -50);
}

TEST("catch-up xp: a late joiner far below gets the cap, and the factors follow the levels as they change")
{
	std::vector<std::uint16_t> levels{ 28, 30, 31 };  // average 30
	levels.push_back(8);                               // average 24
	const auto joined = Of(10, 8, levels);
	CHECK(joined.average == 24);
	CHECK(Near(joined.ownGain, 2.0f));
	// The others' shares of the joiner's kills shrink: 4 to 7 levels ahead at 10% is 40 to 70% off.
	CHECK(Near(Of(10, 28, levels).share, 0.6f));
	CHECK(Near(Of(10, 31, levels).share, 0.3f));
	levels.back() = 20;  // the joiner has caught up some: average 27
	const auto later = Of(10, 20, levels);
	CHECK(later.average == 27);
	CHECK(Near(later.ownGain, 1.7f));
}

TEST("catch-up xp: the bonus for a gain is whole points of the extra, and nothing for a factor of 1")
{
	Factors factors;
	CHECK(CatchUpXp::Bonus(factors, 50.0f) == 0);
	factors.ownGain = 1.5f;
	CHECK(CatchUpXp::Bonus(factors, 50.0f) == 25);
	CHECK(CatchUpXp::Bonus(factors, 1.0f) == 1);  // 0.5 rounds up
	CHECK(CatchUpXp::Bonus(factors, 0.0f) == 0);
	CHECK(CatchUpXp::Bonus(factors, -5.0f) == 0);
	factors.ownGain = 2.0f;
	CHECK(CatchUpXp::Bonus(factors, 120.0f) == 120);
}

TEST("catch-up xp: a symmetric gap gives a symmetric cut, so two players' factors mirror each other")
{
	for (std::uint16_t gap = 0; gap <= 15; ++gap) {
		const std::uint16_t                low = 20;
		const std::uint16_t                high = static_cast<std::uint16_t>(20 + 2 * gap);
		const std::array<std::uint16_t, 2> levels{ low, high };  // average 20 + gap
		const auto                         below = Of(10, low, levels);
		const auto                         above = Of(10, high, levels);
		CHECK(below.behind == -above.behind);
		CHECK(Near(below.ownGain - 1.0f, 1.0f - above.share));
		CHECK(Near(below.share, 1.0f));
		CHECK(Near(above.ownGain, 1.0f));
	}
}
