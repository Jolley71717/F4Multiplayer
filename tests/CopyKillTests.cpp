// A death another player's game reports lands on our reference of that NPC (CopyKill.h, as Spawns::ApplyDeath and its
// frame ask): a protected one has its protection lifted for the console's kill (which names no killer), and the death
// waits while it can't land, is tried again while it stands, and is given up after TRIES.

#include "Test.h"

#include "game/CopyKill.h"

using namespace std::chrono_literals;

namespace
{
	using CopyKill::Clock;
	using CopyKill::Facts;
	using CopyKill::Verdict;

	constexpr std::uint32_t COURSER_COPY = 0xFF0010E8;  // the VM's copy of the host's RRR07 courser, held by its protected alias
	constexpr std::uint32_t RAIDER_COPY = 0xFF001104;

	constexpr Facts STANDING{ .there = true, .deadHere = false, .inWorld = true, .loaded = true };
	constexpr Facts DEAD{ .there = true, .deadHere = true, .inWorld = true, .loaded = true };
	constexpr Facts GONE{ .there = false, .deadHere = false, .inWorld = true, .loaded = false };
}

TEST("copy kill: a protected copy has its protection lifted for the kill; an essential one never (it is only knocked down)")
{
	// RRR07's CourserTarget is a protected alias: the console's bare kill left our copy standing (rig, 2026-11-12).
	CHECK(CopyKill::LiftsProtection(true, false));
	CHECK(!CopyKill::LiftsProtection(true, true));
	CHECK(!CopyKill::LiftsProtection(false, false));
	CHECK(!CopyKill::LiftsProtection(false, true));
}

TEST("copy kill: a data-file NPC's reported death lifts its protection only for the reporter's own kill")
{
	// WorldSync: our reference is our own story's. The reporter's player killed it (as the game lets a player): lifted.
	CHECK(CopyKill::ForReportedDeath(true, false, true) == CopyKill::Protection::kLift);
	// Killed by an NPC there, or a death the session replays (nobody's kill): it stands here, protected as the game keeps it.
	CHECK(CopyKill::ForReportedDeath(true, false, false) == CopyKill::Protection::kKept);
	// Not protected: killed as it is, whoever killed it there. Essential: never lifted (WorldSync holds those first).
	CHECK(CopyKill::ForReportedDeath(false, false, false) == CopyKill::Protection::kNone);
	CHECK(CopyKill::ForReportedDeath(false, false, true) == CopyKill::Protection::kNone);
	CHECK(CopyKill::ForReportedDeath(true, true, true) == CopyKill::Protection::kNone);
	CHECK(CopyKill::ForReportedDeath(true, true, false) == CopyKill::Protection::kNone);
}

TEST("copy kill: a reported death is tried at once, and done once our copy lies dead")
{
	CopyKill::Book book;
	const auto     t0 = Clock::now();
	CHECK(book.Judge(COURSER_COPY, STANDING, t0) == Verdict::kNone);  // no death reported for it
	book.Add(COURSER_COPY);
	CHECK(book.Judge(COURSER_COPY, STANDING, t0) == Verdict::kKill);
	CHECK(book.Tries(COURSER_COPY) == 1);
	CHECK(book.Judge(COURSER_COPY, DEAD, t0 + 10ms) == Verdict::kDone);
	CHECK(book.Size() == 0);
	CHECK(book.Judge(COURSER_COPY, STANDING, t0 + 1s) == Verdict::kNone);
}

TEST("copy kill: a copy the kill left standing is killed again after RETRY_AFTER, and given up after TRIES")
{
	CopyKill::Book book;
	auto           t = Clock::now();
	book.Add(COURSER_COPY);
	for (std::uint32_t i = 1; i <= CopyKill::TRIES; ++i) {
		CHECK(book.Judge(COURSER_COPY, STANDING, t) == Verdict::kKill);
		CHECK(book.Tries(COURSER_COPY) == i);
		CHECK(book.Judge(COURSER_COPY, STANDING, t + CopyKill::RETRY_AFTER - 1ms) == Verdict::kWait);
		t += CopyKill::RETRY_AFTER;
	}
	// The last kill had its RETRY_AFTER to land too.
	CHECK(book.Judge(COURSER_COPY, STANDING, t) == Verdict::kGiveUp);
	CHECK(book.Size() == 0);
	CHECK(book.Judge(COURSER_COPY, STANDING, t + 1h) == Verdict::kNone);
}

TEST("copy kill: a death that comes in a loading screen, or before our copy has its 3D, waits for it and counts no try")
{
	// Before, the report marked the spawn dead at once and the kill was skipped out of the world: lost for good.
	CopyKill::Book book;
	const auto     t0 = Clock::now();
	book.Add(RAIDER_COPY);
	CHECK(book.Judge(RAIDER_COPY, Facts{ .there = true, .deadHere = false, .inWorld = false, .loaded = true }, t0) == Verdict::kWait);
	CHECK(book.Judge(RAIDER_COPY, Facts{ .there = true, .deadHere = false, .inWorld = true, .loaded = false }, t0 + 1min) == Verdict::kWait);
	CHECK(book.Tries(RAIDER_COPY) == 0);
	CHECK(book.Judge(RAIDER_COPY, STANDING, t0 + 2min) == Verdict::kKill);
	CHECK(book.Tries(RAIDER_COPY) == 1);
}

TEST("copy kill: a copy that is gone (deleted, or its cell left) is forgotten; a death told again keeps its tries")
{
	CopyKill::Book book;
	const auto     t0 = Clock::now();
	book.Add(RAIDER_COPY);
	CHECK(book.Judge(RAIDER_COPY, STANDING, t0) == Verdict::kKill);
	book.Add(RAIDER_COPY);  // told again: the same death
	CHECK(book.Tries(RAIDER_COPY) == 1);
	CHECK(book.Judge(RAIDER_COPY, STANDING, t0 + 1ms) == Verdict::kWait);
	CHECK(book.Judge(RAIDER_COPY, GONE, t0 + 1s) == Verdict::kDone);
	CHECK(book.Size() == 0);
	book.Add(0);  // no copy placed: nothing to wait for
	CHECK(book.Size() == 0);
}

TEST("copy kill: the book holds MAX deaths at most")
{
	CopyKill::Book book;
	for (std::uint32_t i = 0; i < CopyKill::MAX + 10; ++i) {
		book.Add(0xFF100000 + i);
	}
	CHECK(book.Size() == CopyKill::MAX);
	book.Clear();
	CHECK(book.Size() == 0);
}
