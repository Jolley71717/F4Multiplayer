// A copy lying dead while its NPC lives in the runner's game is raised (CopyRevive.h, as WorldSync's RaiseCopies asks
// once a second): only once every fact has held for DEAD_FOR, never a story character or a death the session has, and
// at most TRIES times however often it dies again.

#include "Test.h"

#include "game/CopyRevive.h"

using namespace std::chrono_literals;

namespace
{
	using CopyRevive::Clock;
	using CopyRevive::Facts;
	using CopyRevive::Verdict;

	constexpr std::uint32_t RAIDER = 0x0001F401;
	constexpr std::uint32_t PRESTON = 0x0001A4D7;

	// Our copy dead, alive for the session as we know it, another game runs it and was heard from: raise.
	constexpr Facts DEAD_HERE_ALIVE_THERE{ .deadHere = true, .deadForUs = false, .runsElsewhere = true, .heardAlive = true, .story = false };
}

TEST("copy revive: a copy killed by a blast its runner's game dealt elsewhere is raised once it has lain dead a moment")
{
	CopyRevive::Book book;
	const auto       t0 = Clock::now();
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kWait);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0 + CopyRevive::DEAD_FOR - 1ms) == Verdict::kWait);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0 + CopyRevive::DEAD_FOR) == Verdict::kRaise);
	CHECK(book.Raised() == 1);

	// It stands: no longer judged (NpcSync hears no state for it while dead), and forgotten in time.
	book.Prune(t0 + CopyRevive::DEAD_FOR + 1ms);
	CHECK(book.Size() == 0);
}

TEST("copy revive: nothing is raised while the session has it dead, nobody else runs it, its runner is silent, or it only went down")
{
	CopyRevive::Book book;
	const auto       t0 = Clock::now();
	auto             facts = DEAD_HERE_ALIVE_THERE;
	facts.deadForUs = true;  // our report is out, or a friend's death told us (the ledger forgets ours once not taken)
	CHECK(book.Judge(RAIDER, facts, t0 + 1h) == Verdict::kNo);
	facts = DEAD_HERE_ALIVE_THERE;
	facts.runsElsewhere = false;  // our game runs it, or nobody does: the server took a death
	CHECK(book.Judge(RAIDER, facts, t0 + 1h) == Verdict::kNo);
	facts = DEAD_HERE_ALIVE_THERE;
	facts.heardAlive = false;  // no state lately: dying there too, or not loaded there
	CHECK(book.Judge(RAIDER, facts, t0 + 1h) == Verdict::kNo);
	facts = DEAD_HERE_ALIVE_THERE;
	facts.deadHere = false;  // alive, or down and getting up (essential)
	CHECK(book.Judge(RAIDER, facts, t0 + 1h) == Verdict::kNo);
	facts = DEAD_HERE_ALIVE_THERE;
	facts.story = true;  // a quest's character: its death ran scripts here, left as it lies
	CHECK(book.Judge(PRESTON, facts, t0) == Verdict::kNo);
	CHECK(book.Judge(PRESTON, facts, t0 + 1h) == Verdict::kNo);
	CHECK(book.Raised() == 0);
}

TEST("copy revive: the wait starts over when a fact breaks (the runner's death report came, then a game ran it again)")
{
	CopyRevive::Book book;
	const auto       t0 = Clock::now();
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kWait);
	auto facts = DEAD_HERE_ALIVE_THERE;
	facts.deadForUs = true;
	CHECK(book.Judge(RAIDER, facts, t0 + 2s) == Verdict::kNo);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0 + 4s) == Verdict::kWait);  // DEAD_FOR from here, not from t0
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0 + 4s + CopyRevive::DEAD_FOR) == Verdict::kRaise);
}

TEST("copy revive: a copy the engine leaves dead, or that keeps dying, is tried TRIES times, RETRY_AFTER apart, then left")
{
	CopyRevive::Book book;
	auto             t = Clock::now();
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t) == Verdict::kWait);
	t += CopyRevive::DEAD_FOR;
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t) == Verdict::kRaise);

	// Raised, then dead again at once (what killed it here is still there): our report goes out, so the session has it
	// dead for us a while, and the tries go on counting.
	auto reported = DEAD_HERE_ALIVE_THERE;
	reported.deadForUs = true;
	CHECK(book.Judge(RAIDER, reported, t + 1s) == Verdict::kNo);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + 6s) == Verdict::kWait);                     // RETRY_AFTER not over
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + CopyRevive::RETRY_AFTER) == Verdict::kRaise);  // DEAD_FOR over too
	t += CopyRevive::RETRY_AFTER;
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + CopyRevive::RETRY_AFTER) == Verdict::kRaise);
	t += CopyRevive::RETRY_AFTER;
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + CopyRevive::RETRY_AFTER) == Verdict::kGiveUp);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + 1h) == Verdict::kNo);  // said once
	CHECK(book.Raised() == CopyRevive::TRIES);
	CHECK(book.GaveUp() == 1);

	// Long unheard of (its cell unloaded), it is forgotten, and a later death of it is judged afresh.
	book.Prune(t + 2h);
	CHECK(book.Size() == 0);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + 3h) == Verdict::kWait);
}

TEST("copy revive: the book holds MAX_COPIES at most")
{
	CopyRevive::Book book;
	const auto       t0 = Clock::now();
	for (std::uint32_t i = 0; i < CopyRevive::MAX_COPIES; ++i) {
		CHECK(book.Judge(0x00100000 + i, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kWait);
	}
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kNo);
	CHECK(book.Size() == CopyRevive::MAX_COPIES);
}
