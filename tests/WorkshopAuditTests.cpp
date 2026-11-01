#include "Test.h"

#include "SessionFile.h"
#include "WorkshopStore.h"
#include "game/WorkshopAudit.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

// A built piece its builder's world no longer has (a save from before it was built loaded): the builder's game looks
// for what the session keeps as theirs in the world it plays (src/game/WorkshopAudit.h), calling gone only what it is
// sure of, and the server takes out what it says is gone (WorkshopStore::Gone), theirs alone and that character's, and
// keeps it as lost, to come back if that world has it again. The session tests (WorkshopOrphanTests.cpp, the Sim) run
// it end to end.
namespace
{
	using Duration = WorkshopAudit::Duration;
	using WorkshopAudit::Seen;
	using namespace std::chrono_literals;

	constexpr std::uint32_t ALICE = 1;
	constexpr std::uint32_t BOB = 2;
	constexpr std::uint64_t ALICE_A = 0xA11CE0000000000Aull;  // Alice's first character's world
	constexpr std::uint64_t ALICE_B = 0xA11CE0000000000Bull;  // a second character of hers, on the same PC
	constexpr std::uint64_t BOB_A = 0xB0B000000000000Aull;
	constexpr std::uint32_t WALL = 0x0001F2A1;
	constexpr std::uint32_t TURRET = 0x000B6E85;
	constexpr std::uint32_t WIRE = 0x0001D971;
	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;

	Protocol::WorkshopItem Piece(std::uint32_t a_player, std::uint32_t a_ref, std::uint64_t a_character = 0, std::uint8_t a_op = Protocol::WorkshopOp::kPlaced,
		std::uint32_t a_base = WALL)
	{
		Protocol::WorkshopItem item;
		item.playerId = a_player;
		item.refId = a_ref;
		item.op = a_op;
		item.base = a_base;
		item.workshop = 0x000250FE;
		item.position[0] = 10.0f;
		item.character = a_character;
		return item;
	}

	// What a game says is gone: a scrap of the piece it looked for.
	Protocol::WorkshopItem Gone(std::uint32_t a_player, std::uint32_t a_ref, std::uint32_t a_base = WALL)
	{
		return Piece(a_player, a_ref, 0, Protocol::WorkshopOp::kScrapped, a_base);
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

	// Rounds of looks from a_from, every LOOK_EVERY, until a_until: everything they called gone.
	std::set<std::uint32_t> GoneBetween(WorkshopAudit::Audit& a_audit, Engine& a_engine, Duration a_from, Duration a_until)
	{
		std::set<std::uint32_t> out;
		for (auto now = a_from; now <= a_until; now += WorkshopAudit::LOOK_EVERY) {
			for (const auto ref : Refs(a_audit.Look(a_engine, now).gone)) {
				out.insert(ref);
			}
		}
		return out;
	}
}

// ---- The server's side: WorkshopStore ----

TEST("workshop store: a player's own built pieces in one character's world are listed for that world, not the world's pieces they moved or scrapped, nor anyone else's")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_A));
	store.Report(Piece(ALICE, 0x0001A2B3, ALICE_A));                                      // a piece of the world she moved: kept as built, but every game has it
	store.Report(Piece(ALICE, 0x0001A2B4, ALICE_A, Protocol::WorkshopOp::kScrapped));  // a piece of the world she scrapped
	store.Report(Piece(BOB, 0xFF000003, BOB_A));
	const auto own = store.BuiltBy(ALICE, ALICE_A);
	REQUIRE(own.size() == 2);
	CHECK(own[0].refId == 0xFF000001);
	CHECK(own[1].refId == 0xFF000002);
	CHECK(store.BuiltBy(BOB, BOB_A).size() == 1);
	CHECK(store.BuiltBy(3, ALICE_A).empty());
}

