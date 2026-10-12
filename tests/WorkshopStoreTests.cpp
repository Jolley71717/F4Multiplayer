#include "Test.h"

#include "WorkshopStore.h"

#include <vector>

// What the server keeps of settlement building for late joiners: what's built, and the pieces of the
// world scrapped, each kind under its own cap.
namespace
{
	constexpr std::uint32_t ALICE = 1;
	constexpr std::uint32_t BOB = 2;

	Protocol::WorkshopItem Item(std::uint32_t a_player, std::uint32_t a_ref, std::uint8_t a_op = Protocol::WorkshopOp::kPlaced, float a_x = 10.0f)
	{
		Protocol::WorkshopItem item;
		item.playerId = a_player;
		item.refId = a_ref;
		item.op = a_op;
		item.base = 0x0001F2A1;
		item.workshop = 0x000250FE;
		item.position[0] = a_x;
		return item;
	}

	Protocol::WorkshopItem Scrap(std::uint32_t a_player, std::uint32_t a_ref)
	{
		return Item(a_player, a_ref, Protocol::WorkshopOp::kScrapped);
	}
}

TEST("workshop store: what's built is kept and relayed, a move replaces it")
{
	WorkshopStore store{ 10, 10 };
	auto          outcome = store.Report(Item(ALICE, 0xFF000001));
	CHECK(outcome.relay);
	CHECK(!outcome.full);
	outcome = store.Report(Item(ALICE, 0xFF000001, Protocol::WorkshopOp::kPlaced, 20.0f));
	CHECK(outcome.relay);
	REQUIRE(store.Built().size() == 1);
	CHECK(store.Built().begin()->second.position[0] == 20.0f);
}

TEST("workshop store: something built and then scrapped goes, and is relayed")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Item(ALICE, 0xFF000001));
	CHECK(store.Report(Scrap(ALICE, 0xFF000001)).relay);
	CHECK(store.Size() == 0);
}

TEST("workshop store: scrapping something nobody knew about is neither kept nor relayed")
{
	WorkshopStore store{ 10, 10 };
	const auto    outcome = store.Report(Scrap(ALICE, 0xFF000009));
	CHECK(!outcome.relay);
	CHECK(!outcome.full);
	CHECK(store.Size() == 0);
}

TEST("workshop store: a piece of the world scrapped is kept, once")
{
	WorkshopStore store{ 10, 10 };
	CHECK(store.Report(Scrap(ALICE, 0x0001A2B3)).relay);
	CHECK(!store.Report(Scrap(ALICE, 0x0001A2B3)).relay);  // reported again: already gone for everyone
	CHECK(store.Scraps().size() == 1);
	CHECK(store.Built().empty());
}

TEST("workshop store: a piece of the world moved and then scrapped is kept as scrapped, not as moved")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Item(ALICE, 0x0001A2B3, Protocol::WorkshopOp::kPlaced, 20.0f));
	CHECK(store.Built().size() == 1);
	CHECK(store.Report(Scrap(ALICE, 0x0001A2B3)).relay);
	CHECK(store.Built().empty());
	REQUIRE(store.Scraps().size() == 1);
	CHECK(store.Scraps().begin()->second.op == Protocol::WorkshopOp::kScrapped);
}

TEST("workshop store: scraps of the world have their own cap, and leave the room for what's built")
{
	WorkshopStore store{ 2, 3 };
	for (std::uint32_t i = 0; i < 3; ++i) {
		CHECK(!store.Report(Scrap(ALICE, 0x00010000 + i)).full);
	}
	auto outcome = store.Report(Scrap(ALICE, 0x00020000));
	CHECK(outcome.full);
	CHECK(!outcome.relay);
	CHECK(store.Scraps().size() == 3);

	// Building still has its room.
	CHECK(store.Report(Item(BOB, 0xFF000001)).relay);
	CHECK(store.Report(Item(BOB, 0xFF000002)).relay);
	outcome = store.Report(Item(BOB, 0xFF000003));
	CHECK(outcome.full);
	CHECK(!outcome.relay);
	CHECK(store.Report(Item(BOB, 0xFF000002, Protocol::WorkshopOp::kPlaced, 30.0f)).relay);  // a move of one kept is
	CHECK(store.Built().size() == 2);
}

TEST("workshop store: past the scraps' cap, a piece of the world moved first still loses its copy")
{
	WorkshopStore store{ 10, 1 };
	store.Report(Scrap(ALICE, 0x00010000));
	store.Report(Item(BOB, 0x0001A2B3, Protocol::WorkshopOp::kPlaced, 20.0f));
	const auto outcome = store.Report(Scrap(BOB, 0x0001A2B3));
	CHECK(outcome.full);
	CHECK(outcome.relay);
	CHECK(store.Built().empty());
	CHECK(store.Scraps().size() == 1);
}

TEST("workshop store: a saved session comes back split by kind, past the caps, built first")
{
	WorkshopStore store{ 1, 1 };
	store.Restore(Item(ALICE, 0xFF000001));
	store.Restore(Item(ALICE, 0xFF000002));
	store.Restore(Scrap(ALICE, 0x0001A2B3));
	store.Restore(Scrap(BOB, 0x0001A2B4));
	CHECK(store.Built().size() == 2);
	CHECK(store.Scraps().size() == 2);
	CHECK(store.Size() == 4);
	std::vector<std::uint8_t> ops;
	store.ForEach([&](const WorkshopStore::Key&, const Protocol::WorkshopItem& a_item) { ops.push_back(a_item.op); });
	CHECK((ops == std::vector<std::uint8_t>{ Protocol::WorkshopOp::kPlaced, Protocol::WorkshopOp::kPlaced, Protocol::WorkshopOp::kScrapped, Protocol::WorkshopOp::kScrapped }));
	store.Clear();
	CHECK(store.Size() == 0);
}
