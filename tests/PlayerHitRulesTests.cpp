// A hit on our stand-in in a friend's game (kPlayerDamaged) is taken when the NPC that struck it is here to see, and
// otherwise when the friend who reported it is near us (PlayerHits). Each test names what the players would see go
// wrong if it failed: an enemy a friend's game runs that can't hurt us at all.

#include "Test.h"

#include "net/PlayerHitRules.h"

#include <limits>
#include <string_view>

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

TEST("player hits: an NPC's hit with its copy here is made a real hit, anything else is taken as it came")
{
	using Protocol::HitKind::kExplosion;
	using Protocol::HitKind::kMelee;
	using Protocol::HitKind::kNone;
	using Protocol::HitKind::kRanged;
	// Our perks, armor, limbs and radiation apply only when our game makes the hit itself, from our copy of the NPC.
	CHECK(PlayerHits::MakeReal(Verdict::kTake, false, kMelee));
	CHECK(PlayerHits::MakeReal(Verdict::kTake, false, kRanged));
	CHECK(PlayerHits::MakeReal(Verdict::kTake, false, kExplosion));
	// No copy here to strike with: the friend was near, and the hit is taken as their stand-in took it.
	CHECK(!PlayerHits::MakeReal(Verdict::kTakeUnseen, false, kMelee));
	CHECK(!PlayerHits::MakeReal(Verdict::kIgnore, false, kMelee));
	// Friendly fire is the shooter's game's (its rules, its numbers), and a hit that says no kind has nothing to make.
	CHECK(!PlayerHits::MakeReal(Verdict::kTake, true, kNone));
	CHECK(!PlayerHits::MakeReal(Verdict::kTake, true, kRanged));
	CHECK(!PlayerHits::MakeReal(Verdict::kTake, false, kNone));
	CHECK(!PlayerHits::MakeReal(Verdict::kTake, false, Protocol::HitKind::kMax + 1));
}

TEST("player hits: what a hit was is read from its flags")
{
	using namespace PlayerHits::HitFlag;
	CHECK(PlayerHits::KindOf(kMeleeAttack) == Protocol::HitKind::kMelee);
	CHECK(PlayerHits::KindOf(kMeleeAttack | kPowerAttack) == Protocol::HitKind::kMelee);
	CHECK(PlayerHits::KindOf(0) == Protocol::HitKind::kRanged);        // a bullet, a laser, a spit
	CHECK(PlayerHits::KindOf(1u << 3) == Protocol::HitKind::kRanged);  // a critical shot
	CHECK(PlayerHits::KindOf(kExplosion) == Protocol::HitKind::kExplosion);
	CHECK(PlayerHits::KindOf(kExplosion | kMeleeAttack) == Protocol::HitKind::kExplosion);  // an explosive punch's blast
	// Only how the attack was made goes with it; the rest is the stand-in's side (its critical, its block).
	CHECK((kCarried & (1u << 3)) == 0u);
	CHECK((kCarried & kPowerAttack) != 0u);
	CHECK((kCarried & kBash) != 0u);
}

TEST("player hits: a shotgun's pellet takes its share of the shot, as the engine shares a projectile's")
{
	CHECK(PlayerHits::PelletShare(0) == 1.0f);
	CHECK(PlayerHits::PelletShare(1) == 1.0f);
	CHECK(PlayerHits::PelletShare(8) == 0.125f);
	// Eight pellets that each took the whole shot would hit eight times as hard as the gun.
	CHECK(PlayerHits::PelletShare(8) * 8.0f == 1.0f);
}

TEST("player hits: an explosion's blow is made from its base's damage and the multiplier the blow had")
{
	// A frag grenade (base 100) whose blow struck at 60: the base with 0.6, so its typed damage falls off as well.
	auto blast = PlayerHits::BlastFrom(60.0f, 100.0f);
	CHECK(blast.damage == 100.0f);
	CHECK(blast.mult == 0.6f);
	// A strengthened one (the explosion's own multiplier above 1) keeps it, to a bound.
	blast = PlayerHits::BlastFrom(150.0f, 100.0f);
	CHECK(blast.mult == 1.5f);
	CHECK(PlayerHits::BlastFrom(1.0e6f, 1.0f).mult == PlayerHits::MAX_BLAST_MULT);
	// A base with no damage of its own takes the raw damage as it is.
	blast = PlayerHits::BlastFrom(25.0f, 0.0f);
	CHECK(blast.damage == 25.0f);
	CHECK(blast.mult == 1.0f);
	// No damage: nothing.
	CHECK(PlayerHits::BlastFrom(0.0f, 100.0f).mult == 0.0f);
	CHECK(PlayerHits::BlastFrom(std::numeric_limits<float>::quiet_NaN(), 100.0f).damage == 0.0f);
	// The raw blow is what struck the stand-in before its armor's share.
	CHECK(PlayerHits::RawPhysical(30.0f, 70.0f) == 100.0f);
	CHECK(PlayerHits::RawPhysical(-5.0f, 0.0f) == 0.0f);
	CHECK(PlayerHits::RawPhysical(std::numeric_limits<float>::infinity(), 4.0f) == 4.0f);
}
