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

TEST("rolling log: whether there is room for a line can be asked without taking it or counting one left out")
{
	RollingLog::Budget budget{ 2 };
	CHECK(budget.Open(T0));  // nothing asked yet
	CHECK(budget.Open(T0));
	CHECK(budget.Take(T0));
	CHECK(budget.Open(T0 + seconds{ 1 }));
	CHECK(budget.Take(T0 + seconds{ 1 }));
	CHECK(!budget.Open(T0 + seconds{ 2 }));
	CHECK(!budget.Open(T0 + minutes{ 9 }));
	CHECK(budget.LeftOut() == 0);  // asking leaves nothing out

	// The window over, there is room again, and the line that waited is taken.
	CHECK(budget.Open(T0 + RollingLog::WINDOW));
	CHECK(budget.Take(T0 + RollingLog::WINDOW));
	CHECK(budget.Take(T0 + RollingLog::WINDOW));
	CHECK(!budget.Open(T0 + RollingLog::WINDOW + seconds{ 1 }));
	CHECK(budget.LeftOut() == 0);
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

TEST("rolling log: a line that repeats for one subject is said once, then once a period with what went unsaid")
{
	// A hurt copy that regains 2.5 health a second, taken back each time: one line, not three hundred.
	RollingLog::Repeats repeats{ minutes{ 5 } };
	const auto first = repeats.Note(0x1234, 2.5f, T0);
	CHECK(first.say);
	CHECK(first.unsaid == 0);
	for (int i = 1; i < 300; ++i) {
		CHECK(!repeats.Note(0x1234, 2.5f, T0 + seconds{ i }).say);
	}
	const auto next = repeats.Note(0x1234, 2.5f, T0 + minutes{ 5 });
	CHECK(next.say);
	CHECK(next.unsaid == 299);
	CHECK(next.unsaidSum > 747.0f && next.unsaidSum < 748.0f);

	// The count starts over with the line said.
	CHECK(!repeats.Note(0x1234, 2.5f, T0 + minutes{ 5 } + seconds{ 1 }).say);
	const auto third = repeats.Note(0x1234, 2.5f, T0 + minutes{ 10 });
	CHECK(third.say);
	CHECK(third.unsaid == 1);
}

TEST("rolling log: before, every repeat was a line and one copy used up the window's budget for all of them")
{
	// As CopyWatch had it: 40 gain lines a window, asked for once a second by one regenerating copy.
	RollingLog::Budget budget{ 40 };
	for (int i = 0; i < 40; ++i) {
		CHECK(budget.Take(T0 + seconds{ i }));
	}
	// Another copy's first gain, a minute in: left out.
	CHECK(!budget.Take(T0 + seconds{ 60 }));

	// Now: the one copy takes one line, and the other's first is said.
	RollingLog::Budget   now{ 40 };
	RollingLog::Repeats  repeats;
	std::uint32_t        said = 0;
	for (int i = 0; i < 60; ++i) {
		if (repeats.Note(1, 2.5f, T0 + seconds{ i }).say && now.Take(T0 + seconds{ i })) {
			++said;
		}
	}
	CHECK(said == 1);
	CHECK(repeats.Note(2, 30.0f, T0 + seconds{ 60 }).say);
	CHECK(now.Take(T0 + seconds{ 60 }));
	CHECK(now.LeftOut() == 0);
}

TEST("rolling log: a repeat much bigger than the line last said is said at once, and each subject is its own")
{
	RollingLog::Repeats repeats{ minutes{ 5 } };
	CHECK(repeats.Note(1, 2.0f, T0).say);
	CHECK(!repeats.Note(1, 2.0f, T0 + seconds{ 1 }).say);
	CHECK(!repeats.Note(1, 4.0f, T0 + seconds{ 2 }).say);  // twice it: still the same thing

	// A legendary's mutation on a copy that also regenerates: a full heal, not a tick.
	const auto big = repeats.Note(1, 180.0f, T0 + seconds{ 3 });
	CHECK(big.say);
	CHECK(big.unsaid == 2);
	CHECK(big.unsaidSum == 6.0f);
	// The ticks after it are small beside it, and stay unsaid.
	CHECK(!repeats.Note(1, 2.0f, T0 + seconds{ 4 }).say);

	// Another copy's first line is said whatever the first one is doing.
	CHECK(repeats.Note(2, 2.0f, T0 + seconds{ 4 }).say);
	CHECK(!repeats.Note(2, 2.0f, T0 + seconds{ 5 }).say);
	CHECK(repeats.Size() == 2);
}

TEST("rolling log: a subject whose lines stopped is forgotten, and its next line is a first again")
{
	RollingLog::Repeats repeats{ minutes{ 5 } };
	CHECK(repeats.Note(1, 2.0f, T0).say);
	CHECK(!repeats.Note(1, 2.0f, T0 + minutes{ 4 }).say);
	CHECK(repeats.Note(2, 2.0f, T0 + minutes{ 4 }).say);

	// Still repeating within the period: kept.
	repeats.Prune(T0 + minutes{ 6 });
	CHECK(repeats.Size() == 2);

	// Nothing noted for a whole period: gone, with what it had unsaid.
	repeats.Prune(T0 + minutes{ 9 });
	CHECK(repeats.Size() == 0);
	const auto again = repeats.Note(1, 2.0f, T0 + minutes{ 9 });
	CHECK(again.say);
	CHECK(again.unsaid == 0);

	repeats.Clear();
	CHECK(repeats.Size() == 0);
	CHECK(repeats.Note(1, 2.0f, T0 + minutes{ 9 } + seconds{ 1 }).say);
}

TEST("rolling log: past the guard on subjects every line of a new one is said, as it was")
{
	RollingLog::Repeats repeats;
	for (std::uint32_t i = 0; i < RollingLog::Repeats::MAX_SUBJECTS; ++i) {
		CHECK(repeats.Note(i, 1.0f, T0).say);
	}
	CHECK(repeats.Size() == RollingLog::Repeats::MAX_SUBJECTS);
	CHECK(repeats.Note(900000, 1.0f, T0).say);
	CHECK(repeats.Note(900000, 1.0f, T0 + seconds{ 1 }).say);
	CHECK(repeats.Size() == RollingLog::Repeats::MAX_SUBJECTS);
	CHECK(!repeats.Note(5, 1.0f, T0 + seconds{ 1 }).say);
}

TEST("rolling log: the counters are picked out of a status line by whole keys, in the line's order")
{
	const auto line = "dead=3 deadline=9 pending=0  damage=12/10/2 revived=1 x= =5 healthShared=40/2 last";
	CHECK(RollingLog::Pick(line, { "damage", "dead", "healthShared" }) == "dead=3 damage=12/10/2 healthShared=40/2");
	CHECK(RollingLog::Pick(line, { "x" }) == "x=");
	CHECK(RollingLog::Pick(line, { "nothing" }).empty());
	CHECK(RollingLog::Pick("", { "dead" }).empty());
}
