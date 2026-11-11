// A copy's health when our game's hit events don't reach the mod, and when the copy heals itself in steps
// (CopyHealth::HitEvents, CopyHealth::AllAimed, CopyHealth::Book). A player's game sent no hit event for its player's
// shots (another of its plugins may stop them first): the aim saved only the shots that hit the NPC the view pointed at,
// and a shotgun's spread, a blast's splash, or any shot in the ten seconds after a friend's grenade touched the NPC was
// given back, so the player's shots healed the enemies a friend's game ran. In the same log the host's Dogmeat healed
// itself here by +30 every 0.7 s, each step taken back as it should be, but a step in the tenth of a second after an
// unseen shot took the shot back with it. Each test names what the players would see go wrong if it failed.

#include "Test.h"
#include "game/CopyHealth.h"

#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace
{
	using CopyHealth::Book;
	using CopyHealth::HitEvents;
	using std::chrono::milliseconds;

	constexpr std::uint32_t RAIDER = 0x0007BD0E;
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	const CopyHealth::Clock::time_point T0{};

	float Took(Book& a_book, std::uint32_t a_id, CopyHealth::Clock::time_point a_now = T0 + milliseconds{ 9000 })
	{
		const auto sent = a_book.TakeOurs(a_id, a_now);
		return sent ? sent->amount : 0.0f;
	}

	bool Near(float a_value, float a_expected, float a_slack = 0.01f)
	{
		return std::abs(a_value - a_expected) < a_slack;
	}

	bool Near(std::optional<float> a_value, float a_expected)
	{
		return a_value && Near(*a_value, a_expected);
	}

	// A copy at a_health of a_maximum, seen once.
	Book Seen(float a_health = 30.0f, float a_maximum = 30.0f, std::uint32_t a_id = RAIDER)
	{
		Book book;
		CHECK(book.Check(a_id, a_health, T0, a_maximum) == 0.0f);
		return book;
	}

	constexpr float DEG = std::numbers::pi_v<float> / 180.0f;

	// A player at the origin, eyes 120 up, looking a_yaw degrees from +y toward +x and a_pitch degrees down.
	CopyHealth::Aim Looking(float a_yaw, float a_pitch = 0.0f)
	{
		return { .x = 0.0f, .y = 0.0f, .z = 120.0f, .yaw = a_yaw * DEG, .pitch = a_pitch * DEG };
	}

	milliseconds Ms(int a_ms)
	{
		return milliseconds{ a_ms };
	}
}

TEST("hit events: judged missing after three unseen drops in a row with none of our player's hit events, and back with the first that comes")
{
	HitEvents events;
	CHECK(!events.Missing());
	CHECK(!events.Unseen());
	CHECK(!events.Unseen());
	// One of our player's hit events between: they do reach the mod, the count starts over.
	CHECK(!events.Seen());
	CHECK(!events.Unseen());
	CHECK(!events.Unseen());
	CHECK(!events.Missing());
	// The third in a row: missing, told once (one warning in the log).
	CHECK(events.Unseen());
	CHECK(events.Missing());
	CHECK(events.Spells() == 1);
	CHECK(!events.Unseen());
	CHECK(events.Missing());
	// A hit event of our player's comes: back to the events, told once.
	CHECK(events.Seen());
	CHECK(!events.Missing());
	CHECK(!events.Seen());
	// Missing again later: a second spell.
	CHECK(!events.Unseen());
	CHECK(!events.Unseen());
	CHECK(events.Unseen());
	CHECK(events.Spells() == 2);
}

TEST("hit events: with them missing, a shot books every copy in its cone (a shotgun's pellets on the raider beside the one aimed at were healed back)")
{
	using CopyHealth::AllAimed;
	using CopyHealth::NearestAimed;
	const auto aim = Looking(0.0f, 5.0f);
	// One straight ahead, one 15 degrees to the side at the same range, one behind the player, one past the reach.
	const std::vector<std::array<float, 3>> at{ { 0.0f, 400.0f, 0.0f }, { 110.0f, 400.0f, 0.0f }, { 0.0f, -300.0f, 0.0f }, { 0.0f, 9000.0f, 0.0f } };
	CHECK((AllAimed(aim, at) == std::vector<std::size_t>{ 0, 1 }));
	// With hit events, still the one aimed at alone: its neighbour's hits come as events.
	const auto nearest = NearestAimed(aim, at);
	CHECK(nearest && *nearest == 0);
	CHECK(!CopyHealth::AimedAt(aim, 110.0f, 400.0f, 0.0f));
	// Looking away: nobody.
	CHECK(AllAimed(Looking(90.0f, 5.0f), at).empty());
}