// The player ID is the PC's (Hello::identity), not the character's: a second character's world has none of the first's
// pieces, and was asked about them all, which took every one of them out for everyone.
TEST("workshop store: another character of the same player is asked about none of the first's pieces, nor about any whose character isn't known")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_B));
	store.Report(Piece(ALICE, 0xFF000003));  // from an older build, or before her record came: no character
	CHECK(Refs(store.BuiltBy(ALICE, ALICE_A)) == std::set<std::uint32_t>{ 0xFF000001 });
	CHECK(Refs(store.BuiltBy(ALICE, ALICE_B)) == std::set<std::uint32_t>{ 0xFF000002 });
	CHECK(store.BuiltBy(ALICE, 0).empty());  // a world not known
	// Her second character's game saying it lacks the first's piece takes nothing out; nor does a world not known.
	CHECK(!store.Gone(Gone(ALICE, 0xFF000001), ALICE_B));
	CHECK(!store.Gone(Gone(ALICE, 0xFF000003), ALICE_A));
	CHECK(!store.Gone(Gone(ALICE, 0xFF000003), 0));
	CHECK(store.Built().size() == 3);
	CHECK(store.Gone(Gone(ALICE, 0xFF000001), ALICE_A));
}

TEST("workshop store: what a player reported before saying whose world it is takes that world once they say, and nothing reported since says otherwise")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_A));
	store.Tag(ALICE, { 0xFF000001, 0xFF000002, 0xFF000009 }, ALICE_B);
	CHECK((store.Built().at({ ALICE, 0xFF000001 }).character == ALICE_B));
	CHECK((store.Built().at({ ALICE, 0xFF000002 }).character == ALICE_A));
	CHECK((!store.Built().contains({ ALICE, 0xFF000009 })));
}

TEST("workshop store: a piece its builder's world lacks goes, as their scrap would, and only theirs, and is kept as lost")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(BOB, 0xFF000001, BOB_A));  // the same reference ID in Bob's game: another piece
	const auto scrap = store.Gone(Gone(ALICE, 0xFF000001), ALICE_A);
	REQUIRE(scrap);
	CHECK(scrap->op == Protocol::WorkshopOp::kScrapped);
	CHECK(scrap->playerId == ALICE);
	CHECK(scrap->base == WALL);
	CHECK((!store.Built().contains({ ALICE, 0xFF000001 })));
	CHECK((store.Built().contains({ BOB, 0xFF000001 })));
	CHECK(!store.Gone(Gone(ALICE, 0xFF000001), ALICE_A));  // gone already: nothing to pass on
	// Lost, not forgotten: her game is asked about it again, as lost, and it is no part of what a joiner gets.
	CHECK((store.IsLost({ ALICE, 0xFF000001 })));
	const auto own = store.BuiltBy(ALICE, ALICE_A);
	REQUIRE(own.size() == 1);
	CHECK(own[0].op == Protocol::WorkshopOp::kLost);
	CHECK(own[0].base == WALL);
	CHECK(store.BuiltBy(ALICE, ALICE_B).empty());
	std::size_t sent = 0;
	store.ForEach([&](const WorkshopStore::Key&, const Protocol::WorkshopItem&) { ++sent; });
	CHECK(sent == 1);  // Bob's
}

TEST("workshop store: a lost piece its builder's world has again is built again, for everyone; their scrap of it forgets it")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_A));
	REQUIRE(store.Gone(Gone(ALICE, 0xFF000001), ALICE_A));
	REQUIRE(store.Gone(Gone(ALICE, 0xFF000002), ALICE_A));
	auto back = Piece(ALICE, 0xFF000001, ALICE_A);
	back.position[0] = 50.0f;  // where it stands now
	const auto outcome = store.Report(back);
	CHECK(outcome.relay);
	CHECK((store.Built().at({ ALICE, 0xFF000001 }).position[0] == 50.0f));
	CHECK((!store.IsLost({ ALICE, 0xFF000001 })));
	store.Report(Gone(ALICE, 0xFF000002));
	CHECK((!store.IsLost({ ALICE, 0xFF000002 })));
	CHECK(store.LostCount() == 0);
	// Nobody reports kLost: it is the server's own.
	CHECK(!store.Report(Piece(ALICE, 0xFF000003, ALICE_A, Protocol::WorkshopOp::kLost)).relay);
	CHECK((!store.Built().contains({ ALICE, 0xFF000003 })));
}

