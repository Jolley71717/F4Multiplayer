// How the teleport key moves the player across the worldspace they are in, and when the landing is safe for the travel
// autosave (src/game/TeleportRules.h, which Party.cpp follows): a SetPosition far across the Commonwealth landed the
// player short of the friend and in the air, and the autosave made during the fall loaded the fall again.

#include "Test.h"

#include "game/TeleportRules.h"

namespace
{
	using TeleportRules::Clock;
	using TeleportRules::Footing;
	using TeleportRules::Landing;
	using TeleportRules::Step;
	using namespace std::chrono_literals;

	constexpr Footing STANDING{ .loaded = true, .onGround = true, .alive = true };
	constexpr Footing FALLING{ .loaded = true, .onGround = false, .alive = true };

	const Clock::time_point START = Clock::time_point{} + 1h;
}

TEST("teleport: a friend in the cells next to ours is a SetPosition; one beyond them loads their cell first")
{
	using TeleportRules::GridOf;
	using TeleportRules::NeedsLoad;
	CHECK(GridOf(0.0f) == 0);
	CHECK(GridOf(4095.0f) == 0);
	CHECK(GridOf(4096.0f) == 1);
	CHECK(GridOf(-1.0f) == -1);
	CHECK(GridOf(-88371.0f) == -22);
	CHECK(!NeedsLoad(100.0f, 100.0f, 200.0f, 200.0f));     // a few steps
	CHECK(!NeedsLoad(100.0f, 100.0f, 8000.0f, -4000.0f));  // the next cells over
	CHECK(NeedsLoad(100.0f, 100.0f, 8200.0f, 100.0f));     // two cells east
	CHECK(NeedsLoad(100.0f, 100.0f, 100.0f, -4200.0f));    // two cells south
	// The test that found it: near Corvega to a friend at Vault 111, about 40,000 units.
	CHECK(NeedsLoad(-59769.0f, 63168.0f, -88371.0f, 91771.0f));
	// After the cow, the player stands in the friend's cell: the SetPosition to their spot goes ahead.
	CHECK(!NeedsLoad(-88064.0f, 92160.0f, -88371.0f, 91771.0f));
}

TEST("teleport: the landing autosave waits until the player has stood on the ground for a moment")
{
	Landing landing;
	CHECK(landing.Update(START, STANDING) == Step::kNone);  // no teleport
	landing.Begin(START, true);
	CHECK(landing.Active());
	CHECK(landing.Update(START + 100ms, FALLING) == Step::kWait);
	CHECK(landing.Update(START + 2500ms, STANDING) == Step::kWait);  // on the ground, the cells still settling
	CHECK(landing.Update(START + 3s, STANDING) == Step::kWait);   // and not for long enough yet
	CHECK(landing.Update(START + 3500ms, STANDING) == Step::kSave);
	CHECK(!landing.Active());
	CHECK(landing.Update(START + 4s, STANDING) == Step::kNone);  // asked once
}

TEST("teleport: no autosave while the player is in the air; a moment on the ground before a fall is no landing")
{
	Landing landing;
	landing.Begin(START, true);
	// What the log showed: 3.5 s after the teleport the player was still falling. Before, the autosave was made then.
	CHECK(landing.Update(START + 3500ms, FALLING) == Step::kWait);
	CHECK(landing.Update(START + 4s, STANDING) == Step::kWait);  // a frame on the ground
	CHECK(landing.Update(START + 4100ms, FALLING) == Step::kWait);  // then falling again: the standing starts over
	CHECK(landing.Update(START + 5s, STANDING) == Step::kWait);
	CHECK(landing.Update(START + 5900ms, STANDING) == Step::kWait);
	CHECK(landing.Update(START + 6s, STANDING) == Step::kSave);
}

TEST("teleport: no autosave of a player down, dead, loading, or not landed in 30 s")
{
	Landing landing;
	landing.Begin(START, true);
	CHECK(landing.Update(START + 5s, Footing{ .loaded = true, .onGround = true, .alive = false }) == Step::kNoSave);
	CHECK(!landing.Active());

	landing.Begin(START, true);
	CHECK(landing.Update(START + 3s, Footing{ .loaded = true, .onGround = true, .alive = true, .loading = true }) == Step::kWait);
	CHECK(landing.Update(START + 4s, Footing{ .loaded = false, .onGround = true, .alive = true }) == Step::kWait);
	CHECK(landing.Update(START + 29s, FALLING) == Step::kWait);
	CHECK(landing.Update(START + 30s, FALLING) == Step::kNoSave);

	landing.Begin(START, true);
	landing.Cancel();  // a load, or a new teleport
	CHECK(landing.Update(START + 5s, STANDING) == Step::kNone);
}

TEST("teleport: a hop lands without an autosave")
{
	Landing landing;
	landing.Begin(START, false);
	CHECK(landing.Update(START + 1s, STANDING) == Step::kWait);
	CHECK(landing.Update(START + 3s, STANDING) == Step::kLanded);
}

TEST("teleport: a fall does no damage from the move until the player has landed, for at most 15 s")
{
	Landing landing;
	CHECK(!landing.HoldsFall(START));
	landing.Begin(START, true);
	CHECK(landing.HoldsFall(START));
	CHECK(landing.HoldsFall(START + 14s));
	CHECK(!landing.HoldsFall(START + 15s));
	CHECK(landing.Update(START + 2s, STANDING) == Step::kWait);
	CHECK(landing.Update(START + 3s, STANDING) == Step::kSave);
	CHECK(!landing.HoldsFall(START + 3s));  // landed: a jump from here is the player's own
}
