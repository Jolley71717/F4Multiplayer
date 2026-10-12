#include "Test.h"

#include "game/IdleRelease.h"

// When a mirrored NPC stuck in an idle (Dogmeat sitting) is told to stop it so it can run after its
// owner's copy: at once, then at a steady pace while its graph keeps refusing to move.
namespace
{
	using namespace std::chrono_literals;

	const auto t0 = IdleRelease::Clock::time_point{} + 1h;
}

TEST("idle release: the first refused start stops the idle at once")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
}

TEST("idle release: while the graph keeps refusing, the stop is retried every RETRY, not every frame")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
	CHECK(!gate.Due(true, t0 + 16ms));
	CHECK(!gate.Due(true, t0 + IdleRelease::RETRY - 1ms));
	CHECK(gate.Due(true, t0 + IdleRelease::RETRY));
	CHECK(!gate.Due(true, t0 + IdleRelease::RETRY + 16ms));
	CHECK(gate.Due(true, t0 + IdleRelease::RETRY * 2));
}

TEST("idle release: nothing is sent while the graph takes the start")
{
	IdleRelease::Gate gate;
	CHECK(!gate.Due(false, t0));
	CHECK(!gate.Due(false, t0 + 1s));
}

TEST("idle release: once the NPC got moving, its next sit is stopped at once")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
	CHECK(!gate.Due(false, t0 + 100ms));  // it stood up and ran
	CHECK(gate.Due(true, t0 + 200ms));    // sat again later and refuses: no waiting out the old retry
}