TEST("workshop store: the lost pieces are capped, the longest lost going first, and kept apart from the built cap")
{
	WorkshopStore store{ 3, 10, 2 };
	for (std::uint32_t ref = 1; ref <= 3; ++ref) {
		store.Report(Piece(ALICE, 0xFF000000 + ref, ALICE_A));
	}
	CHECK(store.Report(Piece(BOB, 0xFF000009, BOB_A)).full);
	for (std::uint32_t ref = 1; ref <= 3; ++ref) {
		REQUIRE(store.Gone(Gone(ALICE, 0xFF000000 + ref), ALICE_A));
	}
	CHECK(store.LostCount() == 2);
	CHECK((!store.IsLost({ ALICE, 0xFF000001 })));  // the first lost went
	CHECK((store.IsLost({ ALICE, 0xFF000003 })));
	CHECK(store.Size() == 0);  // the lost count against no cap
	CHECK(store.Report(Piece(BOB, 0xFF000009, BOB_A)).relay);
}

TEST("workshop store: a gone report leaves a piece built since under the same reference ID, the world's pieces and every scrap")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A, Protocol::WorkshopOp::kPlaced, TURRET));  // built since, under the ID a lost wall had
	CHECK(!store.Gone(Gone(ALICE, 0xFF000001), ALICE_A));                                      // looked for as the wall
	CHECK((store.Built().contains({ ALICE, 0xFF000001 })));

	store.Report(Piece(ALICE, 0x0001A2B3, ALICE_A));                                      // a piece of the world she moved
	store.Report(Piece(ALICE, 0x0001A2B4, ALICE_A, Protocol::WorkshopOp::kScrapped));  // and one she scrapped
	CHECK(!store.Gone(Gone(ALICE, 0x0001A2B3), ALICE_A));
	CHECK(!store.Gone(Gone(ALICE, 0x0001A2B4), ALICE_A));
	CHECK((store.Built().contains({ ALICE, 0x0001A2B3 })));
	CHECK((store.Scraps().contains({ ALICE, 0x0001A2B4 })));
}

TEST("workshop store: what a gone report takes out makes room under the cap")
{
	WorkshopStore store{ 2, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_A));
	CHECK(store.Report(Piece(BOB, 0xFF000003, BOB_A)).full);
	REQUIRE(store.Gone(Gone(ALICE, 0xFF000002), ALICE_A));
	const auto outcome = store.Report(Piece(BOB, 0xFF000003, BOB_A));
	CHECK(outcome.relay);
	CHECK(!outcome.full);
	CHECK(store.Built().size() == 2);
}

TEST("workshop store: the characters and the lost pieces are kept through the session file (layout 42), and a layout 41 file reads with no character")
{
	WorkshopStore store{ 10, 10 };
	store.Report(Piece(ALICE, 0xFF000001, ALICE_A));
	store.Report(Piece(ALICE, 0xFF000002, ALICE_A));
	store.Report(Piece(BOB, 0xFF000003));
	REQUIRE(store.Gone(Gone(ALICE, 0xFF000002), ALICE_A));
	SessionFile::Snapshot snapshot;
	store.ForEachKept([&](const WorkshopStore::Key&, const Protocol::WorkshopItem& a_item) { snapshot.workshopItems.push_back(a_item); });
	const auto bytes = SessionFile::Encode(snapshot);
	std::uint16_t layout = 0;
	std::memcpy(&layout, bytes.data() + 4, sizeof(layout));
	CHECK(layout == SessionFile::WORKSHOP_CHARACTERS);
	const auto out = SessionFile::Decode(bytes);
	REQUIRE(out);
	CHECK(out->workshopItems == snapshot.workshopItems);
	WorkshopStore again{ 10, 10 };
	for (const auto& item : out->workshopItems) {
		again.Restore(item);
	}
	CHECK((Refs(again.BuiltBy(ALICE, ALICE_A)) == std::set<std::uint32_t>{ 0xFF000001, 0xFF000002 }));
	CHECK((again.IsLost({ ALICE, 0xFF000002 })));
	CHECK(again.Built().size() == 2);

	// A session with no character known and nothing lost is written as before; a 41 file (the rigs', the test builds')
	// reads with every piece's character unknown: never asked about.
	SessionFile::Snapshot old;
	old.workshopItems = { Piece(ALICE, 0xFF000001), Piece(BOB, 0xFF000003) };
	old.npcLevel = 12;  // a 41 file's
	auto oldBytes = SessionFile::Encode(old);
	std::memcpy(&layout, oldBytes.data() + 4, sizeof(layout));
	CHECK(layout == SessionFile::ROLLS);
	const auto read = SessionFile::Decode(oldBytes);
	REQUIRE(read);
	CHECK(read->workshopItems == old.workshopItems);
	WorkshopStore fromOld{ 10, 10 };
	for (const auto& item : read->workshopItems) {
		fromOld.Restore(item);
	}
	CHECK(fromOld.BuiltBy(ALICE, ALICE_A).empty());
}

