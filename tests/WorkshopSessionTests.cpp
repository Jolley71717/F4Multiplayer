// Settlement building across a session, end to end without the game: a real Server, real Protocol
// messages from two or three players, and what the server sends fed into the client's real inbox
// (game/WorkshopInbox.h) and its real rules for each report (game/WorkshopApply.h, the code the plugin
// runs). Only the engine under those rules is a stand-in (Game below): the pieces standing in its
// world, which cells and workbenches are loaded.

#include "ServerFixture.h"

#include "game/WorkshopApply.h"
#include "game/WorkshopInbox.h"
#include "game/WorkshopMove.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <set>

using namespace TestServer;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Key = std::pair<std::uint32_t, std::uint32_t>;
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t WALL = 0x0001F2A1;
	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t TURRET = 0x000B6E85;  // a machine-gun turret: an actor
	constexpr std::uint32_t CRATE = 0x0010C3C3;   // a workshop container
	constexpr std::uint32_t FRAME = 0x0010A0F2;   // a power armor frame's furniture

	// Sync, Received and JoinAs are the fixture's (ServerFixture.h).

	Protocol::WorkshopItem Wall(std::uint32_t a_ref, float a_x, float a_y = 1000.0f)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.base = WALL;
		item.workshop = BENCH;
		item.position[0] = a_x;
		item.position[1] = a_y;
		item.rotation[2] = 1.5f;
		return item;
	}

	Protocol::WorkshopItem Scrapped(Protocol::WorkshopItem a_item)
	{
		a_item.op = Protocol::WorkshopOp::kScrapped;
		return a_item;
	}

	std::vector<std::uint8_t> Report(const Protocol::WorkshopItem& a_item)
	{
		return Protocol::Encode(a_item, MT::kReportWorkshopItem);
	}

	// A friend's game, as far as building goes: the engine under WorkshopApply (the World it asks).
	// What to do with each report is WorkshopApply's; this only keeps what stands in the world.
	struct Game
	{
		struct Piece
		{
			std::uint32_t                          base;
			WorkshopMove::Transform                at;
			std::pair<std::int32_t, std::int32_t> cell;  // the cell it was made in: the engine keeps it there
			int                                    held = 0;  // items in its inventory
		};

		using Copy = int;  // a piece's ID in `world`: the stand-in's handle

		static constexpr float ADOPT_RADIUS = 4.0f;  // a copy the save left counts if it stands this close
		static constexpr float REACH = 12000.0f;     // how far from its workbench a piece may stand
		static constexpr float BENCH_X = 0.0f, BENCH_Y = 1000.0f;

		std::map<int, Piece>                               world;   // what stands in the world, by an ID of the stand-in's own
		std::map<Key, int>                                 copies;  // the copies this run knows, as handles
		std::set<WorkshopCells::Where>                     loaded;
		std::set<std::uint32_t>                            benches{ BENCH };  // workbenches in memory here (WorkshopSync::WhereOf)
		std::map<std::uint32_t, WorkshopApply::Kind>       kinds{ { TURRET, WorkshopApply::Kind::kActor }, { CRATE, WorkshopApply::Kind::kContainer } };
		int                                                inventory = 0;  // the player's items
		std::set<std::uint32_t>                            stocked;  // container bases that come with things in them
		WorkshopInbox::Inbox<Key, Protocol::WorkshopItem> inbox;
		WorkshopApply::Counts                              counts;
		int                                                nextId = 1;
		Clock::time_point                                  now{};
		std::function<Clock::duration()>                   placeCost = [] { return std::chrono::microseconds(10); };
		std::size_t                                        mostInAFrame = 0;
		int                                                frames = 0;
		int                                                applied = 0;  // reports handed to WorkshopApply, waiting ones tried again included

		static WorkshopCells::Where Cell(const Protocol::WorkshopItem& a_item) { return WorkshopCells::Exterior(COMMONWEALTH, a_item.position[0], a_item.position[1]); }

		// ---- the World WorkshopApply asks ----

		bool                 Loaded(const Protocol::WorkshopItem& a_item) const { return benches.contains(a_item.workshop) && loaded.contains(Cell(a_item)); }
		WorkshopCells::Where WhereOf(const Protocol::WorkshopItem& a_item) const { return benches.contains(a_item.workshop) ? Cell(a_item) : WorkshopCells::Where{}; }
		WorkshopCells::Where WhereScrapped(const Protocol::WorkshopItem& a_item) const { return WhereOf(a_item); }
		bool                 InReach(const Protocol::WorkshopItem& a_item) const { return std::hypot(a_item.position[0] - BENCH_X, a_item.position[1] - BENCH_Y) <= REACH; }
		std::set<int>        blocked;  // carrying the keyword WorkshopSync gives its copies (saved with them)
		void                 Keep(Copy a_copy) { blocked.insert(a_copy); }
		bool                 MadeByUs(Copy a_copy) const { return blocked.contains(a_copy); }
		WorkshopApply::Placeable CanPlace(const Protocol::WorkshopItem& a_item) const
		{
			if (!WorkshopApply::Shared(true, a_item.base == FRAME)) {
				return WorkshopApply::Placeable::kFrame;  // WorkshopSync::CanPlace
			}
			return stocked.contains(a_item.base) ? WorkshopApply::Placeable::kStocked : WorkshopApply::Placeable::kYes;
		}

		// Pieces of the world itself (the settlement's own beds and crops), by their reference ID in every game.
		std::map<std::uint32_t, int> own;
		std::set<std::uint32_t>      unmovable;  // ones no friend may move here (not that workbench's, say)

		bool ScrapWorldRef(const Protocol::WorkshopItem& a_item)
		{
			if (const auto it = own.find(a_item.refId); it != own.end()) {
				world.erase(it->second);
				own.erase(it);
			}
			return true;
		}

		bool OwnLoaded(const Protocol::WorkshopItem& a_item) const
		{
			const auto it = own.find(a_item.refId);
			return it != own.end() && loaded.contains(WorkshopCells::Exterior(COMMONWEALTH, world.at(it->second).at.position[0], world.at(it->second).at.position[1]));
		}

		std::optional<Copy> OwnPiece(const Protocol::WorkshopItem& a_item) const
		{
			const auto it = own.find(a_item.refId);
			const bool buildable = it != own.end() && WorkshopApply::Shared(true, world.at(it->second).base == FRAME);  // WorkshopSync::Buildable
			return buildable && !unmovable.contains(a_item.refId) ? std::optional<Copy>{ it->second } : std::nullopt;
		}

		// A piece the settlement came with, at the spot.
		void Own(std::uint32_t a_ref, float a_x, float a_y = 1000.0f, std::uint32_t a_base = WALL)
		{
			world[nextId] = { a_base, { { a_x, a_y, 0.0f }, { 0.0f, 0.0f, 1.5f }, 1.0f }, { WorkshopCells::Grid(a_x), WorkshopCells::Grid(a_y) } };
			own[a_ref] = nextId++;
		}

		std::optional<WorkshopApply::Standing> StandingOf(Copy a_copy) const
		{
			const auto it = world.find(a_copy);
			if (it == world.end()) {
				return std::nullopt;
			}
			return WorkshopApply::Standing{ it->second.base, it->second.at, it->second.cell };
		}

		void MoveTo(Copy a_copy, const Protocol::WorkshopItem& a_item) { world[a_copy].at = WorkshopMove::Of(a_item); }

		// WorkshopSync::Delete: what the player kept in it comes to them first, if WorkshopApply says so.
		void Discard(Copy a_copy)
		{
			const auto it = world.find(a_copy);
			if (it == world.end()) {
				return;
			}
			if (WorkshopApply::Rescues(KindOf(it->second.base), it->second.held > 0)) {
				inventory += it->second.held;
			}
			world.erase(it);
		}

		WorkshopApply::Kind KindOf(std::uint32_t a_base) const
		{
			const auto it = kinds.find(a_base);
			return it != kinds.end() ? it->second : WorkshopApply::Kind::kOther;
		}

		// Pieces this game's player built (linked to our workbench): never a copy to take over (WorkshopSync::IsWorkshopObject).
		std::set<int> built;

		bool SameAtSpot(const Piece& a_piece, const Protocol::WorkshopItem& a_item) const
		{
			return a_piece.base == a_item.base && std::hypot(a_piece.at.position[0] - a_item.position[0], a_piece.at.position[1] - a_item.position[1]) <= ADOPT_RADIUS;
		}

		// A piece of the same object at the spot that no copy of this run is, nor one we built.
		std::optional<Copy> FindExisting(const Protocol::WorkshopItem& a_item) const
		{
			for (const auto& [id, piece] : world) {
				const bool tracked = std::ranges::any_of(copies, [&](const auto& a_copy) { return a_copy.second == id; });
				if (!tracked && !built.contains(id) && SameAtSpot(piece, a_item)) {
					return id;
				}
			}
			return std::nullopt;
		}

		bool OursAt(const Protocol::WorkshopItem& a_item) const
		{
			return std::ranges::any_of(built, [&](int a_id) { return world.contains(a_id) && SameAtSpot(world.at(a_id), a_item); });
		}

		// A piece our player built, at the spot.
		int Build(float a_x, float a_y = 1000.0f)
		{
			world[nextId] = { WALL, { { a_x, a_y, 0.0f }, { 0.0f, 0.0f, 1.5f }, 1.0f }, { WorkshopCells::Grid(a_x), WorkshopCells::Grid(a_y) } };
			built.insert(nextId);
			return nextId++;
		}

		std::optional<Copy> Create(const Protocol::WorkshopItem& a_item)
		{
			const auto to = WorkshopMove::Of(a_item);
			world[nextId] = { a_item.base, to, { WorkshopCells::Grid(to.position[0]), WorkshopCells::Grid(to.position[1]) } };
			// A turret or a robot gets its gear from its record as it loads, and a container whose base comes stocked its things.
			world[nextId].held = KindOf(a_item.base) == WorkshopApply::Kind::kActor ? 3 : stocked.contains(a_item.base) ? 5 : 0;
			return nextId++;
		}

		void Empty(Copy a_copy) { world[a_copy].held = 0; }

		// ---- what WorkshopSync and Session do around it ----

		// Session.cpp: each kWorkshopItem and each item of a kWorkshopItems goes to WorkshopSync::Apply.
		void Receive(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kWorkshopItem)) {
					const auto item = Protocol::DecodeWorkshopItem(packet);
					REQUIRE(item);
					inbox.Arrive({ item->playerId, item->refId }, *item);
				} else if (packet[0] == Type(MT::kWorkshopItems)) {
					const auto items = Protocol::DecodeWorkshopItems(packet);
					REQUIRE(items);
					for (const auto& item : *items) {
						inbox.Arrive({ item.playerId, item.refId }, item);
					}
				}
			}
		}

		// WorkshopSync::Frame: what came in is placed, a few a frame. The time each takes is placeCost's.
		void Frame()
		{
			++frames;
			const auto done = inbox.Drain(WorkshopInbox::APPLY_PER_FRAME, WorkshopInbox::APPLY_TIME_PER_FRAME, [this] { return now; },
				[this](const Key& a_key, const Protocol::WorkshopItem& a_item) {
					now += placeCost();
					++applied;
					WorkshopApply::Apply(*this, copies, inbox, counts, a_key, a_item);
				});
			mostInAFrame = (std::max)(mostInAFrame, done);
		}

		void RunUntilIdle(int a_maxFrames = 1000)
		{
			for (int i = 0; i < a_maxFrames && inbox.Queued() > 0; ++i) {
				Frame();
			}
		}

		// A cell attaches (WorkshopSync::Frame): what waits in it, or next to it, gets in line again, and
		// what waits nowhere whose workbench has come.
		void Attach(const WorkshopCells::Where& a_cell)
		{
			loaded.insert(a_cell);
			inbox.Retry(inbox.Pending().Near(a_cell));
			inbox.Retry(WorkshopApply::FoundSomewhere(*this, inbox));
		}

		// Leaving the session (WorkshopSync::Reset).
		void Leave() { WorkshopApply::RemoveAll(*this, copies, inbox); }

		// A save is loaded (WorkshopSync::OnGameLoaded): the world is the save's again, and the handles of
		// this run's copies mean nothing; what waits in line or for its cell is kept.
		void Load(const std::map<int, Piece>& a_save)
		{
			world = a_save;
			copies.clear();
		}

		std::size_t At(float a_x, float a_y = 1000.0f) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(world, [&](const auto& a_piece) {
				return std::hypot(a_piece.second.at.position[0] - a_x, a_piece.second.at.position[1] - a_y) <= ADOPT_RADIUS;
			}));
		}

		bool AnySpotDoubled() const
		{
			return std::ranges::any_of(world, [&](const auto& a_piece) { return At(a_piece.second.at.position[0], a_piece.second.at.position[1]) > 1; });
		}
	};

	Game GameAt(float a_x, float a_y = 1000.0f)
	{
		Game game;
		game.loaded.insert(WorkshopCells::Exterior(COMMONWEALTH, a_x, a_y));
		return game;
	}
}

