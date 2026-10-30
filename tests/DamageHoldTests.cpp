// A friend's damage on an NPC our game runs that can't be taken off yet is held and tried again, not dropped
// (DamageHold); what can never be taken off is dropped with a reason. Each test names what the players would see
// go wrong if it failed: the friend's shot healed the NPC (their copy lost the health, ours didn't, and our next health
// report set the copy back up), or only the game that ran the NPC could hurt it.

#include "Test.h"

#include "game/DamageHold.h"
#include "game/LogPace.h"

#include <chrono>
#include <cmath>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using DamageHold::Decide;
using DamageHold::Facts;
using DamageHold::Verdict;
using DamageHold::Why;

namespace
{
	using Clock = DamageHold::Clock;

	constexpr std::uint32_t RAIDER = 0x0001F101;
	constexpr std::uint32_t RAIDER2 = 0x0001F102;
	constexpr std::uint32_t US = 7;
	constexpr std::uint32_t FRIEND = 9;

	// The NPC we run, loaded and alive here, as it is in the usual case.
	Facts Ours()
	{
		return { .known = true, .inWorld = true, .deadHere = false, .ledgerDead = false, .local = US, .owner = US, .loaded = true, .hasHealth = true };
	}
}

TEST("damage hold: a friend's damage on the NPC we run is taken off at once")
{
	const auto decision = Decide(Ours());
	CHECK(decision.verdict == Verdict::kApply);
	CHECK(decision.why == Why::kApplied);
}

TEST("damage hold: damage that can't be taken off yet is held: a loading screen, the server's word on its way, the NPC not loaded here")
{
	// The host on a loading screen (a fast travel, a death's reload) while the joiner keeps shooting its copy: before,
	// every shot was dropped and the host's first health report after the load healed them all back.
	auto loading = Ours();
	loading.inWorld = false;
	loading.loaded = false;
	loading.hasHealth = false;
	CHECK(Decide(loading).verdict == Verdict::kHold);
	CHECK(Decide(loading).why == Why::kNotInWorld);

	// A takeover's claim leaves a frame ahead of the hit's damage; after a load our table is empty until the owners come.
	auto unowned = Ours();
	unowned.owner = 0;
	CHECK(Decide(unowned).verdict == Verdict::kHold);
	CHECK(Decide(unowned).why == Why::kNoOwnerYet);

	// The joiner has the NPC's cell loaded, ours hasn't drawn it yet.
	auto unloaded = Ours();
	unloaded.loaded = false;
	unloaded.hasHealth = false;
	CHECK(Decide(unloaded).verdict == Verdict::kHold);
	CHECK(Decide(unloaded).why == Why::kNotLoaded);
}

TEST("damage hold: damage that can never be taken off is dropped, each for its reason")
{
	auto nobody = Ours();
	nobody.known = false;
	CHECK(Decide(nobody).verdict == Verdict::kDrop);
	CHECK(Decide(nobody).why == Why::kNoActor);

	auto corpse = Ours();
	corpse.deadHere = true;
	CHECK(Decide(corpse).why == Why::kDeadHere);

	auto ledger = Ours();
	ledger.ledgerDead = true;
	CHECK(Decide(ledger).why == Why::kLedgerDead);

	// Another game runs it: that game takes the damage off (the server tells everyone but the sender).
	auto theirs = Ours();
	theirs.owner = FRIEND;
	CHECK(Decide(theirs).verdict == Verdict::kDrop);
	CHECK(Decide(theirs).why == Why::kNotOurs);

	auto outOfSession = Ours();
	outOfSession.local = 0;
	outOfSession.owner = 0;
	CHECK(Decide(outOfSession).why == Why::kNotOurs);

	auto noHealth = Ours();
	noHealth.hasHealth = false;
	CHECK(Decide(noHealth).why == Why::kNoHealth);

	// Out of the world nothing of the actor is read: what can't be read doesn't decide anything.
	auto loadingCorpse = Ours();
	loadingCorpse.inWorld = false;
	loadingCorpse.deadHere = true;
	CHECK(Decide(loadingCorpse).verdict == Verdict::kHold);
}

TEST("damage hold: the book holds each hit on its own, with its number and whose it is, and gives each back to be applied once it can be")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	CHECK(book.Hold({ RAIDER, 0.10f, Why::kNotLoaded, t0, 1, FRIEND }));
	CHECK(book.Hold({ RAIDER, 0.15f, Why::kNoOwnerYet, t0 + 100ms, 2, FRIEND }));
	CHECK(book.Size() == 2);
	const auto held = book.HeldFor(RAIDER);
	REQUIRE(held.size() == 2);
	CHECK(std::abs(held[0].fraction + held[1].fraction - 0.25f) < 0.0001f);
	CHECK(held[0].why == Why::kNotLoaded);
	CHECK(held[1].why == Why::kNoOwnerYet);
	CHECK(held[0].since == t0);  // oldest first
	CHECK(book.HeldFor(RAIDER2).empty());

	// Tried again: still held (the NPC isn't loaded), then applied, each hit with its number so each can be answered.
	int tries = 0;
	book.Retry([&](DamageHold::Held& a_held) {
		++tries;
		a_held.why = Why::kNotLoaded;
		return false;
	});
	CHECK(tries == 2);
	CHECK(book.Size() == 2);
	CHECK(book.HeldFor(RAIDER)[1].why == Why::kNotLoaded);
	float                      applied = 0.0f;
	std::vector<std::uint32_t> answered;
	book.Retry([&](DamageHold::Held& a_held) {
		applied += a_held.fraction;
		answered.push_back(a_held.seq);
		CHECK(a_held.from == FRIEND);
		return true;
	});
	CHECK(std::abs(applied - 0.25f) < 0.0001f);
	CHECK((answered == std::vector<std::uint32_t>{ 1, 2 }));
	CHECK(book.Size() == 0);
	CHECK(book.HeldCount() == 2);
	CHECK(book.Expired() == 0);
}