TEST("workshop items: kLost is read only in what the server keeps (kOwnWorkshopItems, its session file), never in a player's report")
{
	const auto lost = Piece(ALICE, 0xFF000001, 0, Protocol::WorkshopOp::kLost);
	CHECK(Protocol::DecodeWorkshopItems(Protocol::Encode(std::vector{ lost }, Protocol::MessageType::kOwnWorkshopItems)));
	CHECK(!Protocol::DecodeWorkshopItems(Protocol::Encode(std::vector{ lost })));
	CHECK(!Protocol::DecodeWorkshopItems(Protocol::Encode(std::vector{ lost }, Protocol::MessageType::kReportWorkshopGone)));
	CHECK(!Protocol::DecodeWorkshopItem(Protocol::Encode(lost, Protocol::MessageType::kReportWorkshopItem)));
	const auto kept = Protocol::DecodeKeptWorkshopItems(Protocol::EncodeKeptWorkshopItems({ Piece(ALICE, 0xFF000001, ALICE_A, Protocol::WorkshopOp::kLost) }));
	REQUIRE(kept);
	CHECK((*kept)[0].character == ALICE_A);
	CHECK((*kept)[0].op == Protocol::WorkshopOp::kLost);
	// The character is the server's own: never on the wire.
	const auto sent = Protocol::DecodeWorkshopItems(Protocol::Encode(std::vector{ Piece(ALICE, 0xFF000001, ALICE_A) }));
	REQUIRE(sent);
	CHECK((*sent)[0].character == 0);
}

// ---- The builder's game: WorkshopAudit ----

TEST("workshop audit: a piece that stands is found and looked for no more; one that is something else here is gone once a second look says so too")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001), Piece(ALICE, 0xFF000002) });
	Engine engine{ { { 0xFF000001, Seen::kStands }, { 0xFF000002, Seen::kGone } } };
	CHECK(audit.Look(engine, 0s).gone.empty());  // one look: the world may still be changing under it
	CHECK(audit.Found() == 1);
	CHECK(audit.Look(engine, 1s).gone.empty());  // no round yet
	const auto gone = audit.Look(engine, WorkshopAudit::LOOK_EVERY).gone;
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].refId == 0xFF000002);
	CHECK(gone[0].op == Protocol::WorkshopOp::kScrapped);
	CHECK(gone[0].base == WALL);
	CHECK(audit.Waiting() == 0);
	CHECK(audit.LostWaiting() == 1);  // looked for still, to be found
	CHECK(audit.Lost() == 1);
}

TEST("workshop audit: something else at its ID once, then not seen, is no second look")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine engine{ { { 0xFF000001, Seen::kGone } } };
	CHECK(audit.Look(engine, 0s).gone.empty());
	engine.seen[0xFF000001] = Seen::kUnseen;  // the world went unsettled, or it went out of reach
	CHECK(audit.Look(engine, WorkshopAudit::LOOK_EVERY).gone.empty());
	engine.seen[0xFF000001] = Seen::kGone;
	CHECK(audit.Look(engine, 2 * WorkshopAudit::LOOK_EVERY).gone.empty());
	CHECK(audit.Look(engine, 3 * WorkshopAudit::LOOK_EVERY).gone.size() == 1);
}

