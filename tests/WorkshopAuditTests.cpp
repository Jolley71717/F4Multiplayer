#include "Test.h"

#include "WorkshopStore.h"
#include "game/WorkshopAudit.h"

#include <map>
#include <set>
#include <vector>

// A built piece its builder's world no longer has (a save from before it was built loaded): the builder's game looks
// for what the session keeps as theirs (src/game/WorkshopAudit.h), calling gone only what it is sure of, and the server
// takes out what it says is gone (WorkshopStore::Gone), theirs alone. The session tests (WorkshopSessionTests.cpp, the
// Sim) run it end to end.
namespace
{
	using Clock = WorkshopAudit::Clock;
	using WorkshopAudit::Seen;

	constexpr std::uint32_t ALICE = 1;
	constexpr std::uint32_t BOB = 2;
	constexpr std::uint32_t WALL = 0x0001F2A1;
	constexpr std::uint32_t TURRET = 0x000B6E85;

	Protocol::WorkshopItem Piece(std::uint32_t a_player, std::uint32_t a_ref, std::uint8_t a_op = Protocol::WorkshopOp::kPlaced, std::uint32_t a_base = WALL)
	{
		Protocol::WorkshopItem item;
		item.playerId = a_player;
		item.refId = a_ref;
		item.op = a_op;
		item.base = a_base;
		item.workshop = 0x000250FE;
		item.position[0] = 10.0f;
		return item;
	}

	Protocol::WorkshopItem Gone(std::uint32_t a_player, std::uint32_t a_ref, std::uint32_t a_base = WALL)
	{
		return Piece(a_player, a_ref, Protocol::WorkshopOp::kScrapped, a_base);
	}

	// What our world shows of each piece; kUnseen for any it isn't told of.
	struct Engine
	{
		std::map<std::uint32_t, Seen> seen;
		std::size_t                   looks = 0;

		Seen Look(const Protocol::WorkshopItem& a_item)
		{
			++looks;
			const auto it = seen.find(a_item.refId);
			return it == seen.end() ? Seen::kUnseen : it->second;
		}
	};

	std::set<std::uint32_t> Refs(const std::vector<Protocol::WorkshopItem>& a_items)
	{
		std::set<std::uint32_t> out;
		for (const auto& item : a_items) {
			out.insert(item.refId);
		}
		return out;
	}
}

// ---- The server's side: WorkshopStore ----

TEST("workshop store: a player's own built pieces are listed for their game, not the world's pieces they moved or scrapped, nor anyone else's")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001));
	store.Report(Piece(ALICE, 0xFF000002));
	store.Report(Piece(ALICE, 0x0001A2B3));                              // a piece of the world she moved: kept as built, but every game has it
	store.Report(Piece(ALICE, 0x0001A2B4, Protocol::WorkshopOp::kScrapped));  // a piece of the world she scrapped
	store.Report(Piece(BOB, 0xFF000003));
	const auto own = store.BuiltBy(ALICE);
	REQUIRE(own.size() == 2);
	CHECK(own[0].refId == 0xFF000001);
	CHECK(own[1].refId == 0xFF000002);
	CHECK(store.BuiltBy(BOB).size() == 1);
	CHECK(store.BuiltBy(3).empty());
}

TEST("workshop store: a piece its builder's world lacks goes, as their scrap would, and only theirs")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001));
	store.Report(Piece(BOB, 0xFF000001));  // the same reference ID in Bob's game: another piece
	const auto scrap = store.Gone(Gone(ALICE, 0xFF000001));
	REQUIRE(scrap);
	CHECK(scrap->op == Protocol::WorkshopOp::kScrapped);
	CHECK(scrap->playerId == ALICE);
	CHECK(scrap->base == WALL);
	CHECK(!store.Built().contains({ ALICE, 0xFF000001 }));
	CHECK(store.Built().contains({ BOB, 0xFF000001 }));
	CHECK(!store.Gone(Gone(ALICE, 0xFF000001)));  // gone already: nothing to pass on
}

TEST("workshop store: a gone report leaves a piece built since under the same reference ID, the world's pieces and every scrap")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, Protocol::WorkshopOp::kPlaced, TURRET));  // built since, under the ID a lost wall had
	CHECK(!store.Gone(Gone(ALICE, 0xFF000001)));                                    // looked for as the wall
	CHECK(store.Built().contains({ ALICE, 0xFF000001 }));

	store.Report(Piece(ALICE, 0x0001A2B3));                                   // a piece of the world she moved
	store.Report(Piece(ALICE, 0x0001A2B4, Protocol::WorkshopOp::kScrapped));  // and one she scrapped
	CHECK(!store.Gone(Gone(ALICE, 0x0001A2B3)));
	CHECK(!store.Gone(Gone(ALICE, 0x0001A2B4)));
	CHECK(store.Built().contains({ ALICE, 0x0001A2B3 }));
	CHECK(store.Scraps().contains({ ALICE, 0x0001A2B4 }));
}

