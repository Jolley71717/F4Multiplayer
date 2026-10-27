// What a copy gains with no report from the game that runs it (CopyHealth::Book::Check): taken back, as a drop with no
// hit of ours is given back. A leveled NPC rolled legendary in one game and plain in the other kept its own roll on the
// copy until the copy's next load (Rolls::Apply writes the owner's roll for that load). Its legendary mutation healed
// the copy to full at half health, which was taken as where the copy stood and never sent: the player who hit it saw
// their hit heal it, while the NPC itself stayed at half in its own game.

#include "Test.h"
#include "game/CopyHealth.h"

#include <cmath>
#include <optional>

namespace
{
	using CopyHealth::Book;
	using std::chrono::milliseconds;

	constexpr std::uint32_t GHOUL = 0x0002A0F1;
	const CopyHealth::Clock::time_point T0{};

	bool Near(std::optional<float> a_health, float a_expected, float a_slack = 0.01f)
	{
		return a_health && std::abs(*a_health - a_expected) < a_slack;
	}
}

TEST("copy gains: a copy's heal of its own (a legendary's mutation) is taken back once it has stood, all of it")
{
	Book book;
	CHECK(book.Check(GHOUL, 100.0f, T0, 100.0f) == 0.0f);
	book.Set(GHOUL, 45.0f);  // the owner's report: 45 of 100
	CHECK(book.Check(GHOUL, 45.0f, T0 + milliseconds{ 50 }, 100.0f) == 0.0f);

	// The mutation: full again here. Not at once (as a drop, it is judged once it has stood).
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 100 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 150 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 100 } + CopyHealth::JUDGE_AFTER, 100.0f) == -55.0f);
	book.Set(GHOUL, 45.0f);
	CHECK(book.Check(GHOUL, 45.0f, T0 + milliseconds{ 300 }, 100.0f) == 0.0f);
	CHECK(book.TakeOurs(GHOUL) == 0.0f);  // nothing of it goes to the owner's game
}

TEST("copy gains: our hit's damage still goes to the owner's game when the copy's mutation heals it a frame later")
{
	Book book;
	CHECK(book.Check(GHOUL, 60.0f, T0, 100.0f) == 0.0f);
	book.OurHit(GHOUL, T0 + milliseconds{ 50 }, 20.0f);
	CHECK(book.Check(GHOUL, 40.0f, T0 + milliseconds{ 50 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 100 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 100 } + CopyHealth::JUDGE_AFTER, 100.0f) == -60.0f);
	CHECK(book.TakeOurs(GHOUL) == 20.0f);
}

TEST("copy gains: a report's raise stands, and a gain gone again before it was judged is nothing")
{
	Book book;
	CHECK(book.Check(GHOUL, 40.0f, T0, 100.0f) == 0.0f);
	// The owner's NPC mutated (or a stimpak): its report raises the copy, which then reads it.
	CHECK(book.Heard(GHOUL, 40.0f, 100.0f, T0 + milliseconds{ 50 }) == 100.0f);
	book.Set(GHOUL, 100.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 100 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 100.0f, T0 + milliseconds{ 500 }, 100.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 100.0f));

	book.Set(GHOUL, 50.0f);
	CHECK(book.Check(GHOUL, 70.0f, T0 + milliseconds{ 1000 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 50.0f, T0 + milliseconds{ 1050 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 50.0f, T0 + milliseconds{ 1000 } + CopyHealth::JUDGE_AFTER, 100.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 50.0f));
}

TEST("copy gains: one that creeps up a little at a time is taken back once it adds up to a hundredth of the maximum")
{
	Book book;
	CHECK(book.Check(GHOUL, 50.0f, T0, 200.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 51.0f, T0 + milliseconds{ 500 }, 200.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 51.0f, T0 + milliseconds{ 1000 }, 200.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 50.0f));  // not followed up
	CHECK(book.Check(GHOUL, 52.5f, T0 + milliseconds{ 1500 }, 200.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 52.5f, T0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER, 200.0f) == -2.5f);
}

TEST("copy gains: a new maximum (our level set to the owner's) keeps the share the copy stood at, and is no gain or drop")
{
	Book book;
	CHECK(book.Check(GHOUL, 50.0f, T0, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 75.0f, T0 + milliseconds{ 50 }, 150.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 75.0f, T0 + milliseconds{ 500 }, 150.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 75.0f));
	CHECK(book.Check(GHOUL, 30.0f, T0 + milliseconds{ 600 }, 60.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 30.0f, T0 + milliseconds{ 900 }, 60.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 30.0f));

	// The maximum a frame after the health: the gain waits to be judged, and by then it is the new maximum's share.
	CHECK(book.Check(GHOUL, 50.0f, T0 + milliseconds{ 1000 }, 60.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 50.0f, T0 + milliseconds{ 1050 }, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 50.0f, T0 + milliseconds{ 1000 } + CopyHealth::JUDGE_AFTER, 100.0f) == 0.0f);
	CHECK(Near(book.Health(GHOUL), 50.0f));
}

TEST("copy gains: a gain taken back never leaves the copy with less than MIN_LEFT")
{
	Book book;
	CHECK(book.Check(GHOUL, 0.5f, T0, 100.0f) == 0.0f);
	CHECK(book.Check(GHOUL, 40.0f, T0 + milliseconds{ 50 }, 100.0f) == 0.0f);
	const float give = book.Check(GHOUL, 40.0f, T0 + milliseconds{ 50 } + CopyHealth::JUDGE_AFTER, 100.0f);
	CHECK(std::abs(give - (CopyHealth::MIN_LEFT - 40.0f)) < 0.01f);
}

TEST("copy gains: without a maximum the gain is judged by rounding alone, and a report held after our hits still stands later")
{
	Book book;
	CHECK(book.Check(GHOUL, 80.0f, T0) == 0.0f);
	book.OurHit(GHOUL, T0 + milliseconds{ 50 }, 10.0f);
	CHECK(book.Check(GHOUL, 70.0f, T0 + milliseconds{ 50 }) == 0.0f);
	// The owner's report from before our hit reached it: held (Heard), and stands once our hits are over.
	CHECK(book.Heard(GHOUL, 70.0f, 80.0f, T0 + milliseconds{ 100 }) == 70.0f);
	CHECK(book.Holds(GHOUL));
	CHECK(book.Check(GHOUL, 70.0f, T0 + milliseconds{ 200 }) == 0.0f);
	CHECK(book.Check(GHOUL, 70.0f, T0 + milliseconds{ 50 } + CopyHealth::OURS_GRACE + milliseconds{ 50 }) == 10.0f);
	book.Set(GHOUL, 80.0f);
	CHECK(!book.Holds(GHOUL));
	CHECK(book.Check(GHOUL, 80.1f, T0 + milliseconds{ 2000 }) == 0.0f);
	CHECK(book.Check(GHOUL, 80.1f, T0 + milliseconds{ 2000 } + CopyHealth::JUDGE_AFTER) < 0.0f);
}