TEST("workshop audit: a piece missing with its cells loaded is gone only after three looks over ten seconds")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine engine{ { { 0xFF000001, Seen::kMissing } } };
	// Three looks in 4 s: too soon.
	CHECK(GoneBetween(audit, engine, 0s, 2 * WorkshopAudit::LOOK_EVERY).empty());
	// Ten seconds from the first look: gone.
	CHECK(GoneBetween(audit, engine, 3 * WorkshopAudit::LOOK_EVERY, WorkshopAudit::CONFIRM - 1s).empty());
	CHECK(GoneBetween(audit, engine, WorkshopAudit::CONFIRM, WorkshopAudit::CONFIRM) == std::set<std::uint32_t>{ 0xFF000001 });
	CHECK(audit.Waiting() == 0);
	// And three looks are needed however long apart the time says they are.
	WorkshopAudit::Audit slow;
	slow.Add({ Piece(ALICE, 0xFF000002) });
	engine.seen[0xFF000002] = Seen::kMissing;
	CHECK(slow.Look(engine, 0s).gone.empty());
	CHECK(slow.Look(engine, 60s).gone.empty());
	CHECK(slow.Look(engine, 120s).gone.size() == 1);
}

TEST("workshop audit: a piece whose cells aren't loaded (another worldspace, towns away, no world yet, cells just loaded) is never called gone, and its wait starts over")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine engine{ { { 0xFF000001, Seen::kUnseen } } };
	CHECK(GoneBetween(audit, engine, 0s, 120s).empty());
	CHECK(audit.Waiting() == 1);
	// Its cells load without it, then unload before the wait is up: missing counts again from the next time.
	engine.seen[0xFF000001] = Seen::kMissing;
	CHECK(GoneBetween(audit, engine, 200s, 208s).empty());
	engine.seen[0xFF000001] = Seen::kUnseen;
	CHECK(audit.Look(engine, 210s).gone.empty());
	engine.seen[0xFF000001] = Seen::kMissing;
	CHECK(GoneBetween(audit, engine, 212s, 220s).empty());
	CHECK(GoneBetween(audit, engine, 222s, 222s).size() == 1);
	// One whose cell loads with it there after all: found.
	audit.Add({ Piece(ALICE, 0xFF000002) });
	engine.seen[0xFF000002] = Seen::kMissing;
	CHECK(GoneBetween(audit, engine, 300s, 306s).empty());
	engine.seen[0xFF000002] = Seen::kStands;
	CHECK(audit.Look(engine, 308s).gone.empty());
	CHECK(audit.Waiting() == 0);
	CHECK(audit.Found() == 1);
}

TEST("workshop audit: a piece reported gone that turns up (its cell was not loaded after all) is found, to be reported placed again")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001) });
	Engine engine{ { { 0xFF000001, Seen::kGone } } };
	CHECK(GoneBetween(audit, engine, 0s, WorkshopAudit::LOOK_EVERY).size() == 1);
	// Gone, missing, unseen: it stays lost, and is never reported gone twice.
	Duration from = 10s;
	for (const auto seen : { Seen::kGone, Seen::kMissing, Seen::kUnseen }) {
		engine.seen[0xFF000001] = seen;
		CHECK(GoneBetween(audit, engine, from, from + 50s).empty());
		from += 60s;
	}
	engine.seen[0xFF000001] = Seen::kStands;
	const auto round = audit.Look(engine, from);
	CHECK(Refs(round.found) == std::set<std::uint32_t>{ 0xFF000001 });
	CHECK(round.found[0].op == Protocol::WorkshopOp::kPlaced);
	CHECK(audit.Restored() == 1);
	CHECK(audit.LostWaiting() == 0);
}

