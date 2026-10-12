#include "Test.h"

#include "game/IdleRelease.h"

#include <vector>

// When a mirrored NPC stuck in an idle (Dogmeat sitting) is told to stop it so it can run after its
// owner's copy: at once, then less and less often while its graph keeps refusing to move, and not at all
// once a stuck spell has had its tries.
namespace
{
	using namespace std::chrono_literals;

	const auto t0 = IdleRelease::Clock::time_point{} + 1h;
	constexpr auto FRAME = 16ms;

	// The times the gate says to stop, over frames from a_from to a_to in which the graph refuses to move.
	std::vector<IdleRelease::Clock::time_point> Refusals(IdleRelease::Gate& a_gate, IdleRelease::Clock::time_point a_from, IdleRelease::Clock::time_point a_to)
	{
		std::vector<IdleRelease::Clock::time_point> due;
		for (auto t = a_from; t < a_to; t += FRAME) {
			if (a_gate.Due(true, t)) {
				due.push_back(t);
			}
		}
		return due;
	}
}

TEST("idle release: the first refused start stops the idle at once")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
}

TEST("idle release: a graph still refusing after the first stop gets the next only after RETRY")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
	const auto due = Refusals(gate, t0 + FRAME, t0 + IdleRelease::RETRY);
	CHECK(due.empty());
	CHECK(gate.Due(true, t0 + IdleRelease::RETRY));
}

TEST("idle release: while the graph keeps refusing, the stops come less and less often, and stop")
{
	IdleRelease::Gate gate;
	const auto        due = Refusals(gate, t0, t0 + 5min);
	REQUIRE(due.size() == IdleRelease::TRIES);
	CHECK(gate.Spent());
	IdleRelease::Clock::duration last{};
	for (std::size_t i = 1; i < due.size(); ++i) {
		const auto gap = due[i] - due[i - 1];
		CHECK(gap >= IdleRelease::RETRY);
		CHECK(gap + FRAME >= last);                   // never sooner than the one before
		CHECK(gap < IdleRelease::MAX_RETRY + FRAME);  // and never more than MAX_RETRY apart
		last = gap;
	}
}

TEST("idle release: the wait doubles from RETRY up to MAX_RETRY")
{
	CHECK(IdleRelease::Backoff(1) == IdleRelease::RETRY);
	CHECK(IdleRelease::Backoff(2) == IdleRelease::RETRY * 2);
	CHECK(IdleRelease::Backoff(3) == IdleRelease::RETRY * 4);
	CHECK(IdleRelease::Backoff(4) == IdleRelease::MAX_RETRY);
	CHECK(IdleRelease::Backoff(100) == IdleRelease::MAX_RETRY);
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

TEST("idle release: a spell that ran out starts over when the owner's copy next stops and goes")
{
	IdleRelease::Gate gate;
	const auto        end = t0 + 5min;
	CHECK(Refusals(gate, t0, end).size() == IdleRelease::TRIES);
	CHECK(!gate.Due(true, end));
	// The owner's copy stood still for a while (nobody asked the graph to move), then went again.
	const auto again = end + IdleRelease::SPELL_GAP + FRAME;
	CHECK(gate.Due(true, again));
	CHECK(!gate.Spent());
}

TEST("idle release: a hitch shorter than the gap does not start a new spell")
{
	IdleRelease::Gate gate;
	CHECK(gate.Due(true, t0));
	// No frame for 0.9 s (a slow frame), then refusing again: still the same spell, so no stop yet.
	CHECK(!gate.Due(true, t0 + 900ms));
	CHECK(gate.Due(true, t0 + IdleRelease::RETRY));
}
