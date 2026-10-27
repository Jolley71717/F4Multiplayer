// The ledger of the ghosts the mod sets (GhostLedger): what WeaponFire does each frame with a ghost of ours until the
// engine reads the copy as no ghost again, and what a save keeps of it. Each test names what the players would see go
// wrong if it failed: a copy left a ghost for good, which the player's shots pass through.

#include "Test.h"

#include "game/GhostLedger.h"

using GhostLedger::Book;
using GhostLedger::Seen;
using GhostLedger::Step;

namespace
{
	constexpr std::uint32_t RAIDER = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t SPAWN_COPY = 0xFF000F8B;  // our copy of a friend's spawn: a form id of our game's own

	// The ghost is on, the copy loaded, no shield wants it.
	constexpr Seen ON{ .loaded = true, .ghost = true };
	constexpr Seen OFF{ .loaded = true, .ghost = false };
	constexpr Seen UNLOADED{ .loaded = false, .ghost = true };
	constexpr Seen SHIELDED{ .loaded = true, .ghost = true, .shielded = true };
}

TEST("ghost ledger: a ghost we set is written down until the copy reads no ghost, whatever runs it, and the console's command is sent again while it stays")
{
	Book book;
	CHECK(book.Add(RAIDER, 100));
	CHECK(!book.Add(RAIDER, 101));  // once
	CHECK(book.Contains(RAIDER));
	CHECK(book.Size() == 1);

	// While the shield wants it, nothing. The shield ends: setghost 0 goes out (the caller's), booked as sent.
	CHECK(book.Check(RAIDER, SHIELDED, 110) == Step::kWait);
	CHECK(book.Check(RAIDER, ON, 120) == Step::kClear);
	book.Sent(RAIDER, 120);
	CHECK(book.Tries(RAIDER) == 1);

	// The command lands a frame late: a frame on the ghost still reads on, and it is not sent again yet.
	CHECK(book.Check(RAIDER, ON, 121) == Step::kWait);
	CHECK(book.Check(RAIDER, ON, 125) == Step::kWait);
	// Still on after the gap (a setghost 1 that landed after the setghost 0): sent again.
	CHECK(book.Check(RAIDER, ON, 120 + GhostLedger::RETRY_AFTER) == Step::kClear);
	book.Sent(RAIDER, 120 + GhostLedger::RETRY_AFTER);
	CHECK(book.Tries(RAIDER) == 2);

	// Off at last: forgotten, counted cleared.
	CHECK(book.Check(RAIDER, OFF, 130) == Step::kDone);
	CHECK(!book.Contains(RAIDER));
	CHECK(book.Count().set == 1);
	CHECK(book.Count().cleared == 1);
	CHECK(book.Count().stuck == 0);
	CHECK(book.Check(RAIDER, OFF, 131) == Step::kDone);  // nothing written down: nothing to do
}

TEST("ghost ledger: a copy not loaded as its ghost is to come off waits, and has the command sent as soon as it is loaded again")
{
	// A door, a fast travel, a despawn inside the shield's second: before, its setghost 0 was never sent again once the
	// run moved on, and the copy came back from the save a ghost for good.
	Book book;
	book.Add(RAIDER, 100);
	for (std::uint64_t frame = 101; frame < 100000; frame += 997) {
		CHECK(book.Check(RAIDER, UNLOADED, frame) == Step::kWait);
	}
	CHECK(book.Contains(RAIDER));
	CHECK(book.Tries(RAIDER) == 0);
	CHECK(book.Check(RAIDER, ON, 100000) == Step::kClear);  // loaded again, a ghost still
	book.Sent(RAIDER, 100000);
	CHECK(book.Check(RAIDER, OFF, 100001) == Step::kDone);
	CHECK(book.Size() == 0);
}

TEST("ghost ledger: a ghost the engine will not let go of is sent setghost 0 again at a growing gap, never more than five seconds, and counts as stuck until it goes")
{
	// Before, three tries and then nothing: the copy stayed a ghost for the rest of the run, and the save kept it.
	Book book;
	book.Add(RAIDER, 0);
	std::uint64_t frame = 0;
	std::uint32_t sent = 0;
	for (int i = 0; i < 40; ++i) {
		const auto step = book.Check(RAIDER, ON, frame);
		if (step == Step::kClear) {
			book.Sent(RAIDER, frame);
			++sent;
			// The gap after the n-th command: 6, 12, 24, ... frames, up to RETRY_MOST.
			const auto gap = Book::RetryAfter(book.Tries(RAIDER));
			CHECK(gap == (std::min)(GhostLedger::RETRY_AFTER << (std::min)(book.Tries(RAIDER) - 1, 10), GhostLedger::RETRY_MOST));
			for (std::uint32_t f = 1; f < gap; ++f) {
				CHECK(book.Check(RAIDER, ON, frame + f) == Step::kWait);
			}
			frame += gap;
		} else {
			++frame;
		}
	}
	CHECK(sent >= 10);
	CHECK(sent == GhostLedger::GIVE_UP_AFTER);  // and no more: a ghost put back that often is a script's
	CHECK(Book::RetryAfter(1) == GhostLedger::RETRY_AFTER);
	CHECK(Book::RetryAfter(2) == 2 * GhostLedger::RETRY_AFTER);
	CHECK(Book::RetryAfter(20) == GhostLedger::RETRY_MOST);
	CHECK(Book::RetryAfter(255) == GhostLedger::RETRY_MOST);
	CHECK(book.Count().stuck == 1);  // once, at STUCK_AFTER tries
	CHECK(book.Count().unstuck == 0);
	CHECK(book.Contains(RAIDER));

	// It goes at last: cleared after all.
	CHECK(book.Check(RAIDER, OFF, frame) == Step::kDone);
	CHECK(book.Count().unstuck == 1);
	CHECK(book.Count().cleared == 1);
}