TEST("workshop audit: a piece the server keeps as lost is only ever found, never reported gone")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001, 0, Protocol::WorkshopOp::kLost), Piece(ALICE, 0xFF000002, 0, Protocol::WorkshopOp::kLost) });
	CHECK(audit.Waiting() == 0);
	CHECK(audit.LostWaiting() == 2);
	Engine engine{ { { 0xFF000001, Seen::kGone }, { 0xFF000002, Seen::kStands } } };
	std::set<std::uint32_t> found;
	for (Duration now = 0s; now <= 60s; now += WorkshopAudit::LOOK_EVERY) {
		const auto round = audit.Look(engine, now);
		CHECK(round.gone.empty());
		for (const auto ref : Refs(round.found)) {
			found.insert(ref);
		}
	}
	CHECK(found == std::set<std::uint32_t>{ 0xFF000002 });
	CHECK(audit.LostWaiting() == 1);
}

TEST("workshop audit: a piece we reported since is looked for no more, and a load forgets the list")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001), Piece(ALICE, 0xFF000002) });
	audit.Reported(0xFF000001);  // built again under that ID since the load, or moved, or scrapped: that report stands
	Engine engine{ { { 0xFF000001, Seen::kGone }, { 0xFF000002, Seen::kUnseen } } };
	CHECK(GoneBetween(audit, engine, 0s, 30s).empty());
	CHECK(audit.Waiting() == 1);
	audit.Clear();
	CHECK(audit.Waiting() == 0);
	CHECK(audit.Look(engine, 40s).gone.empty());
}

TEST("workshop audit: only pieces a game made are looked for, never the world's own a player moved, nor a scrap")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0x0001A2B3), Gone(ALICE, 0xFF000003), Piece(ALICE, 0xFF000001), Piece(ALICE, 0x0001A2B5, 0, Protocol::WorkshopOp::kLost) });
	CHECK(audit.Waiting() == 1);
	CHECK(audit.LostWaiting() == 0);
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
	std::set<std::uint32_t> gone;
	for (Duration now = 0s; now < 6 * WorkshopAudit::LOOK_EVERY; now += WorkshopAudit::LOOK_EVERY) {
		const auto some = audit.Look(engine, now).gone;
		CHECK(some.size() <= WorkshopAudit::LOOKS_PER_ROUND);
		for (const auto& item : some) {
			CHECK(gone.insert(item.refId).second);  // each once
		}
	}
	CHECK(gone.size() == 600);
	CHECK(engine.looks == 6 * WorkshopAudit::LOOKS_PER_ROUND);
}

// ---- Which cells have been loaded long enough: CellAges, Spots ----

TEST("workshop audit cells: a cell counts once attached for ten seconds, with all eight around it; one that unloads starts afresh")
{
	WorkshopAudit::CellAges ages;
	std::vector<WorkshopCells::Where> grid;
	for (std::int32_t x = -2; x <= 2; ++x) {
		for (std::int32_t y = -2; y <= 2; ++y) {
			grid.push_back({ COMMONWEALTH, x, y });
		}
	}
	ages.Update(grid, 0s);
	CHECK(!ages.SettledAround(COMMONWEALTH, 0, 0, WorkshopAudit::CELL_SETTLE - 1s));
	CHECK(ages.SettledAround(COMMONWEALTH, 0, 0, WorkshopAudit::CELL_SETTLE));
	CHECK(ages.SettledAround(COMMONWEALTH, 1, -1, WorkshopAudit::CELL_SETTLE));
	CHECK(!ages.SettledAround(COMMONWEALTH, 2, 0, 60s));  // the cells past it aren't attached
	CHECK(!ages.SettledAround(0x0001A270, 0, 0, 60s));    // another worldspace
	// The player walks one cell east: the new column is just attached, the far one gone.
	std::vector<WorkshopCells::Where> moved;
	for (std::int32_t x = -1; x <= 3; ++x) {
		for (std::int32_t y = -2; y <= 2; ++y) {
			moved.push_back({ COMMONWEALTH, x, y });
		}
	}
	ages.Update(moved, 30s);
	CHECK(ages.SettledAround(COMMONWEALTH, 1, 0, 30s));
	CHECK(!ages.SettledAround(COMMONWEALTH, 2, 0, 35s));
	CHECK(ages.SettledAround(COMMONWEALTH, 2, 0, 40s));
	CHECK((!ages.Settled({ COMMONWEALTH, -2, 0 }, 40s)));
	// And back: the column that unloaded counts from now.
	ages.Update(grid, 50s);
	CHECK(!ages.SettledAround(COMMONWEALTH, -1, 0, 55s));
	CHECK(ages.SettledAround(COMMONWEALTH, -1, 0, 60s));
	// An interior.
	ages.Update({ WorkshopCells::Interior(0x0001F00D) }, 70s);
	CHECK(!ages.Settled(WorkshopCells::Interior(0x0001F00D), 75s));
	CHECK(ages.Settled(WorkshopCells::Interior(0x0001F00D), 80s));
	CHECK(ages.Size() == 1);
}

