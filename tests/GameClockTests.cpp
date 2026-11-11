// The world's own time (src/game/GameClock.h): a clock that runs by the game's frame time, with the time multiplier in
// it, so that the windows pairing a drop on a copy with our shot (src/game/CopyHealth.h) and an explosion with the throw
// before it (src/game/RemoteEdits.h ThrowWatches) are the world's seconds at any speed. Under the group's VATS slow
// motion (0.3) a missile's 0.4 s flight took 1.3 s on the wall clock, outside both windows, and its drop was given back.
#include "Test.h"

#include "game/CopyHealth.h"
#include "game/GameClock.h"
#include "game/RemoteEdits.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace
{
	using namespace std::chrono_literals;
	using Clock = GameClock::Clock;
	using GameClock::Timeline;
	using CopyHealth::Book;
	using std::chrono::milliseconds;

	constexpr std::uint32_t RAIDER = 0x0007BD0E;
	constexpr std::uint32_t PLAYER = 0x00000014;
	constexpr float         FRAME = 1.0f / 60.0f;  // a 60 Hz frame's real time, seconds

	float Seconds(Clock::duration a_duration)
	{
		return std::chrono::duration<float>(a_duration).count();
	}

	bool Near(float a_value, float a_expected, float a_slack = 0.02f)
	{
		return std::abs(a_value - a_expected) < a_slack;
	}

	// A game's frames as the clock sees them: 60 a second on the wall, each moving the world FRAME times the rate.
	struct Game
	{
		Timeline          clock;
		Clock::time_point wall{ Clock::time_point{} + 1000s };
		std::uint64_t     mark = 0;

		Game() { clock.Step(wall, ++mark, FRAME, FRAME); }  // the first frame starts the time

		// Runs a_wallSeconds of wall time at a_rate (the game's multiplier): the world's time after.
		Clock::time_point Run(float a_wallSeconds, float a_rate)
		{
			const int frames = static_cast<int>(std::lround(a_wallSeconds / FRAME));
			for (int i = 0; i < frames; ++i) {
				wall += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(FRAME));
				clock.Step(wall, ++mark, FRAME * a_rate, FRAME);
			}
			return clock.Now(wall);
		}

		[[nodiscard]] Clock::time_point Now() const { return clock.Now(wall); }
	};

	// What our hits took off the copy, sent now (CopyHealth::Book::TakeOurs): the amount, 0 for nothing.
	float Took(Book& a_book, Clock::time_point a_now)
	{
		const auto sent = a_book.TakeOurs(RAIDER, a_now);
		return sent ? sent->amount : 0.0f;
	}
}

TEST("game clock: at full speed the world's time runs with the wall clock, between frames too")
{
	Game game;
	const auto start = game.Now();
	CHECK(start == game.wall);
	const auto after = game.Run(2.0f, 1.0f);
	CHECK(Near(Seconds(after - start), 2.0f));
	CHECK(Near(Seconds(game.clock.Now(game.wall + 8ms) - after), 0.008f, 0.001f));
	CHECK(game.clock.rate == 1.0f);
}

TEST("game clock: at the group's 0.3 a wall second is 0.3 s of the world's, between frames too")
{
	Game game;
	const auto start = game.Now();
	const auto after = game.Run(1.3f, 0.3f);
	CHECK(Near(Seconds(after - start), 0.39f));
	CHECK(Near(game.clock.rate, 0.3f, 0.001f));
	CHECK(Near(Seconds(game.clock.Now(game.wall + 10ms) - after), 0.003f, 0.0005f));
}

TEST("game clock: the same timer mark twice (our work ran twice in one of the engine's frames) steps nothing")
{
	Game game;
	const auto before = game.Now();
	game.wall += 5ms;
	game.clock.Step(game.wall, game.mark, FRAME, FRAME);
	CHECK(game.Now() == before);
	game.wall += 5ms;
	game.clock.Step(game.wall, ++game.mark, FRAME, FRAME);
	CHECK(Near(Seconds(game.Now() - before), FRAME, 0.001f));
}

TEST("game clock: a paused game (no delta) moves nothing, and its time stands between frames")
{
	Game game;
	const auto before = game.Now();
	game.wall += 16ms;
	game.clock.Step(game.wall, ++game.mark, 0.0f, FRAME);
	CHECK(game.Now() == before);
	CHECK(game.clock.Now(game.wall + 100ms) == before);
	CHECK(game.clock.rate == 0.0f);
}

TEST("game clock: a stall steps a quarter second at most, and the time runs on for 250 ms after a step at most")
{
	Game game;
	const auto before = game.Now();
	game.wall += 2s;
	game.clock.Step(game.wall, ++game.mark, 2.0f, 2.0f);
	CHECK(Near(Seconds(game.Now() - before), GameClock::MAX_FRAME, 0.001f));
	CHECK(Near(Seconds(game.clock.Now(game.wall + 5s) - game.Now()), 0.25f, 0.001f));
	// A time not a number steps nothing.
	const auto held = game.Now();
	game.wall += 16ms;
	game.clock.Step(game.wall, ++game.mark, std::nanf(""), FRAME);
	CHECK(game.Now() == held);
}

