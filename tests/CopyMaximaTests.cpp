// A copy whose health maximum is not its NPC's (CopyHealth::Maxima). Health and damage cross between games as shares of
// the sender's maximum; where one game's NPC has another maximum than the other game's copy of it (a console command or a
// mod's script set its health in one game only), a blow takes another amount of health off in each, and nothing said so.
// The game that runs an NPC now says its maximum with every health report; this finds the copies whose own differs, so
// the log names each once and the status line counts them.

#include "Test.h"
#include "game/CopyHealth.h"

#include <chrono>
#include <limits>

namespace
{
	using CopyHealth::Maxima;
	using std::chrono::milliseconds;
	using std::chrono::seconds;

	constexpr std::uint32_t RAIDER = 0x0002A0F1;
	constexpr std::uint32_t GHOUL = 0x0002A0F2;
	constexpr std::uint32_t RUNNER = 2;  // the player whose game runs them
	const CopyHealth::Clock::time_point T0{};
}

TEST("copy maxima: two maxima a point or a hundredth apart are the same; more is apart")
{
	CHECK(!CopyHealth::MaximaApart(185.0f, 185.0f));
	CHECK(!CopyHealth::MaximaApart(185.0f, 185.9f));    // under a point: rounding
	CHECK(!CopyHealth::MaximaApart(2000.0f, 2015.0f));  // under a hundredth of the larger
	CHECK(!CopyHealth::MaximaApart(2015.0f, 2000.0f));
	CHECK(CopyHealth::MaximaApart(185.0f, 370.0f));
	CHECK(CopyHealth::MaximaApart(370.0f, 185.0f));
	CHECK(CopyHealth::MaximaApart(50.0f, 52.0f));
	CHECK(CopyHealth::MaximaApart(2000.0f, 2030.0f));
}

TEST("copy maxima: nothing is said of a copy whose game said no maximum, nor of one whose maximum is its NPC's")
{
	Maxima maxima;
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 60 }));
	CHECK(maxima.Size() == 0);

	maxima.Heard(RAIDER, 185.0f, RUNNER);
	CHECK(maxima.Theirs(RAIDER) == 185.0f);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(!maxima.Seen(RAIDER, 185.4f, RUNNER, T0 + seconds{ 60 }));
	CHECK(maxima.Differing() == 0);
	CHECK(maxima.Said() == 0);
}

TEST("copy maxima: a report that says no maximum, or one that is no number, changes nothing")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 0.0f, RUNNER);
	maxima.Heard(RAIDER, -40.0f, RUNNER);
	maxima.Heard(RAIDER, std::numeric_limits<float>::quiet_NaN(), RUNNER);
	maxima.Heard(RAIDER, std::numeric_limits<float>::infinity(), RUNNER);
	CHECK(maxima.Size() == 0);
	maxima.Heard(RAIDER, 185.0f, RUNNER);
	maxima.Heard(RAIDER, 0.0f, RUNNER);
	CHECK(maxima.Theirs(RAIDER) == 185.0f);
}

TEST("copy maxima: a copy whose maximum stands apart from its NPC's for five seconds is said once, with both")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 4 }));
	CHECK(maxima.Differing() == 0);  // not yet: the copy's level may still be on its way

	const auto apart = maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 5 });
	REQUIRE(apart.has_value());
	CHECK(apart->ours == 185.0f);
	CHECK(apart->theirs == 370.0f);
	CHECK(maxima.Differing() == 1);
	CHECK(maxima.Said() == 1);

	// Every frame after, and every report of the same maximum: counted, not said again.
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 6 }));
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 600 }));
	CHECK(maxima.Differing() == 1);
	CHECK(maxima.Said() == 1);
}

TEST("copy maxima: a copy that takes its NPC's level a moment after it is seen is never said")
{
	// The shared rolls set the copy's level, and with it its maximum, shortly after the copy loads.
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 3 }));
	CHECK(!maxima.Seen(RAIDER, 370.0f, RUNNER, T0 + seconds{ 4 }));
	CHECK(!maxima.Seen(RAIDER, 370.0f, RUNNER, T0 + seconds{ 60 }));
	CHECK(maxima.Differing() == 0);
	CHECK(maxima.Said() == 0);
}

TEST("copy maxima: the wait starts over when either maximum moves, and a pair that is still apart is then said")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	// Our copy's maximum moves at 4 s (to a level that is still not the NPC's): five seconds from then.
	CHECK(!maxima.Seen(RAIDER, 240.0f, RUNNER, T0 + seconds{ 4 }));
	CHECK(!maxima.Seen(RAIDER, 240.0f, RUNNER, T0 + seconds{ 8 }));
	// The NPC's moves at 8.5 s: five seconds from the next look.
	maxima.Heard(RAIDER, 400.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 240.0f, RUNNER, T0 + milliseconds{ 8500 }));
	CHECK(!maxima.Seen(RAIDER, 240.0f, RUNNER, T0 + seconds{ 13 }));
	const auto apart = maxima.Seen(RAIDER, 240.0f, RUNNER, T0 + milliseconds{ 13500 });
	REQUIRE(apart.has_value());
	CHECK(apart->ours == 240.0f);
	CHECK(apart->theirs == 400.0f);
}