TEST("workshop store: what a gone report takes out makes room under the cap")
{
	WorkshopStore store{ 2, 10 };
	store.Report(Piece(ALICE, 0xFF000001));
	store.Report(Piece(ALICE, 0xFF000002));
	CHECK(store.Report(Piece(BOB, 0xFF000003)).full);
	REQUIRE(store.Gone(Gone(ALICE, 0xFF000002)));
	const auto outcome = store.Report(Piece(BOB, 0xFF000003));
	CHECK(outcome.relay);
	CHECK(!outcome.full);
	CHECK(store.Built().size() == 2);
}

// ---- The builder's game: WorkshopAudit ----

TEST("workshop audit: a piece that stands is found and looked for no more; one that is something else here is gone at once, as a scrap of what the server keeps")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001), Piece(ALICE, 0xFF000002) });
	Engine     engine{ { { 0xFF000001, Seen::kStands }, { 0xFF000002, Seen::kGone } } };
	const auto gone = audit.Look(engine, Clock::now());
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].refId == 0xFF000002);
	CHECK(gone[0].op == Protocol::WorkshopOp::kScrapped);
	CHECK(gone[0].base == WALL);
	CHECK(audit.Waiting() == 0);
	CHECK(audit.Found() == 1);
	CHECK(audit.Lost() == 1);
}

TEST("workshop audit: a piece missing with its cell loaded is gone only once it stays missing a while")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine     engine{ { { 0xFF000001, Seen::kMissing } } };
	const auto t0 = Clock::now();
	CHECK(audit.Look(engine, t0).empty());
	CHECK(audit.Look(engine, t0 + std::chrono::seconds(1)).empty());    // no round yet
	CHECK(audit.Look(engine, t0 + WorkshopAudit::LOOK_EVERY).empty());  // missing for 2 s
	CHECK(Refs(audit.Look(engine, t0 + 2 * WorkshopAudit::LOOK_EVERY)) == std::set<std::uint32_t>{ 0xFF000001 });
	CHECK(audit.Waiting() == 0);
}

TEST("workshop audit: a piece whose cell isn't loaded (another worldspace, towns away, no world yet) is never called gone, and its wait starts over")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine engine{ { { 0xFF000001, Seen::kUnseen } } };
	auto   now = Clock::now();
	for (int i = 0; i < 20; ++i, now += WorkshopAudit::LOOK_EVERY) {
		CHECK(audit.Look(engine, now).empty());
	}
	CHECK(audit.Waiting() == 1);
	// Its cell loads without it, then unloads before the wait is up: missing counts again from the next time.
	engine.seen[0xFF000001] = Seen::kMissing;
	CHECK(audit.Look(engine, now).empty());
	engine.seen[0xFF000001] = Seen::kUnseen;
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).empty());
	engine.seen[0xFF000001] = Seen::kMissing;
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).empty());
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).empty());  // 2 s missing
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).size() == 1);
	// One whose cell loads with it there after all: found.
	audit.Add({ Piece(ALICE, 0xFF000002) });
	engine.seen[0xFF000002] = Seen::kMissing;
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).empty());
	engine.seen[0xFF000002] = Seen::kStands;
	CHECK(audit.Look(engine, now += WorkshopAudit::LOOK_EVERY).empty());
	CHECK(audit.Waiting() == 0);
	CHECK(audit.Found() == 1);
}

TEST("workshop audit: a piece we reported since is looked for no more, and a load forgets the list")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001), Piece(ALICE, 0xFF000002) });
	audit.Reported(0xFF000001);  // built again under that ID since the load, or moved, or scrapped: that report stands
	Engine engine{ { { 0xFF000001, Seen::kGone }, { 0xFF000002, Seen::kUnseen } } };
	CHECK(audit.Look(engine, Clock::now()).empty());
	CHECK(audit.Waiting() == 1);
	audit.Clear();
	CHECK(audit.Waiting() == 0);
	CHECK(audit.Look(engine, Clock::now()).empty());
}

TEST("workshop audit: only pieces a game made are looked for, never the world's own a player moved, nor a scrap")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0x0001A2B3), Gone(ALICE, 0xFF000003), Piece(ALICE, 0xFF000001) });
	CHECK(audit.Waiting() == 1);
}

TEST("workshop audit: a big list is looked at a few hundred a round, each in turn")
{
	WorkshopAudit::Audit                audit;
	std::vector<Protocol::WorkshopItem> list;
	Engine                              engine;
	for (std::uint32_t i = 1; i <= 600; ++i) {
		list.push_back(Piece(ALICE, 0xFF000000 + i));
		engine.seen[0xFF000000 + i] = Seen::kGone;
	}
	audit.Add(list);
	auto                    now = Clock::now();
	std::set<std::uint32_t> gone;
	for (int round = 0; round < 3; ++round, now += WorkshopAudit::LOOK_EVERY) {
		const auto some = audit.Look(engine, now);
		CHECK(some.size() <= WorkshopAudit::LOOKS_PER_ROUND);
		for (const auto& item : some) {
			CHECK(gone.insert(item.refId).second);  // each once
		}
	}
	CHECK(gone.size() == 600);
	CHECK(engine.looks == 600);
}