TEST("hit events: with them missing, a friend's grenade three seconds before no longer makes our shot heal the raider")
{
	// Missing: the grenade's hit (set off here for the friend, naming no actor) came, our shot's did not.
	auto book = Seen();
	book.EventsMissing(true);
	book.OtherHit(RAIDER, T0 + Ms(1000));
	book.OurShot(RAIDER, T0 + Ms(4000));
	CHECK(book.Check(RAIDER, 25.0f, T0 + Ms(4050), 30.0f) == 0.0f);
	CHECK(book.Check(RAIDER, 25.0f, T0 + Ms(4050) + CopyHealth::JUDGE_AFTER, 30.0f) == 0.0f);
	CHECK(Near(Took(book, RAIDER), 5.0f));
	CHECK(book.Unseen() == 1);

	// With the events reaching the mod, as before: a burn the other hit left may still tick, and our shot's own event
	// would have come. Given back.
	auto events = Seen();
	events.OtherHit(RAIDER, T0 + Ms(1000));
	events.OurShot(RAIDER, T0 + Ms(4000));
	CHECK(events.Check(RAIDER, 25.0f, T0 + Ms(4050), 30.0f) == 0.0f);
	CHECK(Near(events.Check(RAIDER, 25.0f, T0 + Ms(4050) + CopyHealth::JUDGE_AFTER, 30.0f), 5.0f));
	CHECK(Took(events, RAIDER) == 0.0f);

	// Missing, but another hit in the moment of the drop: that hit's, given back (a friend's blast is no shot of ours).
	auto close = Seen();
	close.EventsMissing(true);
	close.OurShot(RAIDER, T0 + Ms(4000));
	close.OtherHit(RAIDER, T0 + Ms(4040));
	CHECK(close.Check(RAIDER, 25.0f, T0 + Ms(4050), 30.0f) == 0.0f);
	CHECK(Near(close.Check(RAIDER, 25.0f, T0 + Ms(4050) + CopyHealth::JUDGE_AFTER, 30.0f), 5.0f));
	CHECK(Took(close, RAIDER) == 0.0f);

	// Missing, and a friend's explosion set off here reached the copy (CopyWatch::FriendsBlast): the owner's blow, given back.
	auto blast = Seen();
	blast.EventsMissing(true);
	blast.OurShot(RAIDER, T0 + Ms(4000));
	blast.OwnersBlow(RAIDER, T0 + Ms(4030));
	CHECK(blast.Check(RAIDER, 25.0f, T0 + Ms(4050), 30.0f) == 0.0f);
	CHECK(Near(blast.Check(RAIDER, 25.0f, T0 + Ms(4050) + CopyHealth::JUDGE_AFTER, 30.0f), 5.0f));
	CHECK(Took(blast, RAIDER) == 0.0f);
}

TEST("copy steps: a copy's own heal every 0.7 s is taken back each time, and nothing of it reaches the owner's game (the host's Dogmeat)")
{
	auto book = Seen(169.7f, 210.0f, DOGMEAT);
	for (int step = 0; step < 5; ++step) {
		const auto at = T0 + Ms(1000 + step * 734);
		CHECK(book.Check(DOGMEAT, 199.4f, at, 210.0f) == 0.0f);
		CHECK(Near(book.Check(DOGMEAT, 199.4f, at + CopyHealth::JUDGE_AFTER, 210.0f), -29.7f));
		book.Set(DOGMEAT, 169.7f);  // CopyWatch takes it back and books where it reads
		CHECK(Near(book.Health(DOGMEAT), 169.7f));
	}
	CHECK(Took(book, DOGMEAT) == 0.0f);
	CHECK(book.Unseen() == 0);
}