TEST("workshop session: a friend who loads a save gets the others' building back once, nothing doubled and nothing of their own")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Report(Wall(0xFF000002, 200.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 2);
	const auto save = bob.world;  // Bob saves, standing in the settlement

	// Alice keeps building; Bob sees it live.
	f.fake->Deliver(1, Report(Wall(0xFF000003, 300.0f)));
	f.fake->Deliver(2, Report(Wall(0xFF00B0B1, 400.0f)));  // Bob builds something of his own
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(300.0f) == 1);

	// Bob loads his save and asks for the session's changes, as Session::OnGameLoaded does.
	bob.Load(save);
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	const auto answer = Received(f, 2);
	bob.Receive(answer);
	bob.RunUntilIdle();

	// Alice's three pieces, each once: the two the save held taken over, the third made again.
	CHECK(bob.At(100.0f) == 1);
	CHECK(bob.At(200.0f) == 1);
	CHECK(bob.At(300.0f) == 1);
	CHECK(bob.copies.size() == 3);
	CHECK(!bob.AnySpotDoubled());
	CHECK(bob.counts.moved == 0);  // sent again unchanged: nothing is moved (each move marks the copy changed in his save)
	// His own piece is his game's own, not sent back to him as a copy.
	for (const auto& packet : answer) {
		if (!packet.empty() && packet[0] == Type(MT::kWorkshopItems)) {
			const auto items = Protocol::DecodeWorkshopItems(packet);  // named: the loop's range must outlive the loop
			for (const auto& item : *items) {
				CHECK(item.playerId == alice);
			}
		}
	}
	CHECK(bob.At(400.0f) == 0);
}