TEST("damage hold: what is held too long is refused as such, with its number so the friend's copy gets it back; a loading screen's time doesn't count")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	book.Hold({ RAIDER, 0.2f, Why::kNotInWorld, t0, 5, FRIEND });
	book.Hold({ RAIDER2, 0.3f, Why::kNotLoaded, t0 + 1s, 6, FRIEND });
	CHECK(book.Expire(t0 + DamageHold::HOLD - 1ms).empty());
	// The world comes back after a long loading screen: the hold starts afresh.
	book.BackInWorld(t0 + DamageHold::HOLD + 10s);
	CHECK(book.Expire(t0 + DamageHold::HOLD + 11s).empty());
	CHECK(book.Size() == 2);
	// In the world for HOLD with no chance to apply it: taken out to be refused, each with why it waited and its number.
	const auto gone = book.Expire(t0 + DamageHold::HOLD + 10s + DamageHold::HOLD);
	REQUIRE(gone.size() == 2);
	CHECK(book.Size() == 0);
	CHECK(book.Expired() == 2);
	for (const auto& entry : gone) {
		CHECK(entry.id == RAIDER || entry.id == RAIDER2);
		CHECK(entry.why == (entry.id == RAIDER ? Why::kNotInWorld : Why::kNotLoaded));
		CHECK(entry.seq == (entry.id == RAIDER ? 5u : 6u));
		CHECK(entry.from == FRIEND);
		CHECK(DamageHold::Answer(entry.why) == Protocol::DamageAnswer::Why::kHeldTooLong);
	}
}

TEST("damage hold: the book holds at most MAX_HELD hits, and says so")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	for (std::uint32_t i = 0; i < DamageHold::MAX_HELD; ++i) {
		CHECK(book.Hold({ 0x00010000 + i, 0.1f, Why::kNotLoaded, t0, i + 1, FRIEND }));
	}
	CHECK(!book.Hold({ RAIDER, 0.1f, Why::kNotLoaded, t0, 999, FRIEND }));
	CHECK(!book.Hold({ 0x00010000, 0.1f, Why::kNotLoaded, t0, 1000, FRIEND }));  // nor one more for an NPC held for already: each hit is one
	CHECK(book.Size() == DamageHold::MAX_HELD);
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("damage hold: every reason has a name for the log, and an answer for the friend whose hit it was")
{
	using Out = Protocol::DamageAnswer::Why;
	for (const auto why : { Why::kApplied, Why::kNotInWorld, Why::kNoOwnerYet, Why::kNotLoaded, Why::kNoActor, Why::kDeadHere, Why::kLedgerDead, Why::kNotOurs, Why::kNoHealth, Why::kExpired }) {
		CHECK(std::string_view{ DamageHold::Name(why) } != "?");
		CHECK(std::string_view{ Protocol::Name(DamageHold::Answer(why)) } != "?");
	}
	// Applied is applied; a drop names its reason; a hold's reason answers "held too long" (it is answered only at the
	// end of the hold); another game running it is for the server to send on.
	CHECK(DamageHold::Answer(Why::kApplied) == Out::kApplied);
	CHECK(DamageHold::Answer(Why::kDeadHere) == Out::kDeadThere);
	CHECK(DamageHold::Answer(Why::kLedgerDead) == Out::kDeadThere);
	CHECK(DamageHold::Answer(Why::kNoActor) == Out::kNoActorThere);
	CHECK(DamageHold::Answer(Why::kNoHealth) == Out::kNoHealthThere);
	CHECK(DamageHold::Answer(Why::kNotOurs) == Out::kNotTheRunner);
	for (const auto why : { Why::kNotInWorld, Why::kNoOwnerYet, Why::kNotLoaded, Why::kExpired }) {
		CHECK(DamageHold::Answer(why) == Out::kHeldTooLong);
	}
}

TEST("log pace: one line per interval, counting what went by unlogged")
{
	LogPace::Pace pace;
	const auto    t0 = Clock::now();
	CHECK(pace.Due(t0));  // the first is always logged
	CHECK(pace.Since() == 1);
	CHECK(!pace.Due(t0 + 1s));
	CHECK(!pace.Due(t0 + 2s));
	CHECK(pace.Due(t0 + LogPace::INTERVAL));
	CHECK(pace.Since() == 3);  // the two unlogged and this one
	CHECK(pace.Since() == 0);
	CHECK(pace.Count() == 4);
	CHECK(pace.Due(t0 + LogPace::INTERVAL + 500ms, 100ms));  // a shorter interval
}