TEST("game clock: the rate is the frame's delta over its real delta; none is paused, no real delta is full speed, a leap is capped")
{
	CHECK(Near(GameClock::Rate(0.005f, FRAME), 0.3f, 0.001f));
	CHECK(GameClock::Rate(FRAME, FRAME) == 1.0f);
	CHECK(GameClock::Rate(0.0f, FRAME) == 0.0f);
	CHECK(GameClock::Rate(-1.0f, FRAME) == 0.0f);
	CHECK(GameClock::Rate(FRAME, 0.0f) == 1.0f);
	CHECK(GameClock::Rate(1.0f, 0.001f) == GameClock::MAX_RATE);
	CHECK(GameClock::Rate(std::nanf(""), FRAME) == 0.0f);
}

TEST("game clock: a missile's 0.4 s flight at the group's 0.3 (1.3 s on the wall) is still our shot's: its drop goes to the NPC's game, not back onto the copy")
{
	Game game;
	Book book;
	CHECK(book.Check(RAIDER, 30.0f, game.Now(), 30.0f) == 0.0f);
	book.OurShot(RAIDER, game.Now());  // the missile leaves
	game.Run(1.3f, 0.3f);              // ... and flies 0.39 s of the world's time
	CHECK(book.Check(RAIDER, 5.4f, game.Now(), 30.0f) == 0.0f);  // judged for JUDGE_AFTER of the world's time
	game.Run(0.4f, 0.3f);
	CHECK(book.Check(RAIDER, 5.4f, game.Now(), 30.0f) == 0.0f);
	CHECK(book.Unseen() == 1);
	CHECK(Near(Took(book, game.Now()), 24.6f, 0.01f));
}

TEST("game clock: the same flight on the wall clock was outside SHOT_REACH and given back (the symptom behind the option)")
{
	const Clock::time_point T0{};
	Book                    book;
	CHECK(book.Check(RAIDER, 30.0f, T0, 30.0f) == 0.0f);
	book.OurShot(RAIDER, T0);
	CHECK(book.Check(RAIDER, 5.4f, T0 + 1300ms, 30.0f) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 5.4f, T0 + 1300ms + CopyHealth::JUDGE_AFTER, 30.0f), 24.6f, 0.01f));
	CHECK(book.Unseen() == 0);
}

TEST("game clock: at full speed the windows are what they were: a drop 1.3 s after the shot is no longer its")
{
	Game game;
	Book book;
	CHECK(book.Check(RAIDER, 30.0f, game.Now(), 30.0f) == 0.0f);
	book.OurShot(RAIDER, game.Now());
	game.Run(1.3f, 1.0f);
	CHECK(book.Check(RAIDER, 5.4f, game.Now(), 30.0f) == 0.0f);
	game.Run(0.2f, 1.0f);
	CHECK(Near(book.Check(RAIDER, 5.4f, game.Now(), 30.0f), 24.6f, 0.01f));
	CHECK(book.Unseen() == 0);
}

TEST("game clock: our hit event's grace is the world's second too: a grenade's drop 1.3 s of wall time after our hit at 0.3 is ours")
{
	Game game;
	Book book;
	CHECK(book.Check(RAIDER, 30.0f, game.Now(), 30.0f) == 0.0f);
	book.OurHit(RAIDER, game.Now());  // the event, no figure
	game.Run(1.3f, 0.3f);
	CHECK(book.Check(RAIDER, 10.0f, game.Now(), 30.0f) == 0.0f);
	CHECK(Near(Took(book, game.Now()), 20.0f, 0.01f));
	// At full speed the same wall time is past the grace: judged, then given back.
	Game full;
	Book other;
	CHECK(other.Check(RAIDER, 30.0f, full.Now(), 30.0f) == 0.0f);
	other.OurHit(RAIDER, full.Now());
	full.Run(1.3f, 1.0f);
	CHECK(other.Check(RAIDER, 10.0f, full.Now(), 30.0f) == 0.0f);
	full.Run(0.2f, 1.0f);
	CHECK(Near(other.Check(RAIDER, 10.0f, full.Now(), 30.0f), 20.0f, 0.01f));
}

TEST("game clock: a frag's 3 s fuse under the group's 0.3 (10 s on the wall) still ends inside the throw's watch, so its blast is the throw's")
{
	Game                      game;
	RemoteEdits::ThrowWatches throws;
	throws.Arm(PLAYER, game.Now() + 8s);  // WeaponFire's EXPLOSION_WATCH
	game.Run(10.0f, 0.3f);                // the fuse: 3 s of the world's time
	CHECK(throws.Shared(PLAYER, game.Now()));
	// On the wall clock the watch had ended 2 s before the blast.
	RemoteEdits::ThrowWatches wall;
	const Clock::time_point   T0{};
	wall.Arm(PLAYER, T0 + 8s);
	CHECK(!wall.Shared(PLAYER, T0 + 10s));
	// At full speed the watch ends as it did: a blast 9 s after the throw is no throw's.
	Game full;
	RemoteEdits::ThrowWatches fast;
	fast.Arm(PLAYER, full.Now() + 8s);
	full.Run(9.0f, 1.0f);
	CHECK(!fast.Shared(PLAYER, full.Now()));
}