TEST("workshop session: our own piece where a friend's identical one stands (saves that share a past) stays ours, and no copy is put over it")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));
	// Both saves come from one: the wall built before they parted stands in both, linked to each one's workbench (and a
	// settler's job may hang on it). A copy an earlier session left is another matter: taken over, as before.
	const auto mine = bob.Build(100.0f);
	bob.world[bob.nextId++] = { WALL, { { 200.0f, 1000.0f, 0.0f }, { 0.0f, 0.0f, 1.5f }, 1.0f }, { 0, 0 } };

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));  // Alice's game tells of what she built before the session
	f.fake->Deliver(1, Report(Wall(0xFF000002, 200.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.counts.oursThere == 1);
	CHECK(bob.counts.adopted == 1);
	CHECK(bob.At(100.0f) == 1);  // not doubled
	CHECK(bob.copies.size() == 1);
	CHECK(std::ranges::none_of(bob.copies, [&](const auto& a_copy) { return a_copy.second == mine; }));

	// Alice scraps hers: it was never ours to remove. Bob leaves: his own piece stays in his world.
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF000001, 100.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.contains(mine));
	bob.Leave();
	CHECK(bob.world.contains(mine));
	CHECK(bob.At(200.0f) == 0);  // the adopted copy went with the session
}

TEST("workshop session: after a reload, a friend's piece moved or scrapped later still moves or goes")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));
	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Report(Wall(0xFF000002, 200.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	const auto save = bob.world;
	bob.Load(save);
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();

	// The copies the save held are known again: a move moves them, a scrap takes them away, and no
	// ghost is left at the old spot.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 150.0f)));
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF000002, 200.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(100.0f) == 0);
	CHECK(bob.At(150.0f) == 1);
	CHECK(bob.At(200.0f) == 0);
	CHECK(bob.world.size() == 1);
}

TEST("workshop session: a big settlement arrives a few pieces a frame, and a piece scrapped meanwhile never stays")
{
	constexpr std::uint32_t PIECES = 300;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	for (std::uint32_t i = 0; i < PIECES; ++i) {
		f.fake->Deliver(1, Report(Wall(0xFF100000 + i, 100.0f + static_cast<float>(i) * 10.0f)));
	}
	Received(f, 1);

	JoinAs(f, 2, "Bob");
	auto bob = GameAt(100.0f);  // the whole settlement is in one cell
	bob.Receive(Received(f, 2));
	REQUIRE(bob.inbox.Queued() == PIECES);
	// Alice scraps one of them while Bob's game is still placing the rest.
	bob.Frame();
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF100000 + 250, 2600.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();

	CHECK(bob.mostInAFrame <= WorkshopInbox::APPLY_PER_FRAME);
	CHECK(bob.frames >= static_cast<int>(PIECES / WorkshopInbox::APPLY_PER_FRAME));
	CHECK(bob.world.size() == PIECES - 1);
	CHECK(bob.At(2600.0f) == 0);
	CHECK(bob.At(2590.0f) == 1);
}

