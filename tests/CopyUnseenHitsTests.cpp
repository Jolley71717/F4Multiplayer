// Our player's shot that hurts a copy with no hit event for it (CopyHealth::Book::OurShot, SHOT_REACH). On the rig a
// host's shot took 24.6 off a friend's raider in the host's game and no hit event ever reached the mod: CopyWatch gave
// the drop back as "no hit of ours", so the shot healed what it hit and the raider never lost a point in the game that
// ran it. The host could not kill that raider while the friend could; a player's game whose hit events stop turns
// every NPC a friend runs into one they can't kill. Each test names what the players would see go wrong if it failed.

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
	using std::chrono::milliseconds;

	constexpr std::uint32_t RAIDER = 0x0007BD0E;
	const CopyHealth::Clock::time_point T0{};

	bool Near(float a_value, float a_expected, float a_slack = 0.01f)
	{
		return std::abs(a_value - a_expected) < a_slack;
	}

	// A copy at full health, seen once.
	Book Seen(float a_health = 30.0f)
	{
		Book book;
		CHECK(book.Check(RAIDER, a_health, T0, a_health) == 0.0f);
		return book;
	}

	constexpr float DEG = std::numbers::pi_v<float> / 180.0f;

	// A player at the origin, eyes 120 up, looking a_yaw degrees from +y toward +x and a_pitch degrees down.
	CopyHealth::Aim Looking(float a_yaw, float a_pitch = 0.0f)
	{
		return { .x = 0.0f, .y = 0.0f, .z = 120.0f, .yaw = a_yaw * DEG, .pitch = a_pitch * DEG };
	}
}

