// How many of a kind of line go to the log (RollingLog): so many a window, not a run, so that a fight six hours into
// a session still leaves its lines in the log the player sends; and the counters picked out for the periodic summary.

#include "Test.h"
#include "game/RollingLog.h"

namespace
{
	using std::chrono::minutes;
	using std::chrono::seconds;

	const RollingLog::Clock::time_point T0{};
}

TEST("rolling log: so many lines a window, the rest counted as left out, and a new window once the last one is over")
{
	RollingLog::Budget budget{ 3 };
	CHECK(budget.Take(T0));
	CHECK(budget.Take(T0 + seconds{ 1 }));
	CHECK(budget.Take(T0 + seconds{ 2 }));
	CHECK(!budget.Take(T0 + seconds{ 3 }));
	CHECK(!budget.Take(T0 + minutes{ 9 }));
	CHECK(budget.LeftOut() == 2);

	// Six hours on, the fight the player reports is in the log.
	const auto later = T0 + std::chrono::hours{ 6 };
	CHECK(budget.Take(later));
	CHECK(budget.Take(later + seconds{ 1 }));
	CHECK(budget.Take(later + seconds{ 2 }));
	CHECK(!budget.Take(later + seconds{ 3 }));
	CHECK(budget.LeftOut() == 3);

	// The window starts with the first line after the last one ended, not on the clock.
	CHECK(budget.Take(later + RollingLog::WINDOW));
	CHECK(budget.LeftOut() == 3);
}

TEST("rolling log: before, a cap for the run left nothing of a late fight")
{
	// As CopyWatch had it: 100 hits a run.
	RollingLog::Budget run{ 100, std::chrono::hours{ 1000 } };
	for (int i = 0; i < 100; ++i) {
		CHECK(run.Take(T0 + seconds{ i }));
	}
	CHECK(!run.Take(T0 + std::chrono::hours{ 6 }));
}

TEST("rolling log: the summary is due a window after the first ask, then a window after the last, and starts over")
{
	RollingLog::Every every;
	CHECK(!every.Due(T0));
	CHECK(!every.Due(T0 + minutes{ 9 }));
	CHECK(every.Due(T0 + minutes{ 10 }));
	CHECK(!every.Due(T0 + minutes{ 15 }));
	CHECK(every.Due(T0 + minutes{ 21 }));
	every.Reset();
	CHECK(!every.Due(T0 + minutes{ 40 }));
	CHECK(every.Due(T0 + minutes{ 50 }));
}

TEST("rolling log: the counters are picked out of a status line by whole keys, in the line's order")
{
	const auto line = "dead=3 deadline=9 pending=0  damage=12/10/2 revived=1 x= =5 healthShared=40/2 last";
	CHECK(RollingLog::Pick(line, { "damage", "dead", "healthShared" }) == "dead=3 damage=12/10/2 healthShared=40/2");
	CHECK(RollingLog::Pick(line, { "x" }) == "x=");
	CHECK(RollingLog::Pick(line, { "nothing" }).empty());
	CHECK(RollingLog::Pick("", { "dead" }).empty());
}
