#include "Test.h"

#include "game/StuckSyncAnim.h"

using namespace std::chrono_literals;

namespace
{
	// In the world, marked, nothing running: the stuck player of a bad start.
	StuckSyncAnim::Facts Stuck()
	{
		return { .inWorld = true, .marked = true };
	}

	// Frames from a_from, every 16 ms up to a_until, with the same facts; how many said to clear.
	int Run(StuckSyncAnim::Guard& a_guard, const StuckSyncAnim::Facts& a_facts, StuckSyncAnim::Clock::time_point a_from, std::chrono::milliseconds a_until)
	{
		int clears = 0;
		for (auto t = 0ms; t <= a_until; t += 16ms) {
			clears += a_guard.Frame(a_facts, a_from + t) == StuckSyncAnim::Step::kClear;
		}
		return clears;
	}
}

TEST("stuck sync anim: a player marked with nothing running is cleared once, after the grace time")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	CHECK(guard.Frame(Stuck(), t0) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 900ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + StuckSyncAnim::GRACE) == StuckSyncAnim::Step::kClear);
	CHECK(guard.Cleared() == 1);
	CHECK(!guard.Armed());
	CHECK(Run(guard, Stuck(), t0 + 2s, 5000ms) == 0);  // once per game start
}

TEST("stuck sync anim: a free player settles the game, and a later mark is the manager's")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	CHECK(guard.Frame({ .inWorld = true }, t0) == StuckSyncAnim::Step::kNothing);
	CHECK(!guard.Armed());
	CHECK(Run(guard, Stuck(), t0 + 1s, 5000ms) == 0);
	CHECK(guard.Cleared() == 0);
}

TEST("stuck sync anim: nothing is done at the main menu or behind a loading screen, and the wait starts in the world")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	CHECK(Run(guard, { .marked = true }, t0, 10000ms) == 0);  // the main menu: the garbage mark is there already
	CHECK(guard.Armed());
	CHECK(guard.Frame(Stuck(), t0 + 11s) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame({ .marked = true }, t0 + 11500ms) == StuckSyncAnim::Step::kNothing);  // a loading screen: the wait starts over
	CHECK(guard.Frame(Stuck(), t0 + 12s) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 12900ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 13s) == StuckSyncAnim::Step::kClear);
}

TEST("stuck sync anim: a real synced animation is never cleared, whoever's it is and however long it runs")
{
	const auto t0 = StuckSyncAnim::Clock::now();
	for (const auto& facts : { StuckSyncAnim::Facts{ .inWorld = true, .marked = true, .syncAnimsRunning = true },
			 StuckSyncAnim::Facts{ .inWorld = true, .marked = true, .inScene = true },
			 StuckSyncAnim::Facts{ .inWorld = true, .marked = true, .inFurniture = true },
			 StuckSyncAnim::Facts{ .inWorld = true, .marked = true, .inDialogue = true },
			 StuckSyncAnim::Facts{ .inWorld = true, .marked = true, .deadOrDown = true } }) {
		StuckSyncAnim::Guard guard;
		CHECK(Run(guard, facts, t0, 30000ms) == 0);
		CHECK(guard.Armed());
		// It ends and the manager clears its mark: settled, nothing cleared.
		CHECK(guard.Frame({ .inWorld = true }, t0 + 31s) == StuckSyncAnim::Step::kNothing);
		CHECK(!guard.Armed());
		CHECK(guard.Cleared() == 0);
	}
}

TEST("stuck sync anim: a synced animation that blinks in the grace time starts the wait over")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	auto                 running = Stuck();
	running.syncAnimsRunning = true;
	CHECK(guard.Frame(Stuck(), t0) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 800ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(running, t0 + 816ms) == StuckSyncAnim::Step::kNothing);  // one being set up: it may set the mark
	CHECK(guard.Frame(Stuck(), t0 + 832ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 1500ms) == StuckSyncAnim::Step::kNothing);  // 1 s after 0 but not after 832 ms
	CHECK(guard.Frame(Stuck(), t0 + 1832ms) == StuckSyncAnim::Step::kClear);
}

TEST("stuck sync anim: a scene that ends with the mark left on is cleared after it, as a stuck save is")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	auto                 scene = Stuck();
	scene.inScene = true;
	CHECK(Run(guard, scene, t0, 20000ms) == 0);
	CHECK(guard.Frame(Stuck(), t0 + 21s) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 22s) == StuckSyncAnim::Step::kClear);
}

TEST("stuck sync anim: each game start is looked at again, a stuck save's load as much as the run's first game")
{
	StuckSyncAnim::Guard guard;
	const auto           t0 = StuckSyncAnim::Clock::now();
	CHECK(guard.Frame({ .inWorld = true }, t0) == StuckSyncAnim::Step::kNothing);  // the first game: free
	CHECK(!guard.Armed());
	guard.Arm();  // a save made while stuck loads
	CHECK(guard.Armed());
	CHECK(Run(guard, Stuck(), t0 + 10s, 1100ms) == 1);
	CHECK(guard.Cleared() == 1);
	guard.Arm();  // a load half way through the wait starts it over
	CHECK(guard.Frame(Stuck(), t0 + 20s) == StuckSyncAnim::Step::kNothing);
	guard.Arm();
	CHECK(guard.Frame(Stuck(), t0 + 20500ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 21200ms) == StuckSyncAnim::Step::kNothing);
	CHECK(guard.Frame(Stuck(), t0 + 21500ms) == StuckSyncAnim::Step::kClear);
	CHECK(guard.Cleared() == 2);
}