TEST("unseen hits: our shot's damage with no hit event goes to the NPC's game, not back onto the copy (the host could not kill a friend's raider)")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	// The bullet lands a frame later: 30 -> 5.4, and no hit event comes.
	CHECK(book.Check(RAIDER, 5.4f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(book.Check(RAIDER, 5.4f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 24.6f));
	CHECK(book.Unseen() == 1);
	CHECK(book.Health(RAIDER) && Near(*book.Health(RAIDER), 5.4f));
}

TEST("unseen hits: a drop with no shot of ours and no hit is still given back (a hazard only our game has must not hurt a friend's NPC)")
{
	auto book = Seen();
	CHECK(book.Check(RAIDER, 20.0f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 20.0f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f), 10.0f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
	CHECK(book.Unseen() == 0);
}

TEST("unseen hits: a drop long after our shot is not the shot's (a missed shot must not make the next unseen drop ours)")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	const auto late = T0 + milliseconds{ 1000 } + CopyHealth::SHOT_REACH + milliseconds{ 50 };
	CHECK(book.Check(RAIDER, 20.0f, late, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 20.0f, late + CopyHealth::JUDGE_AFTER, 30.0f), 10.0f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
}

TEST("unseen hits: another's hit around the drop keeps it given back (a fire or a friend's blast is not our shot)")
{
	auto book = Seen();
	book.OtherHit(RAIDER, T0 + milliseconds{ 500 });  // a fire it stands in: its ticks follow with no event
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	CHECK(book.Check(RAIDER, 25.0f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 25.0f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f), 5.0f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
	CHECK(book.Unseen() == 0);
}

TEST("unseen hits: a blow of ours left to the owner around the drop keeps it given back (a throw's blast would be dealt twice)")
{
	auto book = Seen();
	book.OwnersBlow(RAIDER, T0 + milliseconds{ 900 });
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	CHECK(book.Check(RAIDER, 10.0f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 10.0f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f), 20.0f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
}

TEST("unseen hits: when the hit event comes, its figure is what goes, as before (the shot adds nothing twice)")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	book.OurHit(RAIDER, T0 + milliseconds{ 1050 }, 15.6f);
	CHECK(book.Check(RAIDER, 14.4f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 15.6f));
	CHECK(book.Unseen() == 0);
}

TEST("unseen hits: a burst with no events: every shot's damage goes, none is given back (an automatic weapon healed the NPC as it fired)")
{
	auto  book = Seen(100.0f);
	float health = 100.0f;
	auto  at = T0 + milliseconds{ 1000 };
	float given = 0.0f;
	for (int shot = 0; shot < 8; ++shot) {
		book.OurShot(RAIDER, at);
		health -= 9.0f;
		given += book.Check(RAIDER, health, at + milliseconds{ 50 }, 100.0f);
		given += book.Check(RAIDER, health, at + milliseconds{ 50 } + CopyHealth::JUDGE_AFTER, 100.0f);
		at += milliseconds{ 400 };  // slower than the judging: each shot's drop is judged alone
	}
	CHECK(given == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 72.0f));
}

TEST("unseen hits: a drop well after a credited shot, with no shot of its own, is given back again")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	CHECK(book.Check(RAIDER, 20.0f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(book.Check(RAIDER, 20.0f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 10.0f));
	const auto later = T0 + milliseconds{ 4000 };
	CHECK(book.Check(RAIDER, 15.0f, later, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 15.0f, later + CopyHealth::JUDGE_AFTER, 30.0f), 5.0f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
}

TEST("unseen hits: a shot aims at the actor the view points at, and at nothing beside, behind, above or out of reach")
{
	using CopyHealth::AimedAt;
	// A raider 500 ahead (+y), standing level with the player.
	CHECK(AimedAt(Looking(0.0f, 5.0f), 0.0f, 500.0f, 0.0f));
	CHECK(AimedAt(Looking(3.0f, 5.0f), 0.0f, 500.0f, 0.0f));   // a little to the side: the gun's spread, the camera
	CHECK(!AimedAt(Looking(30.0f, 5.0f), 0.0f, 500.0f, 0.0f));  // another raider's way
	CHECK(!AimedAt(Looking(180.0f, 5.0f), 0.0f, 500.0f, 0.0f));
	CHECK(!AimedAt(Looking(0.0f, -45.0f), 0.0f, 500.0f, 0.0f));  // at the sky
	CHECK(!AimedAt(Looking(0.0f, 60.0f), 0.0f, 500.0f, 0.0f));   // at the floor in front
	CHECK(!AimedAt(Looking(0.0f, 0.0f), 0.0f, 9000.0f, 0.0f));
	// Due east (+x) across the wrap of the yaw.
	CHECK(AimedAt(Looking(90.0f, 0.0f), 800.0f, 0.0f, 0.0f));
	CHECK(AimedAt(Looking(-270.0f, 0.0f), 800.0f, 0.0f, 0.0f));
	CHECK(AimedAt(Looking(359.0f, 2.0f), 0.0f, 800.0f, 0.0f));
	// A swing at one right in front: wide as it is near.
	CHECK(AimedAt(Looking(30.0f, 20.0f), 0.0f, 70.0f, 0.0f));
	// One below on a slope, the view down at it.
	CHECK(AimedAt(Looking(0.0f, 20.0f), 0.0f, 600.0f, -200.0f));
}

TEST("unseen hits: the owner's report from before our damage reached it is held, not set (the shot would heal back a moment later)")
{
	auto book = Seen();
	book.OurShot(RAIDER, T0 + milliseconds{ 1000 });
	CHECK(book.Check(RAIDER, 5.4f, T0 + milliseconds{ 1050 }, 30.0f) == 0.0f);
	CHECK(book.Check(RAIDER, 5.4f, T0 + milliseconds{ 1050 } + CopyHealth::JUDGE_AFTER, 30.0f) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 24.6f));
	// The owner's game reports 30 still: sent before our damage got there.
	CHECK(Near(book.Heard(RAIDER, 5.4f, 30.0f, T0 + milliseconds{ 1200 }), 5.4f));
	CHECK(book.Holds(RAIDER));
	// Its next report has our damage taken off: that one stands.
	CHECK(Near(book.Heard(RAIDER, 5.4f, 5.4f, T0 + milliseconds{ 1400 }), 5.4f));
	CHECK(!book.Holds(RAIDER));
}

TEST("unseen hits: an automatic weapon's shots closer together than a drop is judged in: all of it is ours")
{
	auto  book = Seen(100.0f);
	float health = 100.0f;
	float given = 0.0f;
	// Frames every 20 ms; a shot every 80 ms from 1000 ms, each landing 40 ms after it leaves; 6 shots.
	for (int ms = 1000; ms <= 2000; ms += 20) {
		const int since = ms - 1000;
		if (since % 80 == 0 && since / 80 < 6) {
			book.OurShot(RAIDER, T0 + milliseconds{ ms });
		}
		if (since >= 40 && (since - 40) % 80 == 0 && (since - 40) / 80 < 6) {
			health -= 7.0f;
		}
		given += book.Check(RAIDER, health, T0 + milliseconds{ ms }, 100.0f);
	}
	CHECK(given == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 42.0f));
	CHECK(Near(health, 58.0f));
}

TEST("unseen hits: a shot is booked on the nearest actor the view points at, not on one behind it or beside it")
{
	using CopyHealth::NearestAimed;
	const auto aim = Looking(0.0f, 5.0f);
	// Two raiders in line ahead, one 1.5 bodies to the side of the near one, one behind the player.
	const std::vector<std::array<float, 3>> at{ { 0.0f, 900.0f, 0.0f }, { 0.0f, 400.0f, 0.0f }, { 150.0f, 400.0f, 0.0f }, { 0.0f, -300.0f, 0.0f } };
	const auto aimed = NearestAimed(aim, at);
	CHECK(aimed && *aimed == 1);
	// Turned toward the one to the side.
	const auto side = NearestAimed(Looking(20.5f, 5.0f), at);
	CHECK(side && *side == 2);
	// At nobody.
	CHECK(!NearestAimed(Looking(90.0f, 5.0f), at));
	CHECK(!NearestAimed(aim, {}));
}