TEST("workshop session: a settlement of slow pieces never holds a frame long past its budget, and every frame still places some")
{
	constexpr std::uint32_t PIECES = 120;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	for (std::uint32_t i = 0; i < PIECES; ++i) {
		f.fake->Deliver(1, Report(Wall(0xFF200000 + i, 100.0f + static_cast<float>(i) * 10.0f)));
	}
	Received(f, 1);
	JoinAs(f, 2, "Bob");
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));
	REQUIRE(bob.inbox.Queued() == PIECES);

	// Pieces that take from a tenth of a millisecond to longer than a whole frame's budget each (a big
	// FindExisting, a havok object, a robot).
	const std::vector<Clock::duration> costs{ std::chrono::microseconds(100), std::chrono::microseconds(1500), std::chrono::microseconds(700),
		WorkshopInbox::APPLY_TIME_PER_FRAME * 3 };
	std::size_t     next = 0;
	Clock::duration last{};
	bob.placeCost = [&] { return last = costs[next++ % costs.size()]; };
	while (bob.inbox.Queued() > 0) {
		const auto placedBefore = bob.world.size();
		const auto start = bob.now;
		bob.Frame();
		const auto took = bob.now - start;
		CHECK(bob.world.size() > placedBefore);  // the line always moves
		// No piece is started once the frame's time is up: only the last one may run over.
		CHECK(took - last < WorkshopInbox::APPLY_TIME_PER_FRAME);
		REQUIRE(bob.frames < static_cast<int>(PIECES) + 1);
	}
	CHECK(bob.world.size() == PIECES);
	CHECK(!bob.AnySpotDoubled());
}

TEST("workshop session: a newer report for a piece waiting for its cell wins, whichever comes up first")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	Game bob;  // far from the settlement: nothing of it is loaded
	bob.Receive(Received(f, 2));
	const auto settlement = WorkshopCells::Exterior(COMMONWEALTH, 100.0f, 1000.0f);

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Report(Wall(0xFF000002, 200.0f)));
	f.fake->Deliver(1, Report(Wall(0xFF000003, 300.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());
	CHECK(bob.inbox.Pending().Size() == 3);

	// Alice moves the first and scraps the second; the reports arrive in the same frame the cell attaches,
	// before the waiting ones are tried again. The waiting ones are older and must not undo them.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 150.0f)));
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF000002, 200.0f))));
	bob.Receive(Received(f, 2));
	bob.Attach(settlement);
	bob.RunUntilIdle();

	CHECK(bob.At(100.0f) == 0);
	CHECK(bob.At(150.0f) == 1);
	CHECK(bob.At(200.0f) == 0);
	CHECK(bob.At(300.0f) == 1);
	CHECK(bob.inbox.Pending().Empty());
}

TEST("workshop session: a friend's building that came while its workbench wasn't loaded shows up once a cell brings the workbench")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	Game bob;  // far away: neither the settlement nor its workbench is loaded
	bob.benches.clear();
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());
	CHECK(bob.inbox.Pending().Nowhere().size() == 1);

	// Bob travels: a cell on the way attaches, and the next with the settlement and its workbench.
	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 30000.0f, 30000.0f));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());
	bob.benches.insert(BENCH);
	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 100.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.At(100.0f) == 1);
	CHECK(bob.inbox.Pending().Empty());
}

TEST("workshop session: building at a friend's mod settlement your game doesn't have waits quietly, instead of going through the line again at every cell you pass")
{
	constexpr std::uint32_t MOD_BENCH = 0x0A00BE7C;  // a workbench from a mod Bob doesn't have
	constexpr std::uint32_t PIECES = 200;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	Game bob;  // on the road: nothing of either settlement is loaded, nor the real one's workbench
	bob.benches.clear();
	bob.Receive(Received(f, 2));
	for (std::uint32_t i = 0; i < PIECES; ++i) {
		auto item = Wall(0xFF500000 + i, 100.0f + static_cast<float>(i) * 10.0f);
		item.workshop = MOD_BENCH;
		f.fake->Deliver(1, Report(item));
	}
	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.inbox.Pending().Nowhere().size() == PIECES + 1);

	// Bob travels through twenty cells: none brings anything up.
	const auto before = bob.applied;
	for (int i = 0; i < 20; ++i) {
		bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 30000.0f + static_cast<float>(i) * 4096.0f, 30000.0f));
		CHECK(bob.inbox.Queued() == 0);
		bob.RunUntilIdle();
	}
	CHECK(bob.applied == before);

	// He reaches the settlement he does have: its piece is placed, the mod's go on waiting.
	bob.benches.insert(BENCH);
	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 100.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.applied == before + 1);
	CHECK(bob.world.size() == 1);
	CHECK(bob.At(100.0f) == 1);
	CHECK(bob.inbox.Pending().Nowhere().size() == PIECES);
}