TEST("ghost ledger: a ghost a script puts back every time is given up on after sixteen commands, kept as ours and in the save, and forgotten once it reads off by itself")
{
	// A command every five seconds for six hours would be a log full of the same line and a fight with that script.
	Book          book;
	std::uint64_t frame = 0;
	book.Add(RAIDER, frame);
	for (std::uint8_t i = 0; i < GhostLedger::GIVE_UP_AFTER; ++i) {
		while (book.Check(RAIDER, ON, frame) != Step::kClear) {
			++frame;
		}
		book.Sent(RAIDER, frame);
	}
	CHECK(book.Tries(RAIDER) == GhostLedger::GIVE_UP_AFTER);
	CHECK(book.GivenUp(RAIDER));
	CHECK(book.Count().gaveUp == 1);
	CHECK(frame < 60 * 60 * 2);  // inside two minutes at 60 fps
	for (std::uint64_t f = frame; f < frame + 60 * 60 * 60 * 6; f += 60) {
		CHECK(book.Check(RAIDER, ON, f) == Step::kWait);  // six hours: not one command more
	}
	CHECK(book.Contains(RAIDER));
	CHECK((book.Saved() == std::vector<std::uint32_t>{ RAIDER }));  // still ours, still saved: a load tries again
	CHECK(book.Check(RAIDER, OFF, frame + 1) == Step::kDone);  // the script let go: forgotten as any
	CHECK(book.Count().unstuck == 1);
	CHECK(!book.GivenUp(RAIDER));
}

TEST("ghost ledger: a save keeps the plugins' references we ghosted and not a spawn copy's runtime id, and a load writes them down again as ghosts the save brought")
{
	Book book;
	book.Add(RAIDER, 10);
	book.Add(SPAWN_COPY, 10);
	book.Add(RAIDER_2, 11);
	CHECK((book.Saved() == std::vector<std::uint32_t>{ RAIDER, RAIDER_2 }));
	const auto bytes = GhostLedger::Encode(book.Saved());
	CHECK(bytes.size() == 2 + 2 + 2 * 4);
	const auto read = GhostLedger::Decode(bytes);
	REQUIRE(read);
	CHECK((*read == std::vector<std::uint32_t>{ RAIDER, RAIDER_2 }));

	// The loaded game: the entries come back as ghosts the save brought, and come off as each copy loads, whoever runs
	// the NPC (the ledger never asks).
	Book loaded;
	CHECK(loaded.Restore(*read, 5) == 2);
	CHECK(loaded.Count().fromSaves == 2);
	CHECK(loaded.FromSave(RAIDER));
	CHECK(loaded.Check(RAIDER, UNLOADED, 6) == Step::kWait);
	CHECK(loaded.Check(RAIDER, ON, 300) == Step::kClear);
	loaded.Sent(RAIDER, 300);
	CHECK(loaded.Check(RAIDER, OFF, 301) == Step::kDone);
	CHECK(loaded.Check(RAIDER_2, OFF, 301) == Step::kDone);  // the save's flag was off already (hidden from it): nothing to send
	CHECK(loaded.Size() == 0);
	CHECK(loaded.Count().cleared == 2);

	// A runtime id in a record, or an unreadable record, is refused whole; nothing is written for an empty ledger.
	CHECK(!GhostLedger::Decode(std::vector<std::uint8_t>{ 1, 0, 1, 0, 0x8B, 0x0F, 0x00, 0xFF }));  // a runtime id
	CHECK(!GhostLedger::Decode(std::vector<std::uint8_t>{ 2, 0, 1, 0, 0x01, 0xA0, 0x02, 0x00 }));  // another version
	CHECK(!GhostLedger::Decode(std::vector<std::uint8_t>{ 1, 0, 2, 0, 0x01, 0xA0, 0x02, 0x00 }));  // a count past the length
	CHECK(!GhostLedger::Decode(std::vector<std::uint8_t>{ 1 }));
	CHECK(GhostLedger::Encode({}).empty());
	CHECK(GhostLedger::Encode({ SPAWN_COPY }).empty());
	CHECK((GhostLedger::Decode(GhostLedger::Encode({ RAIDER_2, RAIDER, RAIDER })) == std::optional{ std::vector<std::uint32_t>{ RAIDER, RAIDER_2 } }));
	CHECK(loaded.Restore({ SPAWN_COPY }, 400) == 0);  // never from a save
}

TEST("ghost ledger: a spawn copy's entry is forgotten without a command once the copy is gone, and the ledger holds only so many")
{
	Book book;
	book.Add(SPAWN_COPY, 1);
	CHECK(book.Forget(SPAWN_COPY));
	CHECK(!book.Forget(SPAWN_COPY));
	CHECK(book.Count().cleared == 0);

	for (std::uint32_t i = 0; i < GhostLedger::MAX; ++i) {
		CHECK(book.Add(0x0003A000 + i, 1));
	}
	CHECK(!book.Add(RAIDER, 1));
	CHECK(book.Count().refused == 1);
	CHECK(book.Size() == GhostLedger::MAX);
	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(book.Ids().empty());
}
