#include "Test.h"

#include "game/ItemWork.h"

// An actor is worked on (items given, equipped, taken away) only once the engine has it ready and its 3D has stood
// unchanged for a moment: the queue the engine runs after such a change uses the actor's 3D as it is then.
namespace
{
	using namespace std::chrono_literals;
	using ItemWork::Clock;
	using ItemWork::Verdict;

	const auto t0 = Clock::time_point{} + 1h;

	ItemWork::Facts Loaded(std::uintptr_t a_root = 0x1000)
	{
		return { .exists = true, .root = a_root, .hasProcess = true, .cellAttached = true };
	}
}

TEST("item work: an actor with no 3D is not ready, and says why")
{
	ItemWork::Track track;
	auto facts = Loaded(0);
	CHECK(ItemWork::Check(track, facts, t0) == Verdict::kNo3D);
	CHECK(ItemWork::Check(track, facts, t0 + 10s) == Verdict::kNo3D);
}

TEST("item work: a 3D that has just come in settles for SETTLE before the actor is ready")
{
	ItemWork::Track track;
	const auto facts = Loaded();
	CHECK(ItemWork::Check(track, facts, t0) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, facts, t0 + ItemWork::SETTLE - 1ms) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, facts, t0 + ItemWork::SETTLE) == Verdict::kReady);
	CHECK(ItemWork::Check(track, facts, t0 + 1h) == Verdict::kReady);
}

TEST("item work: a 3D that is replaced is timed afresh, and so is one that went and came back")
{
	ItemWork::Track track;
	CHECK(ItemWork::Check(track, Loaded(0x1000), t0) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, Loaded(0x1000), t0 + 1s) == Verdict::kReady);
	// Rebuilt (a power armor switch, a fresh load): a new root.
	CHECK(ItemWork::Check(track, Loaded(0x2000), t0 + 2s) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, Loaded(0x2000), t0 + 2s + ItemWork::SETTLE) == Verdict::kReady);
	// Gone, then back at the same address: still a new 3D.
	CHECK(ItemWork::Check(track, Loaded(0), t0 + 3s) == Verdict::kNo3D);
	CHECK(ItemWork::Check(track, Loaded(0x2000), t0 + 4s) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, Loaded(0x2000), t0 + 4s + ItemWork::SETTLE) == Verdict::kReady);
}

TEST("item work: no AI process, or a cell not attached, is not ready however long the 3D has stood")
{
	ItemWork::Track track;
	auto facts = Loaded();
	CHECK(ItemWork::Check(track, facts, t0) == Verdict::kSettling);
	facts.hasProcess = false;
	CHECK(ItemWork::Check(track, facts, t0 + 1h) == Verdict::kNoProcess);
	facts.hasProcess = true;
	facts.cellAttached = false;
	CHECK(ItemWork::Check(track, facts, t0 + 1h) == Verdict::kCellNotAttached);
	facts.cellAttached = true;
	CHECK(ItemWork::Check(track, facts, t0 + 1h) == Verdict::kReady);
}

TEST("item work: a reference that is gone is gone, and forgotten")
{
	ItemWork::Track track;
	CHECK(ItemWork::Check(track, Loaded(), t0) == Verdict::kSettling);
	ItemWork::Facts gone;
	gone.exists = false;
	CHECK(ItemWork::Check(track, gone, t0 + 1s) == Verdict::kGone);
	CHECK(track.root == 0);
}

TEST("item work: a stretch of waiting is said once, and again after the next one")
{
	ItemWork::Track track;
	CHECK(ItemWork::Check(track, Loaded(0), t0) == Verdict::kNo3D);
	CHECK(ItemWork::FirstSay(track));
	CHECK(!ItemWork::FirstSay(track));
	CHECK(ItemWork::Check(track, Loaded(), t0 + 1s) == Verdict::kSettling);
	CHECK(!ItemWork::FirstSay(track));
	CHECK(ItemWork::Check(track, Loaded(), t0 + 2s) == Verdict::kReady);
	CHECK(ItemWork::Check(track, Loaded(0), t0 + 3s) == Verdict::kNo3D);
	CHECK(ItemWork::FirstSay(track));
}

TEST("item work: after work on an actor the next waits QUIET, so the engine's queue has run on what was changed")
{
	ItemWork::Track track;
	const auto facts = Loaded();
	CHECK(ItemWork::Check(track, facts, t0) == Verdict::kSettling);
	CHECK(ItemWork::Check(track, facts, t0 + 1s) == Verdict::kReady);
	ItemWork::Worked(track, t0 + 1s);
	CHECK(ItemWork::Check(track, facts, t0 + 1s) == Verdict::kBusy);
	CHECK(ItemWork::Check(track, facts, t0 + 1s + ItemWork::QUIET - 1ms) == Verdict::kBusy);
	CHECK(ItemWork::Check(track, facts, t0 + 1s + ItemWork::QUIET) == Verdict::kReady);
}

TEST("item work: a stand-in's first dressing waits out the engine's own outfit being put on it")
{
	// Its 3D comes in at t0; the engine equips its base outfit and queues those stacks' updates; our dressing would take those
	// stacks out (Apply empties it): not until the 3D has stood for SETTLE.
	ItemWork::Track track;
	const auto facts = Loaded(0x5000);
	for (auto t = t0; t < t0 + ItemWork::SETTLE; t += 16ms) {
		CHECK(ItemWork::Check(track, facts, t) != Verdict::kReady);
	}
	CHECK(ItemWork::Check(track, facts, t0 + ItemWork::SETTLE) == Verdict::kReady);
}
