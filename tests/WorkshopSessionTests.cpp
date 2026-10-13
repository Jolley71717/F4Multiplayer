// Settlement building across a session, end to end without the game: a real Server, real Protocol
// messages from two or three players, and what the server sends fed into the client's real inbox
// (game/WorkshopInbox.h). The engine side of a friend's game is a small stand-in (Game below): the
// pieces standing in its world, the copies it knows are copies, and which cells are loaded.

#include "ServerFixture.h"

#include "game/WorkshopInbox.h"
#include "game/WorkshopMove.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
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

	// Returns once the server has handled everything delivered so far (it answers a heartbeat in order).
	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	Packets Received(Fixture& a_f, PeerId a_peer)
	{
		Sync(a_f, a_peer);
		return a_f.fake->Take(a_peer);
	}

	std::uint32_t JoinAs(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_identity = 0)
	{
		const auto welcome = a_f.Join(a_peer, Fixture::MakeHello(std::move(a_name), a_identity));
		REQUIRE(welcome);
		return welcome->playerId;
	}

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

	// A friend's game, as far as building goes. Its rules for a report follow WorkshopSync::ApplyNow: a
	// scrap takes the copy away; a piece whose cell isn't loaded waits for it; a piece with a copy moves
	// it (WorkshopMove: within its cell, or made again in the next); otherwise a copy the save left at the
	// same spot is taken over, or a new one made.
	struct Game
	{
		struct Piece
		{
			std::uint32_t                          base;
			WorkshopMove::Transform                at;
			std::pair<std::int32_t, std::int32_t> cell;  // the cell it was made in: the engine keeps it there
		};

		static constexpr float ADOPT_RADIUS = 4.0f;  // a copy the save left counts if it stands this close

		std::map<int, Piece>                               world;   // what stands in the world, by an ID of the stand-in's own
		std::map<Key, int>                                 copies;  // the copies this run knows, as handles
		std::set<WorkshopCells::Where>                     loaded;
		WorkshopInbox::Inbox<Key, Protocol::WorkshopItem> inbox;
		int                                                nextId = 1;
		Clock::time_point                                  now{};
		Clock::duration                                    placeCost = std::chrono::microseconds(10);
		std::size_t                                        mostInAFrame = 0;
		int                                                frames = 0;
		int                                                moves = 0;        // copies moved where they stand (each marks it changed in the save)
		int                                                recreations = 0;  // copies made again in another cell

		static WorkshopCells::Where Cell(const Protocol::WorkshopItem& a_item) { return WorkshopCells::Exterior(COMMONWEALTH, a_item.position[0], a_item.position[1]); }

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

		void ApplyNow(const Key& a_key, const Protocol::WorkshopItem& a_item)
		{
			now += placeCost;
			if (a_item.op == Protocol::WorkshopOp::kScrapped) {
				if (const auto it = copies.find(a_key); it != copies.end()) {
					world.erase(it->second);
					copies.erase(it);
				}
				return;
			}
			if (!loaded.contains(Cell(a_item))) {
				inbox.Wait(Cell(a_item), a_key, a_item);
				return;
			}
			const auto to = WorkshopMove::Of(a_item);
			const auto move = [&](int a_id) {
				auto& piece = world[a_id];
				switch (WorkshopMove::HowToMove(piece.at, to, piece.cell)) {
				case WorkshopMove::How::kStay:
					return true;
				case WorkshopMove::How::kInPlace:
					piece.at = to;
					++moves;
					return true;
				default:
					world.erase(a_id);
					++recreations;
					return false;
				}
			};
			if (const auto it = copies.find(a_key); it != copies.end()) {
				const auto id = it->second;
				copies.erase(it);
				if (move(id)) {
					copies[a_key] = id;
					return;
				}
			}
			for (const auto& [id, piece] : world) {
				const bool tracked = std::ranges::any_of(copies, [&](const auto& a_copy) { return a_copy.second == id; });
				if (!tracked && piece.base == a_item.base && std::hypot(piece.at.position[0] - to.position[0], piece.at.position[1] - to.position[1]) <= ADOPT_RADIUS) {
					const auto found = id;
					if (move(found)) {
						copies[a_key] = found;
						return;
					}
					break;
				}
			}
			copies[a_key] = nextId;
			world[nextId++] = { a_item.base, to, { WorkshopCells::Grid(to.position[0]), WorkshopCells::Grid(to.position[1]) } };
		}

		// WorkshopSync::Frame: what came in is placed, a few a frame.
		void Frame()
		{
			++frames;
			const auto done = inbox.Drain(WorkshopInbox::APPLY_PER_FRAME, WorkshopInbox::APPLY_TIME_PER_FRAME, [this] { return now; },
				[this](const Key& a_key, const Protocol::WorkshopItem& a_item) { ApplyNow(a_key, a_item); });
			mostInAFrame = (std::max)(mostInAFrame, done);
		}

		void RunUntilIdle(int a_maxFrames = 1000)
		{
			for (int i = 0; i < a_maxFrames && inbox.Queued() > 0; ++i) {
				Frame();
			}
		}

		// A cell attaches: what waits in it, or next to it, gets in line again.
		void Attach(const WorkshopCells::Where& a_cell)
		{
			loaded.insert(a_cell);
			inbox.Retry(inbox.Pending().Near(a_cell));
		}

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
	CHECK(bob.moves == 0);  // sent again unchanged: nothing is moved (each move marks the copy changed in his save)
	// His own piece is his game's own, not sent back to him as a copy.
	for (const auto& packet : answer) {
		if (!packet.empty() && packet[0] == Type(MT::kWorkshopItems)) {
			for (const auto& item : *Protocol::DecodeWorkshopItems(packet)) {
				CHECK(item.playerId == alice);
			}
		}
	}
	CHECK(bob.At(400.0f) == 0);
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

TEST("workshop session: placing stops for the frame once its time is up, and the line still moves")
{
	auto bob = GameAt(100.0f);
	for (std::uint32_t i = 0; i < 20; ++i) {
		auto item = Wall(0xFF000001 + i, 100.0f + static_cast<float>(i) * 10.0f);
		item.playerId = 1;
		bob.inbox.Arrive({ 1, item.refId }, item);
	}
	bob.placeCost = std::chrono::milliseconds(1);  // a slow piece (a big FindExisting, a havok object)
	bob.Frame();
	CHECK(bob.world.size() == 2);  // 2 ms a frame
	bob.placeCost = std::chrono::milliseconds(50);  // one that takes longer than a frame's budget by itself
	bob.Frame();
	CHECK(bob.world.size() == 3);  // still one: a slow piece can't stall the rest for good
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
	CHECK(bob.moves == 2);

	// Over the cell's edge: made again there, the old one gone.
	f.fake->Deliver(1, Report(Wall(0xFF000001, 4200.0f)));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	REQUIRE(bob.world.size() == 1);
	CHECK(bob.world.begin()->first != first);
	CHECK(bob.At(400.0f) == 0);
	CHECK(bob.At(4200.0f) == 1);
	CHECK(bob.recreations == 1);

	// And the scrap still finds the one made again.
	f.fake->Deliver(1, Report(Scrapped(Wall(0xFF000001, 4200.0f))));
	bob.Receive(Received(f, 2));
	bob.RunUntilIdle();
	CHECK(bob.world.empty());
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
