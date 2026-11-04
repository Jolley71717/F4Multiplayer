// A hit on our stand-in in a friend's game (kPlayerDamaged) is taken when the NPC that struck it is here to see, and
// otherwise when the friend who reported it is near us (PlayerHits). Each test names what the players would see go
// wrong if it failed: an enemy a friend's game runs that can't hurt us at all.

#include "Test.h"

#include "net/PlayerHitRules.h"

using PlayerHits::Attacker;
using PlayerHits::Facts;
using PlayerHits::Judge;
using PlayerHits::Verdict;

TEST("player hits: a hit by an NPC here to see, or by a friend themselves, is taken")
{
	CHECK(Judge({ .byPlayer = false, .attacker = Attacker::kHere }) == Verdict::kTake);
	CHECK(Judge({ .byPlayer = false, .attacker = Attacker::kHere, .reporterApart = 50000.0f }) == Verdict::kTake);
	// Friendly fire names no NPC: taken wherever the friend is (they shot us from across the map).
	CHECK(Judge({ .byPlayer = true, .attacker = Attacker::kUnnamed }) == Verdict::kTake);
	CHECK(Judge({ .byPlayer = true, .attacker = Attacker::kUnnamed, .reporterApart = 50000.0f }) == Verdict::kTake);
}

TEST("player hits: a hit by an NPC not to be seen here is taken when the friend who reported it is near")
{
	// Our copy is a corpse (the session had it dead; the host's game brought it back with a cell reset): before, the
	// enemy the host saw fighting us never hurt us.
	CHECK(Judge({ .attacker = Attacker::kDead, .reporterApart = 800.0f }) == Verdict::kTakeUnseen);
	// Our copy stayed outside while the friend and the enemy went through a door, and we followed.
	CHECK(Judge({ .attacker = Attacker::kElsewhere, .reporterApart = 800.0f }) == Verdict::kTakeUnseen);
	// An NPC of a plugin our game lacks: the hit names nothing (FormTranslate).
	CHECK(Judge({ .attacker = Attacker::kUnnamed, .reporterApart = 800.0f }) == Verdict::kTakeUnseen);
	CHECK(Judge({ .attacker = Attacker::kNotHere, .reporterApart = PlayerHits::REPORTER_RANGE - 1.0f }) == Verdict::kTakeUnseen);
	CHECK(Judge({ .attacker = Attacker::kFar, .reporterApart = 100.0f }) == Verdict::kTakeUnseen);
}

TEST("player hits: a hit by an NPC not to be seen here is ignored when the friend is nowhere near")
{
	// Their enemy fights a stand-in nowhere near our player: damage from nowhere.
	CHECK(Judge({ .attacker = Attacker::kDead }) == Verdict::kIgnore);  // not in our cell or worldspace
	CHECK(Judge({ .attacker = Attacker::kNotHere, .reporterApart = PlayerHits::REPORTER_RANGE }) == Verdict::kIgnore);
	CHECK(Judge({ .attacker = Attacker::kUnnamed, .reporterApart = 100000.0f }) == Verdict::kIgnore);
	CHECK(Judge({ .attacker = Attacker::kElsewhere }) == Verdict::kIgnore);
}

TEST("player hits: every state of the attacker has a name for the log")
{
	for (const auto attacker : { Attacker::kUnnamed, Attacker::kNotHere, Attacker::kDead, Attacker::kElsewhere, Attacker::kFar, Attacker::kHere }) {
		CHECK(std::string_view{ PlayerHits::Name(attacker) } != "?");
	}
}
