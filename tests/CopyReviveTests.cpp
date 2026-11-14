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

	// Our copy dead, alive for the session as we know it, another game runs it and was heard from just now, no menu on
	// it: raise.
	constexpr Facts DEAD_HERE_ALIVE_THERE{ .deadHere = true, .deadForUs = false, .runsElsewhere = true, .heardAlive = true, .heardNow = true, .inMenu = false, .story = false };
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

TEST("copy revive: a copy due to be raised waits, without spending a try, until its runner's last state is within HEARD_NOW")
{
	CopyRevive::Book book;
	const auto       t0 = Clock::now();
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kWait);

	// Its runner's NPC began to fall there a moment ago: its last state is 1-3 s old, still heard (LOST_AFTER) but not now.
	// Raised now, it would be killed again as that death arrives.
	auto falling = DEAD_HERE_ALIVE_THERE;
	falling.heardNow = false;
	CHECK(book.Judge(RAIDER, falling, t0 + CopyRevive::DEAD_FOR) == Verdict::kWait);
	CHECK(book.Judge(RAIDER, falling, t0 + CopyRevive::DEAD_FOR + 1s) == Verdict::kWait);
	CHECK(book.HeldUnheard() == 2);
	CHECK(book.Raised() == 0);

	// Its death came (dead for us) and a game ran it again later: the wait starts over, nothing was spent.
	auto died = falling;
	died.deadForUs = true;
	CHECK(book.Judge(RAIDER, died, t0 + CopyRevive::DEAD_FOR + 2s) == Verdict::kNo);

	// Or a fresh state came after all (a hitch there, not a fall): raised at once, the wait already served, a first try.
	CopyRevive::Book hitch;
	CHECK(hitch.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0) == Verdict::kWait);
	CHECK(hitch.Judge(RAIDER, falling, t0 + CopyRevive::DEAD_FOR) == Verdict::kWait);
	CHECK(hitch.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t0 + CopyRevive::DEAD_FOR + 1s) == Verdict::kRaise);
	CHECK(hitch.Raised() == 1);
}

TEST("copy revive: a copy our player is looting is raised once the menu closes, and the wait spends none of its tries")
{
	CopyRevive::Book book;
	auto             t = Clock::now();
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t) == Verdict::kWait);
	auto looting = DEAD_HERE_ALIVE_THERE;
	looting.inMenu = true;
	// The container menu stays open on it well past DEAD_FOR and RETRY_AFTER: held each look, no try counted.
	for (auto at = t + CopyRevive::DEAD_FOR; at < t + CopyRevive::DEAD_FOR + 2 * CopyRevive::RETRY_AFTER; at += 1s) {
		CHECK(book.Judge(RAIDER, looting, at) == Verdict::kWait);
	}
	CHECK(book.Raised() == 0);
	CHECK(book.HeldInMenu() > 0);

	// Closed: raised at the next look.
	t += CopyRevive::DEAD_FOR + 2 * CopyRevive::RETRY_AFTER;
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t) == Verdict::kRaise);
	CHECK(book.Raised() == 1);

	// It dies again here and our player loots it again: the menu holds the next try too, and all TRIES are still there.
	CHECK(book.Judge(RAIDER, looting, t + CopyRevive::RETRY_AFTER) == Verdict::kWait);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + CopyRevive::RETRY_AFTER + 1s) == Verdict::kRaise);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + 2 * CopyRevive::RETRY_AFTER + 1s) == Verdict::kRaise);
	CHECK(book.Judge(RAIDER, DEAD_HERE_ALIVE_THERE, t + 3 * CopyRevive::RETRY_AFTER + 1s) == Verdict::kGiveUp);
	CHECK(book.Raised() == CopyRevive::TRIES);

	// A menu open on a copy that is not due changes nothing (no hold counted before DEAD_FOR).
	CopyRevive::Book fresh;
	const auto       held = fresh.HeldInMenu();
	CHECK(fresh.Judge(RAIDER, looting, t) == Verdict::kWait);
	CHECK(fresh.HeldInMenu() == held);
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
