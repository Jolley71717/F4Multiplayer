// Who runs each NPC and how often that changes (NpcHands.h, as NpcSync::ApplyOwners notes what the server names):
// a change within QUICK of the last is a quick one, a release and the claim after it count as the change they are,
// and nothing is remembered for good.

#include "Test.h"

#include "game/NpcHands.h"

using namespace std::chrono_literals;

namespace
{
	using NpcHands::Change;
	using NpcHands::Clock;

	constexpr std::uint32_t RAIDER = 0x0001F301;
	constexpr std::uint32_t GUARD = 0x00095E8A;
	constexpr std::uint32_t HOST = 1;
	constexpr std::uint32_t FRIEND = 2;
}

TEST("npc hands: two players shooting the same raider trade it every cooldown, and each trade is a quick change")
{
	NpcHands::Book book;
	const auto     t0 = Clock::now();
	CHECK(book.Note(RAIDER, HOST, t0).change == Change::kFirst);
	CHECK(book.Note(RAIDER, HOST, t0 + 1s).change == Change::kNone);  // named again (a claim answered to everyone)
	CHECK(book.Changes() == 0);

	// The friend's hit takes it over 5 s on (the server's cooldown), the host's hit 5 s after that.
	const auto first = book.Note(RAIDER, FRIEND, t0 + 5s);
	CHECK(first.change == Change::kQuick);
	CHECK(first.from == HOST);
	CHECK(first.since == 5s);
	const auto second = book.Note(RAIDER, HOST, t0 + 10s);
	CHECK(second.change == Change::kQuick);
	CHECK(second.from == FRIEND);
	CHECK(book.Changes() == 2);
	CHECK(book.Quick() == 2);

	// A change after a quiet spell is a plain one.
	const auto later = book.Note(RAIDER, FRIEND, t0 + 10s + NpcHands::QUICK);
	CHECK(later.change == Change::kChanged);
	CHECK(later.since == NpcHands::QUICK);
	CHECK(book.Changes() == 3);
	CHECK(book.Quick() == 2);
}

TEST("npc hands: a hand-off is a release and a claim, and counts as one change; a release long past counts for nothing")
{
	NpcHands::Book book;
	const auto     t0 = Clock::now();
	book.Note(GUARD, HOST, t0);
	// The host's game lets go (it is far, the friend near); the friend's game claims it within the second.
	const auto freed = book.Note(GUARD, 0, t0 + 30s);
	CHECK(freed.change == Change::kFreed);
	CHECK(freed.from == HOST);
	CHECK(book.Note(GUARD, 0, t0 + 30s).change == Change::kNone);
	const auto claimed = book.Note(GUARD, FRIEND, t0 + 30s + 400ms);
	CHECK(claimed.change == Change::kQuick);
	CHECK(claimed.from == 0);
	CHECK(claimed.since == 400ms);
	CHECK(book.Changes() == 1);
	CHECK(book.Quick() == 1);

	// Let go and left alone past QUICK, then run again: that is a first runner again, not a change.
	book.Note(GUARD, 0, t0 + 60s);
	CHECK(book.Tracked() == 1);
	book.Expire(t0 + 60s + NpcHands::QUICK);
	CHECK(book.Tracked() == 0);
	CHECK(book.Note(GUARD, HOST, t0 + 60s + NpcHands::QUICK).change == Change::kFirst);
	CHECK(book.Changes() == 1);
	// One still running is never expired.
	book.Expire(t0 + 600s);
	CHECK(book.Tracked() == 1);

	// Nobody for one never run is nothing.
	CHECK(book.Note(RAIDER, 0, t0).change == Change::kNone);
	CHECK(book.Tracked() == 1);
}

TEST("npc hands: no more than MAX_TRACKED NPCs are remembered, and Clear forgets them all")
{
	NpcHands::Book book;
	const auto     t0 = Clock::now();
	for (std::uint32_t i = 0; i < NpcHands::MAX_TRACKED; ++i) {
		CHECK(book.Note(0x00100000 + i, HOST, t0).change == Change::kFirst);
	}
	CHECK(book.Note(RAIDER, HOST, t0).change == Change::kNone);
	CHECK(book.Tracked() == NpcHands::MAX_TRACKED);
	// One already in the book still has its changes counted.
	CHECK(book.Note(0x00100000, FRIEND, t0 + 1s).change == Change::kQuick);
	book.Clear();
	CHECK(book.Tracked() == 0);
	CHECK(book.Changes() == 0);
	CHECK(book.Quick() == 0);
	CHECK(book.Note(RAIDER, HOST, t0).change == Change::kFirst);
}