TEST("copy steps: our unseen shot's drop still goes to the owner's game when the copy's own heal steps in before it is judged")
{
	auto book = Seen(130.0f, 210.0f);
	book.OurShot(RAIDER, T0 + Ms(1000));
	CHECK(book.Check(RAIDER, 107.8f, T0 + Ms(1050), 210.0f) == 0.0f);  // the shot lands; no hit event comes
	CHECK(book.Check(RAIDER, 137.8f, T0 + Ms(1100), 210.0f) == 0.0f);  // a step of its own heal, 50 ms later
	// Judged: the shot's 22.2 is ours and goes; the step's 30 is taken back, off where the shot left it.
	CHECK(Near(book.Check(RAIDER, 137.8f, T0 + Ms(1100) + CopyHealth::JUDGE_AFTER, 210.0f), -30.0f));
	CHECK(Near(Took(book, RAIDER), 22.2f));
	CHECK(Near(book.Health(RAIDER), 107.8f));
	CHECK(book.Unseen() == 1);

	// With no shot of ours, the drop and the step both go back: the copy stands where its owner had it.
	auto none = Seen(130.0f, 210.0f);
	CHECK(none.Check(RAIDER, 107.8f, T0 + Ms(1050), 210.0f) == 0.0f);
	CHECK(none.Check(RAIDER, 137.8f, T0 + Ms(1100), 210.0f) == 0.0f);
	CHECK(Near(none.Check(RAIDER, 137.8f, T0 + Ms(1100) + CopyHealth::JUDGE_AFTER, 210.0f), -7.8f));
	CHECK(Took(none, RAIDER) == 0.0f);
	CHECK(none.Unseen() == 0);
}

TEST("copy steps: a hit event a frame behind its drop, with a friend's blast around, still counts its figure times the group multiplier when a step comes between")
{
	auto book = Seen();
	book.OwnersBlow(RAIDER, T0 + Ms(1000));  // a friend's grenade set off here reached it
	CHECK(book.Check(RAIDER, 10.0f, T0 + Ms(1050), 30.0f) == 0.0f);  // 20 gone: blast and our shot
	// Our shot's event, a frame behind: 16 before the engine's multiplier, halved by group difficulty.
	book.OurHit(RAIDER, T0 + Ms(1060), CopyHealth::FigureDealt(16.0f, true, 0.5f));
	CHECK(book.Check(RAIDER, 35.0f, T0 + Ms(1100), 30.0f) == 0.0f);  // a step of its own heal
	// Ours: the 8 our hit took; the blast's 12 is the owner's, and the step goes too.
	CHECK(Near(book.Check(RAIDER, 35.0f, T0 + Ms(1100) + CopyHealth::JUDGE_AFTER, 30.0f), -13.0f));
	CHECK(Near(Took(book, RAIDER), 8.0f));
}

TEST("unseen hits: a killing shot with no hit event goes to the NPC's game, so it dies there too (it lay dead here and stood on there)")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + Ms(1000));
	CHECK(!book.Fell(RAIDER, 0.0f, T0 + Ms(1050)));  // judged at once: the shot's
	CHECK(book.FellToUs(RAIDER));
	CHECK(Near(Took(book, RAIDER), 30.0f + CopyHealth::MIN_LEFT));
	CHECK(book.Unseen() == 1);
	CHECK(book.Lethal() == 1);

	// No shot of ours: asked again for a frame's worth, then nobody's, and nothing goes.
	auto none = Seen();
	CHECK(none.Fell(RAIDER, 0.0f, T0 + Ms(1050)));
	CHECK(!none.Fell(RAIDER, 0.0f, T0 + Ms(1050) + CopyHealth::JUDGE_AFTER));
	CHECK(!none.FellToUs(RAIDER));
	CHECK(Took(none, RAIDER) == 0.0f);
	CHECK(none.Unseen() == 0);
}

TEST("unseen hits: an explosion of ours booked on a copy after a later shot keeps the later shot's time")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + Ms(3000));  // our shot
	book.OurShot(RAIDER, T0 + Ms(1500));  // the blast of an earlier one, booked a frame after
	// A drop 1.4 s after the blast but 0.1 s after the shot: the shot's.
	CHECK(book.Check(RAIDER, 20.0f, T0 + Ms(3100), 30.0f) == 0.0f);
	CHECK(book.Check(RAIDER, 20.0f, T0 + Ms(3100) + CopyHealth::JUDGE_AFTER, 30.0f) == 0.0f);
	CHECK(Near(Took(book, RAIDER), 10.0f));
}