TEST("workshop session: a friend's save from an earlier run of the session keeps its copies after the host restarts")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-workshop-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	auto bob = GameAt(100.0f);
	{
		Fixture f{ options };
		JoinAs(f, 1, "Alice", 0xA11CE);
		JoinAs(f, 2, "Bob", 0xB0B);
		Received(f, 1);
		f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
		f.fake->Deliver(1, Report(Wall(0xFF000002, 200.0f)));
		bob.Receive(Received(f, 2));
		bob.RunUntilIdle();
	}  // the host stops (and saves the session); Bob quits, saving
	const auto save = bob.world;

	{
		Fixture f{ options };
		JoinAs(f, 1, "Alice", 0xA11CE);
		Received(f, 1);
		// Bob starts his game again from that save and joins the restarted session.
		Game again = GameAt(100.0f);
		again.Load(save);
		JoinAs(f, 2, "Bob", 0xB0B);
		again.Receive(Received(f, 2));
		again.RunUntilIdle();
		CHECK(again.At(100.0f) == 1);
		CHECK(again.At(200.0f) == 1);
		CHECK(again.copies.size() == 2);

		// And after loading that save once more, mid-session.
		again.Load(save);
		f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
		again.Receive(Received(f, 2));
		again.RunUntilIdle();
		CHECK(again.world.size() == 2);
		CHECK(again.copies.size() == 2);
	}
	std::filesystem::remove(path);
}

TEST("workshop session: a friend's moved piece moves for you, and one moved into the next cell is made again there with nothing left behind")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.loaded.insert(WorkshopCells::Exterior(COMMONWEALTH, 4200.0f, 1000.0f));
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 1);
	const auto first = bob.world.begin()->first;

	// Dragged across the room, then turned where it stands: the same copy follows.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 400.0f)));
	auto turned = Wall(0xFF000001, 400.0f);
	turned.rotation[2] = 3.0f;
	f.fake->Deliver(1, Report(turned));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 1);
	CHECK(bob.world.begin()->first == first);
	CHECK(bob.At(100.0f) == 0);
	CHECK(bob.At(400.0f) == 1);
	CHECK(bob.world.begin()->second.at.rotation[2] == 3.0f);
	CHECK(bob.counts.moved == 2);

	// Over the cell's edge: made again there, the old one gone.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 4200.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 1);
	CHECK(bob.world.begin()->first != first);
	CHECK(bob.At(400.0f) == 0);
	CHECK(bob.At(4200.0f) == 1);
	CHECK(bob.counts.recreated == 1);

	// And the scrap still finds the one made again.
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF000001, 4200.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());
}

TEST("workshop session: a friend who loads their own save and builds something else gets it shown, not their old piece slid over")
{
	constexpr std::uint32_t CHAIR = 0x0001F3C4;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));

	// Alice quicksaves, builds a wall, loads the quicksave and builds a chair: her game gives the chair
	// the wall's reference ID again.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	auto chair = Wall(0xFF000001, 300.0f);
	chair.base = CHAIR;
	f.fake->Deliver(1, Report(chair));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 1);
	CHECK(bob.world.begin()->second.base == CHAIR);
	CHECK(bob.At(100.0f) == 0);
	CHECK(bob.At(300.0f) == 1);

	// Someone joining later is sent the chair alone.
	JoinAs(f, 3, "Carol");
	auto carol = GameAt(100.0f);
	carol.Receive(Received(f, 3));
	carol.RunUntilIdle();
	REQUIRE(carol.world.size() == 1);
	CHECK(carol.world.begin()->second.base == CHAIR);
}

// ---- reports that overtake each other while a settlement isn't loaded ------------------------

namespace
{
	// Alice builds a settlement of a_pieces walls before Bob joins; Bob joins with nothing of it loaded,
	// his game takes in one frame of the welcome, and Alice scraps a_scrap (still in his line).
	void ScrapWhileAway(Fixture& a_f, Game& a_bob, std::uint32_t a_pieces, std::uint32_t a_scrap)
	{
		JoinAs(a_f, 1, "Alice");
		for (std::uint32_t i = 0; i < a_pieces; ++i) {
			a_f.fake->Deliver(1, Report(Wall(0xFF300000 + i, 100.0f + static_cast<float>(i) * 10.0f)));
		}
		Received(a_f, 1);
		JoinAs(a_f, 2, "Bob");
		a_bob.Receive(Received(a_f, 2));
		REQUIRE(a_bob.inbox.Queued() == a_pieces);
		a_bob.Frame();
		REQUIRE(a_bob.inbox.Queued() >= a_pieces - a_scrap);  // the scrapped one is still in line
		a_f.fake->Deliver(1, Report(Scrapped(Wall(0xFF300000 + a_scrap, 100.0f + static_cast<float>(a_scrap) * 10.0f))));
		a_bob.Receive(Received(a_f, 2));
		a_bob.RunUntilIdle();
		CHECK(a_bob.world.empty());
	}
}

TEST("workshop session: a piece a friend scraps while your game still takes in their settlement from afar isn't standing when you get there")
{
	constexpr std::uint32_t PIECES = 100, SCRAP = 80;
	Fixture                 f;
	Game                    bob;  // far from the settlement: nothing of it is loaded
	ScrapWhileAway(f, bob, PIECES, SCRAP);
	CHECK(bob.inbox.Pending().Size() == PIECES - 1);

	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 100.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.At(100.0f + SCRAP * 10.0f) == 0);
	CHECK(bob.At(100.0f + (SCRAP - 1) * 10.0f) == 1);
	CHECK(bob.world.size() == PIECES - 1);
	CHECK(bob.inbox.Pending().Empty());
}

TEST("workshop session: the same, with the settlement's workbench not loaded either")
{
	constexpr std::uint32_t PIECES = 100, SCRAP = 80;
	Fixture                 f;
	Game                    bob;
	bob.benches.clear();
	ScrapWhileAway(f, bob, PIECES, SCRAP);
	CHECK(bob.inbox.Pending().Nowhere().size() == PIECES - 1);

	bob.benches.insert(BENCH);
	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 100.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.At(100.0f + SCRAP * 10.0f) == 0);
	CHECK(bob.world.size() == PIECES - 1);
	CHECK(bob.inbox.Pending().Empty());
}