TEST("workshop audit cells: a piece reaches its spot's cell, and a wire strung across cells the cells of both its ends")
{
	auto chair = Piece(ALICE, 0xFF000001);
	chair.position[0] = 100.0f;
	chair.position[1] = 100.0f;
	CHECK((WorkshopAudit::Spots(chair) == std::vector<std::pair<std::int32_t, std::int32_t>>{ { 0, 0 } }));
	auto wire = Piece(ALICE, 0xFF000002, 0, Protocol::WorkshopOp::kPlaced, WIRE);
	wire.position[0] = 4096.0f;  // its middle on the line between cells 0 and 1
	wire.position[1] = 100.0f;
	wire.wire = Protocol::WireShape{ .halfExtents = { 600.0f, 0.0f, 50.0f } };
	const auto spots = WorkshopAudit::Spots(wire);
	CHECK((std::set(spots.begin(), spots.end()) == std::set<std::pair<std::int32_t, std::int32_t>>{ { 0, 0 }, { 1, 0 } }));
	// A cell beside the middle's loaded long enough, the far end's not yet: the wire isn't missing.
	WorkshopAudit::CellAges ages;
	std::vector<WorkshopCells::Where> west;
	for (std::int32_t x = -2; x <= 1; ++x) {
		for (std::int32_t y = -2; y <= 2; ++y) {
			west.push_back({ COMMONWEALTH, x, y });
		}
	}
	ages.Update(west, 0s);
	const auto all = [&](Duration a_now) {
		return std::ranges::all_of(spots, [&](const auto& a_spot) { return ages.SettledAround(COMMONWEALTH, a_spot.first, a_spot.second, a_now); });
	};
	CHECK(!all(60s));
	west.push_back({ COMMONWEALTH, 2, -1 });
	west.push_back({ COMMONWEALTH, 2, 0 });
	west.push_back({ COMMONWEALTH, 2, 1 });
	ages.Update(west, 60s);
	CHECK(!all(65s));
	CHECK(all(70s));
}

TEST("workshop audit: its time is the settled world's: a long menu, a loading screen or a stopped game adds none, however long by the wall clock")
{
	WorkshopAudit::Audit audit;
	audit.Add({ Piece(ALICE, 0xFF000001), Piece(ALICE, 0xFF000002) });
	Engine engine{ { { 0xFF000001, Seen::kMissing }, { 0xFF000002, Seen::kGone } } };
	CHECK(audit.Look(engine, 5s).gone.empty());
	// The game stops (AuditFrame passes no time and looks at nothing): asked again at the same time, no round runs.
	for (int i = 0; i < 100; ++i) {
		CHECK(audit.Look(engine, 5s).gone.empty());
	}
	CHECK(engine.looks == 2);
	// Running again: the second look, two seconds of play later, confirms the one that is something else; the missing
	// one waits for its three looks over ten seconds of play.
	CHECK((Refs(audit.Look(engine, 5s + WorkshopAudit::LOOK_EVERY).gone) == std::set<std::uint32_t>{ 0xFF000002 }));
	CHECK(GoneBetween(audit, engine, 5s + 2 * WorkshopAudit::LOOK_EVERY, 14s).empty());
	CHECK((GoneBetween(audit, engine, 15s, 15s) == std::set<std::uint32_t>{ 0xFF000001 }));
}