TEST("copy maxima: a pair that changes after it was said is said again; one that comes back to the same is not")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	REQUIRE(maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 5 }).has_value());

	// The copy's maximum becomes the NPC's (it was loaded again with the level), then drifts off to the same as before.
	CHECK(!maxima.Seen(RAIDER, 370.0f, RUNNER, T0 + seconds{ 10 }));
	CHECK(maxima.Differing() == 0);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 20 }));
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 30 }));  // apart again, the same pair: counted only
	CHECK(maxima.Differing() == 1);
	CHECK(maxima.Said() == 1);

	// The NPC's maximum is set to another in its game: a new pair.
	maxima.Heard(RAIDER, 740.0f, RUNNER);
	CHECK(maxima.Differing() == 0);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 31 }));
	const auto again = maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 36 });
	REQUIRE(again.has_value());
	CHECK(again->theirs == 740.0f);
	CHECK(maxima.Said() == 2);
}

TEST("copy maxima: a maximum said by a game that no longer runs the NPC is not held against the copy")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	// Player 3's game runs it now, and has not said its maximum yet.
	CHECK(!maxima.Seen(RAIDER, 185.0f, 3, T0 + seconds{ 5 }));
	CHECK(!maxima.Seen(RAIDER, 185.0f, 3, T0 + seconds{ 60 }));
	CHECK(maxima.Differing() == 0);
	// It says so: the wait is from then.
	maxima.Heard(RAIDER, 300.0f, 3);
	CHECK(!maxima.Seen(RAIDER, 185.0f, 3, T0 + seconds{ 61 }));
	const auto apart = maxima.Seen(RAIDER, 185.0f, 3, T0 + seconds{ 66 });
	REQUIRE(apart.has_value());
	CHECK(apart->theirs == 300.0f);
}

TEST("copy maxima: a maximum heard before we knew who runs the NPC counts for whoever does")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, 0);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 5 }).has_value());
}

TEST("copy maxima: a copy with no maximum of its own (not loaded far enough) is not compared")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 0.0f, RUNNER, T0));
	CHECK(!maxima.Seen(RAIDER, 0.0f, RUNNER, T0 + seconds{ 60 }));
	CHECK(!maxima.Seen(RAIDER, std::numeric_limits<float>::quiet_NaN(), RUNNER, T0 + seconds{ 61 }));
	CHECK(maxima.Differing() == 0);
	// Once it has one, the wait is from then.
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 62 }));
	CHECK(maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 67 }).has_value());
}

TEST("copy maxima: each NPC on its own; forgotten ones are gone, and a new session starts the count over")
{
	Maxima maxima;
	maxima.Heard(RAIDER, 370.0f, RUNNER);
	maxima.Heard(GHOUL, 90.0f, RUNNER);
	CHECK(!maxima.Seen(RAIDER, 185.0f, RUNNER, T0));
	CHECK(!maxima.Seen(GHOUL, 120.0f, RUNNER, T0 + seconds{ 2 }));
	CHECK(maxima.Seen(RAIDER, 185.0f, RUNNER, T0 + seconds{ 5 }).has_value());
	CHECK(!maxima.Seen(GHOUL, 120.0f, RUNNER, T0 + seconds{ 5 }));
	CHECK(maxima.Seen(GHOUL, 120.0f, RUNNER, T0 + seconds{ 7 }).has_value());
	CHECK(maxima.Differing() == 2);
	CHECK(maxima.Said() == 2);

	maxima.Forget(GHOUL);
	CHECK(maxima.Size() == 1);
	CHECK(maxima.Theirs(GHOUL) == 0.0f);
	CHECK(maxima.Differing() == 1);
	maxima.Prune([](std::uint32_t a_id) { return a_id != RAIDER; });  // no other game runs it any more
	CHECK(maxima.Size() == 0);
	CHECK(maxima.Said() == 2);

	maxima.Heard(RAIDER, 370.0f, RUNNER);
	maxima.Clear();
	CHECK(maxima.Size() == 0);
	CHECK(maxima.Said() == 0);
}

TEST("copy maxima: at most MAX_MAXIMA are kept; past it the one heard of longest ago goes")
{
	Maxima maxima;
	for (std::uint32_t i = 0; i < CopyHealth::MAX_MAXIMA; ++i) {
		maxima.Heard(0x1000 + i, 100.0f, RUNNER);
	}
	maxima.Heard(0x1000, 100.0f, RUNNER);  // heard again: no longer the oldest
	maxima.Heard(RAIDER, 185.0f, RUNNER);
	CHECK(maxima.Size() == CopyHealth::MAX_MAXIMA);
	CHECK(maxima.Theirs(RAIDER) == 185.0f);
	CHECK(maxima.Theirs(0x1000) == 100.0f);
	CHECK(maxima.Theirs(0x1001) == 0.0f);
}