TEST("workshop session: a friend's piece moved out of a cell you haven't loaded, both reports in line at once, stays where they moved it when you get there")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);  // the settlement's first cell is loaded, the next one east isn't
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(0xFF000001, 4300.0f)));  // in the next cell
	f.fake->Deliver(1, Report(Wall(0xFF000001, 300.0f)));   // dragged back over the edge
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(300.0f) == 1);

	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 4300.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.At(4300.0f) == 0);
	CHECK(bob.At(300.0f) == 1);
	CHECK(bob.world.size() == 1);
	CHECK(bob.counts.recreated == 0);
	CHECK(bob.copies.contains({ alice, 0xFF000001 }));
}

TEST("workshop session: building still in line when you load a save neither doubles a piece nor puts a moved one back")
{
	constexpr std::uint32_t FIRST = 40, MORE = 60;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Receive(Received(f, 2));
	const auto at = [](std::uint32_t a_i) { return 100.0f + static_cast<float>(a_i) * 10.0f; };
	for (std::uint32_t i = 0; i < FIRST; ++i) {
		f.fake->Deliver(1, Report(Wall(0xFF400000 + i, at(i))));
	}
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	const auto save = bob.world;  // Bob saves with Alice's first forty standing

	// Alice builds sixty more and moves one of them; one frame of it is placed when Bob loads his save.
	for (std::uint32_t i = FIRST; i < FIRST + MORE; ++i) {
		f.fake->Deliver(1, Report(Wall(0xFF400000 + i, at(i))));
	}
	constexpr std::uint32_t MOVED = FIRST + MORE - 5;
	f.fake->Deliver(1, Report(Wall(0xFF400000 + MOVED, at(MOVED), 2000.0f)));
	bob.Receive(Received(f, 2));
	bob.Frame();
	REQUIRE(bob.inbox.Queued() > 0);
	bob.Load(save);
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();

	for (std::uint32_t i = 0; i < FIRST + MORE; ++i) {
		CHECK(bob.At(at(i), i == MOVED ? 2000.0f : 1000.0f) == 1);
	}
	CHECK(bob.At(at(MOVED)) == 0);
	CHECK(bob.world.size() == FIRST + MORE);
	CHECK(bob.copies.size() == FIRST + MORE);
	CHECK(bob.counts.adopted == FIRST);  // the save's copies taken over, not made again beside themselves
	CHECK(!bob.AnySpotDoubled());
}

// ---- a player sending what they shouldn't ---------------------------------------------------

TEST("workshop session: a joiner who claims to scrap the host's piece, or to build far out in the open world, changes nothing in a friend's game")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Mallory");
	JoinAs(f, 3, "Carol");
	Received(f, 1);
	Received(f, 2);
	auto carol = GameAt(100.0f);
	carol.loaded.insert(WorkshopCells::Exterior(COMMONWEALTH, 60000.0f, 1000.0f));
	carol.Receive(Received(f, 3));

	f.fake->Deliver(1, Report(Wall(0xFF000001, 100.0f)));
	carol.Receive(Received(f, 3));
	carol.RunUntilIdle();
	REQUIRE(carol.At(100.0f) == 1);

	// Mallory names Alice's reference ID: it is hers alone to scrap.
	f.fake->Deliver(2, Report(Scrapped(Wall(0xFF000001, 100.0f))));
	// And claims Alice's settlement for a wall of her own sixty thousand units away, then for one
	// moved there from inside it.
	f.fake->Deliver(2, Report(Wall(0xFF00BAD1, 60000.0f)));
	f.fake->Deliver(2, Report(Wall(0xFF00BAD2, 200.0f)));
	f.fake->Deliver(2, Report(Wall(0xFF00BAD2, 60000.0f)));
	carol.Receive(Received(f, 3));
	carol.RunUntilIdle();

	CHECK(carol.At(100.0f) == 1);
	CHECK(carol.copies.contains({ alice, 0xFF000001 }));
	CHECK(carol.At(60000.0f) == 0);
	CHECK(carol.At(200.0f) == 1);  // left where it stood
	CHECK(carol.counts.refused == 2);
}

// ---- what a copy held, when it goes ----------------------------------------------------------

