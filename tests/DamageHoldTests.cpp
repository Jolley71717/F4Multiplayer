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

TEST("damage hold: the book sums what is held for an NPC and gives it back to be applied once it can be")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	CHECK(book.Hold(RAIDER, 0.10f, Why::kNotLoaded, t0));
	CHECK(book.Hold(RAIDER, 0.15f, Why::kNoOwnerYet, t0 + 100ms));
	CHECK(book.Size() == 1);
	const auto held = book.HeldFor(RAIDER);
	REQUIRE(held);
	CHECK(std::abs(held->fraction - 0.25f) < 0.0001f);
	CHECK(held->why == Why::kNoOwnerYet);
	CHECK(held->since == t0);  // held since the first

	// Tried again: still held (the NPC isn't loaded), then applied.
	int tries = 0;
	book.Retry([&](DamageHold::Held& a_held) {
		++tries;
		a_held.why = Why::kNotLoaded;
		return false;
	});
	CHECK(tries == 1);
	CHECK(book.Size() == 1);
	CHECK(book.HeldFor(RAIDER)->why == Why::kNotLoaded);
	float applied = 0.0f;
	book.Retry([&](DamageHold::Held& a_held) {
		applied = a_held.fraction;
		return true;
	});
	CHECK(std::abs(applied - 0.25f) < 0.0001f);
	CHECK(book.Size() == 0);
	CHECK(book.HeldCount() == 1);
	CHECK(book.Expired() == 0);
}

TEST("damage hold: what is held too long is dropped, but a loading screen's time doesn't count")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	book.Hold(RAIDER, 0.2f, Why::kNotInWorld, t0);
	book.Hold(RAIDER2, 0.3f, Why::kNotLoaded, t0 + 1s);
	CHECK(book.Expire(t0 + DamageHold::HOLD - 1ms).empty());
	// The world comes back after a long loading screen: the hold starts afresh.
	book.BackInWorld(t0 + DamageHold::HOLD + 10s);
	CHECK(book.Expire(t0 + DamageHold::HOLD + 11s).empty());
	CHECK(book.Size() == 2);
	// In the world for HOLD with no chance to apply it: dropped, and said why it waited.
	const auto gone = book.Expire(t0 + DamageHold::HOLD + 10s + DamageHold::HOLD);
	REQUIRE(gone.size() == 2);
	CHECK(book.Size() == 0);
	CHECK(book.Expired() == 2);
	for (const auto& entry : gone) {
		CHECK(entry.id == RAIDER || entry.id == RAIDER2);
		CHECK(entry.why == (entry.id == RAIDER ? Why::kNotInWorld : Why::kNotLoaded));
	}
}

TEST("damage hold: the book holds for at most MAX_HELD NPCs, and says so")
{
	DamageHold::Book book;
	const auto       t0 = Clock::now();
	for (std::uint32_t i = 0; i < DamageHold::MAX_HELD; ++i) {
		CHECK(book.Hold(0x00010000 + i, 0.1f, Why::kNotLoaded, t0));
	}
	CHECK(!book.Hold(RAIDER, 0.1f, Why::kNotLoaded, t0));
	CHECK(book.Hold(0x00010000, 0.1f, Why::kNotLoaded, t0));  // more for one held already always fits
	CHECK(book.Size() == DamageHold::MAX_HELD);
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("damage hold: every reason has a name for the log")
{
	for (const auto why : { Why::kApplied, Why::kNotInWorld, Why::kNoOwnerYet, Why::kNotLoaded, Why::kNoActor, Why::kDeadHere, Why::kLedgerDead, Why::kNotOurs, Why::kNoHealth, Why::kExpired }) {
		CHECK(std::string_view{ DamageHold::Name(why) } != "?");
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