TEST("workshop session: what you keep in a friend's container comes to you when it goes, but their turrets never hand you their gun and ammo")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.loaded.insert(WorkshopCells::Exterior(COMMONWEALTH, 4300.0f, 1000.0f));
	bob.Receive(Received(f, 2));

	const auto turret = [](std::uint32_t a_ref, float a_x) {
		auto item = Wall(a_ref, a_x);
		item.base = TURRET;
		return item;
	};
	auto crate = Wall(0xFF000010, 500.0f);
	crate.base = CRATE;
	f.fake->Deliver(1, Report(turret(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Report(turret(0xFF000002, 200.0f)));
	f.fake->Deliver(1, Report(turret(0xFF000003, 300.0f)));
	f.fake->Deliver(1, Report(crate));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 4);
	bob.world[bob.copies.at({ alice, crate.refId })].held += 5;  // Bob keeps his stash in it

	// Alice scraps one turret and drags another into the next cell (made again there).
	f.fake->Deliver(1, Report(Scrapped(turret(0xFF000001, 100.0f))));
	f.fake->Deliver(1, Report(turret(0xFF000002, 4300.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.counts.recreated == 1);
	CHECK(bob.inventory == 0);

	// She drags the crate into the next cell too: its copy there is a new one, and Bob's stash comes to him.
	crate.position[0] = 4400.0f;
	f.fake->Deliver(1, Report(crate));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.inventory == 5);
	CHECK(bob.world.at(bob.copies.at({ alice, crate.refId })).held == 0);

	// He keeps a little in the new one, then leaves the session: every copy goes, and only the crate's
	// contents come with him.
	bob.world[bob.copies.at({ alice, crate.refId })].held += 2;
	bob.Leave();
	CHECK(bob.world.empty());
	CHECK(bob.inventory == 7);
}

TEST("workshop session: a friend's container that comes stocked is copied empty, as the workshop builds it, and a full one an older version left in your save is replaced")
{
	constexpr std::uint32_t VENDOR = 0x0010C3D0;  // a container whose base holds things of its own
	Fixture                 f;
	const auto              alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.stocked.insert(VENDOR);
	bob.kinds[VENDOR] = WorkshopApply::Kind::kContainer;
	bob.Receive(Received(f, 2));

	// A copy an older version made of Alice's vending machine stands in Bob's save, with what he put in it.
	auto vendor = Wall(0xFF000001, 300.0f);
	vendor.base = VENDOR;
	bob.Load({ { 99, { VENDOR, WorkshopMove::Of(vendor), { 0, 0 }, 4 } } });
	f.fake->Deliver(1, Report(vendor));
	auto fresh = Wall(0xFF000002, 600.0f);  // and one she builds now
	fresh.base = VENDOR;
	f.fake->Deliver(1, Report(fresh));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();

	REQUIRE(bob.copies.contains({ alice, vendor.refId }));
	REQUIRE(bob.copies.contains({ alice, fresh.refId }));
	CHECK(bob.world.size() == 2);  // the old copy went: a new one stands at its spot
	CHECK(!bob.world.contains(99));
	CHECK(bob.world.at(bob.copies.at({ alice, vendor.refId })).held == 0);  // none of what the base comes with: no loot
	CHECK(bob.world.at(bob.copies.at({ alice, fresh.refId })).held == 0);
	CHECK(bob.inventory == 4);  // what he had in the old copy came to him
	CHECK(bob.counts.emptied == 2);
	CHECK(bob.counts.refused == 0);

	// What he keeps in the new one comes to him when it goes; nothing of the base's does.
	bob.world[bob.copies.at({ alice, fresh.refId })].held += 2;
	bob.Leave();
	CHECK(bob.world.empty());
	CHECK(bob.inventory == 6);
}

TEST("workshop session: a piece of the world a friend moves moves in your world, with no copy of it left beside the original")
{
	constexpr std::uint32_t BED = 0x0001ABCD;  // a bed the settlement came with: the same reference in every game
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Own(BED, 100.0f);
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(BED, 150.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(100.0f) == 0);
	CHECK(bob.At(150.0f) == 1);
	CHECK(bob.world.size() == 1);
	CHECK(bob.copies.empty());
	CHECK(bob.counts.movedOwn == 1);
	CHECK(bob.counts.placed == 0);

	// Moved again, then scrapped: ours follows, then goes.
	f.fake->Deliver(1, Report(Wall(BED, 250.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(250.0f) == 1);
	CHECK(bob.world.size() == 1);
	f.fake->Deliver(1, Report(Scrapped(Wall(BED, 250.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());

	// Someone joining later is sent the scrap alone: nothing of the bed stands for them.
	JoinAs(f, 3, "Carol");
	auto carol = GameAt(100.0f);
	carol.Own(BED, 100.0f);
	carol.Receive(Received(f, 3));
	carol.RunUntilIdle();
	CHECK(carol.world.empty());
}

TEST("workshop session: a piece of the world moved where yours isn't loaded yet waits, and one you may not move stays put")
{
	constexpr std::uint32_t BED = 0x0001ABCD;
	constexpr std::uint32_t CROP = 0x0001ABCE;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	bob.Own(BED, 4200.0f);  // in the next cell, not loaded yet
	bob.Own(CROP, 300.0f);
	bob.unmovable.insert(CROP);  // not that workbench's here, say
	bob.Receive(Received(f, 2));

	f.fake->Deliver(1, Report(Wall(BED, 4210.0f)));
	f.fake->Deliver(1, Report(Wall(CROP, 350.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.At(300.0f) == 1);  // the crop stays where it stood
	CHECK(bob.At(350.0f) == 0);
	CHECK(bob.counts.ownRefused == 1);
	CHECK(bob.At(4200.0f) == 1);  // the bed waits for its cell
	CHECK(bob.copies.empty());

	bob.Attach(WorkshopCells::Exterior(COMMONWEALTH, 4200.0f, 1000.0f));
	bob.RunUntilIdle();
	CHECK(bob.At(4200.0f) == 0);
	CHECK(bob.At(4210.0f) == 1);
	CHECK(bob.world.size() == 2);
	CHECK(bob.copies.empty());
}

TEST("workshop session: a power armor frame a friend's older game reports from workshop mode gets no copy, the copy an older version left goes, and nothing of ours is moved or taken")
{
	constexpr std::uint32_t PARKED = 0x0001F00D;  // a frame the world came with: the same reference in every game
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	auto bob = GameAt(100.0f);
	// Bob's save holds the workshop copy 0.9.1 made of a frame Alice set down at 300 (with the keyword it gave its copies),
	// and his own suit, stripped, at 600, where Alice had set one down too.
	auto left = Wall(0xFF000020, 300.0f);
	left.base = FRAME;
	auto suit = Wall(0xFF000022, 600.0f);
	suit.base = FRAME;
	bob.Load({ { 99, { FRAME, WorkshopMove::Of(left), { 0, 0 } } }, { 98, { FRAME, WorkshopMove::Of(suit), { 0, 0 } } } });
	bob.blocked.insert(99);
	bob.Own(PARKED, 700.0f, 1000.0f, FRAME);
	bob.Receive(Received(f, 2));

	// Alice's 0.9.1 game reports both frames (her own, or her copies of Bob's), the world's frame moved, and a wall.
	f.fake->Deliver(1, Report(left));
	f.fake->Deliver(1, Report(suit));
	auto parked = Wall(PARKED, 750.0f);
	parked.base = FRAME;
	f.fake->Deliver(1, Report(parked));
	f.fake->Deliver(1, Report(Wall(0xFF000021, 900.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();

	CHECK(bob.At(300.0f) == 0);  // the old copy went
	CHECK(!bob.world.contains(99));
	CHECK(bob.counts.framesCleared == 1);
	CHECK(bob.At(600.0f) == 1);  // his suit stays his: neither taken over nor deleted
	CHECK(bob.world.contains(98));
	CHECK(bob.At(700.0f) == 1);  // his frame stays where he parked it
	CHECK(bob.At(750.0f) == 0);
	CHECK(bob.counts.ownRefused == 1);
	CHECK(bob.At(900.0f) == 1);
	CHECK(bob.counts.placed == 1);
	CHECK(bob.counts.adopted == 0);
	CHECK(bob.copies.size() == 1);
	CHECK(bob.world.size() == 3);

	// The server keeps what it was told; a load, or someone joining later, makes no frame of it either.
	const auto save = bob.world;
	bob.Load(save);
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.size() == 3);
	CHECK(bob.world.contains(98));
	CHECK(bob.counts.framesCleared == 1);
	CHECK(!bob.AnySpotDoubled());
	bob.Leave();
	CHECK(bob.world.contains(98));  // nor does the session's end take it
	JoinAs(f, 3, "Carol");
	auto carol = GameAt(100.0f);
	carol.Receive(Received(f, 3));
	carol.RunUntilIdle();
	REQUIRE(carol.world.size() == 1);
	CHECK(carol.world.begin()->second.base == WALL);
}

TEST("workshop apply: a power armor frame is never a workshop piece, though workshop mode moves it")
{
	CHECK(WorkshopApply::Shared(true, false));
	CHECK(!WorkshopApply::Shared(true, true));
	CHECK(!WorkshopApply::Shared(false, false));
}

TEST("workshop session: a wire's shape goes to the others with it, and to whoever joins later")
{
	constexpr std::uint32_t WIRE = 0x0001D971;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Received(f, 1);
	Received(f, 2);

	auto wire = Wall(0xFF000FCA, 150.0f);
	wire.base = WIRE;
	wire.wire = Protocol::WireShape{ { -62.7f, 24.0f, 37.8f }, 0.05f, 1.5f };
	f.fake->Deliver(1, Report(wire));
	auto moved = wire;  // one of its ends moved: the game strung it again, its middle and shape changed
	moved.position[0] = 170.0f;
	moved.wire->halfExtents[0] = 96.8f;
	f.fake->Deliver(1, Report(moved));

	const auto heard = [](const Packets& a_packets) {
		std::vector<Protocol::WorkshopItem> items;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(MT::kWorkshopItem)) {
				items.push_back(*Protocol::DecodeWorkshopItem(packet));
			} else if (!packet.empty() && packet[0] == Type(MT::kWorkshopItems)) {
				const auto batch = Protocol::DecodeWorkshopItems(packet);
				items.insert(items.end(), batch->begin(), batch->end());
			}
		}
		return items;
	};
	const auto bob = heard(Received(f, 2));
	REQUIRE(bob.size() == 2);
	CHECK(bob[0].wire == wire.wire);
	CHECK(bob[1].wire == moved.wire);

	JoinAs(f, 3, "Carol");
	const auto carol = heard(Received(f, 3));
	REQUIRE(carol.size() == 1);
	CHECK(carol[0].wire == moved.wire);
	CHECK(carol[0].position[0] == 170.0f);
}

TEST("workshop apply: only a container's contents are handed over when its copy goes, never an actor's")
{
	using WorkshopApply::Kind;
	CHECK(WorkshopApply::Rescues(Kind::kContainer, true));
	CHECK(!WorkshopApply::Rescues(Kind::kContainer, false));
	CHECK(!WorkshopApply::Rescues(Kind::kActor, true));  // a turret's ammo, a robot's parts
	CHECK(!WorkshopApply::Rescues(Kind::kOther, true));
}

// ---- WorkshopMove's rules, alone ------------------------------------------------------------

TEST("workshop move: a report that changes nothing (within half a unit, a hundredth of a radian) moves nothing")
{
	WorkshopMove::Transform at{ { 100.0f, 200.0f, 50.0f }, { 0.0f, 0.0f, 0.001f }, 1.0f };
	auto                    to = at;
	to.position[0] += 0.3f;
	to.rotation[2] = 2.0f * std::numbers::pi_v<float> - 0.002f;  // the same heading, written a full turn on
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ 0, 0 }) == WorkshopMove::How::kStay);
	to.scale = 1.5f;  // only bigger
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ 0, 0 }) == WorkshopMove::How::kInPlace);
	to = at;
	to.rotation[0] = 0.5f;  // tilted
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ 0, 0 }) == WorkshopMove::How::kInPlace);
}

TEST("workshop move: a copy changes cells only by being made again, below zero too; an interior is all one cell")
{
	const WorkshopMove::Transform at{ { -10.0f, 100.0f, 0.0f }, {}, 1.0f };
	auto                          to = at;
	to.position[0] = -4000.0f;  // still cell -1
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ -1, 0 }) == WorkshopMove::How::kInPlace);
	to.position[0] = 10.0f;  // cell 0
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ -1, 0 }) == WorkshopMove::How::kRecreate);
	to.position[0] = -10.0f;
	to.position[1] = -10.0f;  // cell -1, -1
	CHECK(WorkshopMove::HowToMove(at, to, std::pair{ -1, 0 }) == WorkshopMove::How::kRecreate);
	to.position[0] = 90000.0f;
	CHECK(WorkshopMove::HowToMove(at, to, std::nullopt) == WorkshopMove::How::kInPlace);
}
