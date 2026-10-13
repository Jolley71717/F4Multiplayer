// Which friends outside get a stand-in here, and what becomes of a stand-in whose cell the game stops drawing
// (game/StandInRange.h). The rules on their own first, then a friend walking about as our game hears of them through a
// real server, each frame decided by the function RemotePlayers asks, with the engine's loaded cells played by a model.

#include "ServerFixture.h"

#include "game/StandInRange.h"

#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using StandInRange::Cell;
	using StandInRange::Left;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t HOUSE = 0x0001F2A4;  // an interior cell
	constexpr int           GRIDS = 5;           // the game's default uGridsToLoad

	StandInRange::LeftFacts Facts(Cell a_cell, bool a_inGrid = true, bool a_drawn = false, Clock::duration a_sinceSpawned = 100ms, bool a_weInside = false)
	{
		return { .weInside = a_weInside, .cell = a_cell, .cellInGrid = a_inGrid, .drawn = a_drawn, .sinceSpawned = a_sinceSpawned };
	}
}

TEST("stand-in range: a friend past the cells the game has loaded around us is not drawn, even within 9,000 units")
{
	// We stand 96 units inside our cell's east edge: the loaded square ends two cells further east, at 12,288.
	CHECK(StandInRange::Visible(12287.0f, 2000.0f, 4000.0f, 2000.0f, GRIDS));
	CHECK(!StandInRange::Visible(12288.0f, 2000.0f, 4000.0f, 2000.0f, GRIDS));
	// The reported spot: 8,500 units off, inside the old 9,000 radius, one cell past the loaded square.
	CHECK(!StandInRange::Visible(12500.0f, 2000.0f, 4000.0f, 2000.0f, GRIDS));
	// The same distance the other way, where the square reaches further, is drawn.
	CHECK(StandInRange::Visible(-4500.0f, 2000.0f, 4000.0f, 2000.0f, GRIDS));
	// Near our cell's west edge, 8,950 units east is still in the square.
	CHECK(StandInRange::Visible(9050.0f, 0.0f, 100.0f, 0.0f, GRIDS));
	// In the square but past 9,000 units (a far corner): not drawn either.
	CHECK(!StandInRange::Visible(12000.0f, 12000.0f, 4000.0f, 4000.0f, GRIDS));
}

TEST("stand-in range: cells below zero are counted from their lower edge")
{
	// We stand in cell -1 (x from -4,096 to -1); the square reaches down to cell -3, which starts at -12,288.
	CHECK(StandInRange::Visible(-12288.0f, 0.0f, -4000.0f, 0.0f, GRIDS));
	CHECK(!StandInRange::Visible(-12289.0f, 0.0f, -4000.0f, 0.0f, GRIDS));  // cell -4: rounding toward zero would say -3
	// Just below zero is cell -1, a neighbour of cell 0, not cell 0 itself.
	CHECK(StandInRange::InLoadedGrid(-1, 0, 0, 0, 3));
	CHECK(!StandInRange::InLoadedGrid(-2, 0, 0, 0, 3));
}

TEST("stand-in range: a bigger uGridsToLoad draws friends further out, and a broken one counts as the default")
{
	CHECK(StandInRange::LoadedRadius(5) == 2);
	CHECK(StandInRange::LoadedRadius(7) == 3);
	CHECK(StandInRange::LoadedRadius(3) == 1);
	CHECK(StandInRange::LoadedRadius(0) == 2);  // unreadable: the default
	CHECK(StandInRange::LoadedRadius(-3) == 2);
	CHECK(StandInRange::LoadedRadius(1) == 1);  // never less than our cell and its neighbours
	CHECK(StandInRange::LoadedRadius(4) == 1);  // an even one loads the odd square below it
	// With 7 the reported spot is in the square; with 3 a friend two cells off is not.
	CHECK(StandInRange::Visible(12500.0f, 2000.0f, 4000.0f, 2000.0f, 7));
	CHECK(!StandInRange::Visible(8200.0f, 2000.0f, 4000.0f, 2000.0f, 3));
}

TEST("stand-in range: a stand-in in a neighbouring cell the game draws, or is bringing in, stays")
{
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kAttached)) == Left::kKeep);
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kAttached, false, true, 1h)) == Left::kKeep);
	// We crossed a cell edge and the grid shifted: its new cell attaches over some frames.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kOnItsWay)) == Left::kKeep);
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kOnItsWay, true, false, 9s)) == Left::kKeep);
}

TEST("stand-in range: a stand-in the game never drew where it stands is replaced, and none is placed for a moment")
{
	// A cell outside the loaded square, held in the game's buffer, never attaches while we stand here.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kOnItsWay, false)) == Left::kReplaceLater);
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kGone)) == Left::kReplaceLater);
	// A cell that should be coming in but has not in 10 s.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kOnItsWay, true, false, 11s)) == Left::kReplaceLater);
	CHECK(!StandInRange::MayRespawn(999ms));
	CHECK(StandInRange::MayRespawn(1s));
}

TEST("stand-in range: a stand-in left behind a door, or in a cell we walked away from, is replaced at once")
{
	// We went inside: whatever cell outside it stands in.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kAttached, true, true, 1min, true)) == Left::kReplace);
	// We came out and it is still in the interior, or in no cell at all.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kInterior, false, true)) == Left::kReplace);
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kNone, false)) == Left::kReplace);
	// Drawn where it was placed, and that cell has since dropped out as we walked on with them: no wait.
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kGone, false, true, 30s)) == Left::kReplace);
	CHECK(StandInRange::ChooseLeft(Facts(Cell::kOnItsWay, false, true, 30s)) == Left::kReplace);
}

// ---- a friend walking about, as our game hears of them through the server ----
// Each frame goes through StandInRange::Frame, the function RemotePlayers::Update asks; what the test plays is the
// engine: which cells it has attached (by the engine's own CELL_STATE numbers) and whether a stand-in has its 3D.

namespace
{
	using StandInRange::CellState;

	// The engine's side: the exterior cells it has attached, a square centred on its own grid cell. That follows our
	// player's cell, but can lag behind it while it loads after a cell edge (lagging); cells just outside it sit loaded in
	// its buffer, and cells further out are not loaded.
	struct Engine
	{
		int                           grids = GRIDS;
		bool                          lagging = false;
		std::int32_t                  centreX = 0;
		std::int32_t                  centreY = 0;
		std::set<std::pair<int, int>> stuck;  // cells in the square that stay loaded and never attach

		CellState StateOf(std::int32_t a_x, std::int32_t a_y) const
		{
			if (StandInRange::InLoadedGrid(a_x, a_y, centreX, centreY, grids)) {
				return stuck.contains({ a_x, a_y }) ? CellState::kLoaded : CellState::kAttached;
			}
			return StandInRange::InLoadedGrid(a_x, a_y, centreX, centreY, grids + 2) ? CellState::kLoaded : CellState::kNotLoaded;
		}
	};

	// Our game, as far as one friend's stand-in goes, frame by frame: it does what StandInRange::Frame says, as
	// RemotePlayers::Update does. A stand-in stays in the cell it was placed in while it walks.
	struct Ours
	{
		struct StandIn
		{
			std::uint32_t interior = 0;
			std::int32_t  x = 0;
			std::int32_t  y = 0;
		};

		Engine                 engine;
		float                  x = 0.0f;
		float                  y = 0.0f;
		std::uint32_t          interior = 0;  // our player's interior cell; 0 outside
		std::optional<StandIn> standIn;
		StandInRange::Track    track;
		Clock::time_point      now = Clock::time_point{} + 1h;
		int                    spawns = 0;
		int                    leftBehind = 0;
		int                    respawnWaits = 0;

		bool Drawn() const
		{
			return standIn && (standIn->interior ? standIn->interior == interior : !interior && engine.StateOf(standIn->x, standIn->y) == CellState::kAttached);
		}

		// The stand-in as RemotePlayers::StandInOf reads it from the engine.
		std::optional<StandInRange::StandIn> Read() const
		{
			if (!standIn) {
				return std::nullopt;
			}
			StandInRange::StandIn read;
			read.inCell = true;
			read.interior = standIn->interior != 0;
			read.inOurCell = standIn->interior ? standIn->interior == interior : !interior && standIn->x == WorkshopCells::Grid(x) && standIn->y == WorkshopCells::Grid(y);
			read.state = standIn->interior ? (standIn->interior == interior ? CellState::kAttached : CellState::kNotLoaded) : engine.StateOf(standIn->x, standIn->y);
			read.x = standIn->interior ? 0 : standIn->x;
			read.y = standIn->interior ? 0 : standIn->y;
			read.has3D = Drawn();
			return read;
		}

		void Frame(const std::optional<Protocol::PlayerState>& a_friend)
		{
			now += 16ms;
			if (!engine.lagging) {
				engine.centreX = WorkshopCells::Grid(x);
				engine.centreY = WorkshopCells::Grid(y);
			}
			StandInRange::Sight sight;
			sight.sameSpace = a_friend && (interior ? a_friend->cell == interior : a_friend->cell == 0 && a_friend->worldspace == COMMONWEALTH);
			if (sight.sameSpace) {
				sight.weInside = interior != 0;
				sight.x = a_friend->x;
				sight.y = a_friend->y;
				sight.ourX = x;
				sight.ourY = y;
				sight.gridsToLoad = engine.grids;
				sight.standIn = Read();
			}
			const auto look = StandInRange::Frame(track, sight, now);
			if (!look.visible) {
				standIn.reset();
				return;
			}
			if (standIn && look.left != Left::kKeep) {
				++leftBehind;
				respawnWaits += look.left == Left::kReplaceLater;
				standIn.reset();
			}
			if (!standIn) {
				if (!look.mayPlace) {
					return;
				}
				standIn = StandIn{ a_friend->cell, a_friend->cell ? 0 : WorkshopCells::Grid(a_friend->x), a_friend->cell ? 0 : WorkshopCells::Grid(a_friend->y) };
				++spawns;
				StandInRange::Placed(track, now);
			}
		}

		void Frames(const std::optional<Protocol::PlayerState>& a_friend, int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				Frame(a_friend);
			}
		}

		bool Shown() const { return Drawn(); }
	};

	// Alice (peer 2) and Bob (peer 1), both in. Bob's game is the one whose stand-ins are looked at.
	struct Session
	{
		Fixture       f;
		std::uint32_t alice = 0;
		std::uint32_t sequence = 0;

		Session()
		{
			REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
			const auto welcome = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
			REQUIRE(welcome);
			alice = welcome->playerId;
		}

		static Protocol::PlayerState Outside(float a_x, float a_y)
		{
			Protocol::PlayerState state;
			state.worldspace = COMMONWEALTH;
			state.x = a_x;
			state.y = a_y;
			return state;
		}

		// Alice's game sends a state; what Bob's game is told of her.
		std::optional<Protocol::PlayerState> Send(Protocol::PlayerState a_state)
		{
			a_state.sequence = ++sequence;
			std::this_thread::sleep_for(20ms);  // her game sends 30 a second; the server takes 60 at most
			f.fake->Deliver(2, Protocol::Encode(a_state));
			while (const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerStates), 2000ms)) {
				const auto batch = Protocol::DecodePlayerStates(*packet);
				REQUIRE(batch);
				for (const auto& remote : batch->players) {
					if (remote.playerId == alice && remote.state.sequence == a_state.sequence) {
						return remote.state;
					}
				}
			}
			return std::nullopt;
		}
	};

	// A friend standing in a neighbouring cell, in our square, as a frame sees them, their stand-in there in the state a_state.
	StandInRange::Sight Neighbour(CellState a_state, bool a_has3D)
	{
		StandInRange::Sight sight;
		sight.sameSpace = true;
		sight.x = 6000.0f;  // cell 1
		sight.y = 2000.0f;
		sight.ourX = 2000.0f;  // cell 0
		sight.ourY = 2000.0f;
		sight.standIn = StandInRange::StandIn{ .inCell = true, .state = a_state, .x = 1, .y = 0, .has3D = a_has3D };
		return sight;
	}
}

TEST("stand-in range: a friend's stand-in in a cell the engine is still bringing in waits for it; one in a cell it is letting go of goes")
{
	const auto placed = Clock::time_point{} + 1h;
	// Each of the engine's own cell states, for a stand-in placed a moment ago and not drawn yet.
	for (const auto state : { CellState::kLoadingData, CellState::kLoading, CellState::kLoaded, CellState::kAttachQueued, CellState::kAttaching, CellState::kAttached }) {
		StandInRange::Track track;
		StandInRange::Placed(track, placed);
		const auto look = StandInRange::Frame(track, Neighbour(state, false), placed + 200ms);
		CHECK(look.visible);
		CHECK(look.left == Left::kKeep);
	}
	for (const auto state : { CellState::kNotLoaded, CellState::kUnloading, CellState::kDetaching }) {
		StandInRange::Track track;
		StandInRange::Placed(track, placed);
		const auto look = StandInRange::Frame(track, Neighbour(state, false), placed + 200ms);
		CHECK(look.left == Left::kReplaceLater);  // never drawn there: none placed again for a moment
		CHECK(!look.mayPlace);
	}
	// Drawn there once (its 3D came in a frame before), then the cell went: replaced at once.
	StandInRange::Track track;
	StandInRange::Placed(track, placed);
	CHECK(StandInRange::Frame(track, Neighbour(CellState::kAttached, true), placed + 100ms).left == Left::kKeep);
	const auto look = StandInRange::Frame(track, Neighbour(CellState::kDetaching, false), placed + 200ms);
	CHECK(look.left == Left::kReplace);
	CHECK(look.mayPlace);
}

TEST("stand-in range: a friend who walks out past the loaded cells and stands there gets no stand-in thrown away every frame")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;  // 96 units inside our cell's east edge
	bob.y = 2000.0f;
	std::optional<Protocol::PlayerState> alice;
	bool shownInBand = false;
	bool shownBeforeBand = false;
	// Alice walks east across the field to 12,500 (8,500 units off, a cell past the loaded square) ...
	for (float x = 4200.0f; x <= 12500.0f; x += 300.0f) {
		alice = session.Send(Session::Outside(x, 2000.0f));
		REQUIRE(alice);
		bob.Frames(alice, 4);
		if (x < 12288.0f) {
			shownBeforeBand = shownBeforeBand || bob.Shown();
		} else {
			shownInBand = shownInBand || bob.standIn.has_value();
		}
	}
	// ... and stands there a while: a minute in her game, two seconds of frames here are enough.
	alice = session.Send(Session::Outside(12500.0f, 2000.0f));
	REQUIRE(alice);
	bob.Frames(alice, 120);
	shownInBand = shownInBand || bob.standIn.has_value();
	CHECK(shownBeforeBand);
	CHECK(!shownInBand);
	CHECK(bob.spawns == 1);  // placed once as she set off, gone as she left the square
	CHECK(bob.leftBehind == 0);

	// She walks back: one new stand-in, drawn as soon as she is in the square again.
	for (float x = 12500.0f; x >= 6000.0f; x -= 400.0f) {
		alice = session.Send(Session::Outside(x, 2000.0f));
		REQUIRE(alice);
		bob.Frames(alice, 4);
	}
	CHECK(bob.Shown());
	CHECK(bob.spawns == 2);
	CHECK(bob.leftBehind == 0);
}

TEST("stand-in range: as we cross a cell edge, a friend's stand-in waits for its cell to attach instead of being replaced")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;
	bob.y = 2000.0f;
	// Alice waits three cells east, one past the loaded square: not drawn.
	const auto alice = session.Send(Session::Outside(13000.0f, 2000.0f));
	REQUIRE(alice);
	bob.Frames(alice, 10);
	CHECK(!bob.standIn);
	// We step over our east edge: her cell is in our square now, and a stand-in is placed in it, but the engine's square
	// is still centred on our old cell for a while as it loads, so her cell is only loaded in its buffer.
	bob.engine.lagging = true;
	bob.x = 4200.0f;
	bob.Frame(alice);
	REQUIRE(bob.standIn);
	CHECK(!bob.Shown());
	bob.Frames(alice, 60);  // a second of loading
	CHECK(bob.standIn);
	CHECK(bob.leftBehind == 0);
	bob.engine.lagging = false;  // the engine's square catches up and attaches her cell
	bob.Frames(alice, 2);
	CHECK(bob.Shown());
	CHECK(bob.spawns == 1);
}

TEST("stand-in range: a friend in a cell the engine loads but never draws costs one stand-in every eleven seconds, and is shown within a second once it does")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;
	bob.y = 2000.0f;
	bob.engine.stuck.insert({ 2, 0 });  // her cell is in the square, loaded, but the engine never attaches it
	const auto alice = session.Send(Session::Outside(9000.0f, 2000.0f));
	REQUIRE(alice);
	// A minute of frames: each stand-in waits ten seconds for the cell, then none is placed for a second. Placed every
	// frame, as before, that would be 3,750 of them.
	int shownFrames = 0;
	for (int i = 0; i < 3750; ++i) {
		bob.Frame(alice);
		shownFrames += bob.Shown();
	}
	CHECK(shownFrames == 0);
	CHECK(bob.spawns >= 5);
	CHECK(bob.spawns <= 6);
	CHECK(bob.respawnWaits == bob.spawns - 1);
	// The cell attaches at last: she is shown within the wait and a frame or two, wherever in the cycle it falls.
	for (int wait = 0; wait < 80; wait += 7) {
		Ours again = bob;
		again.Frames(alice, wait);
		again.engine.stuck.clear();
		int frames = 0;
		while (!again.Shown() && frames < 200) {
			again.Frame(alice);
			++frames;
		}
		CHECK(frames <= 1000 / 16 + 2);
	}
}

TEST("stand-in range: a friend who follows us through a door is shown inside at once")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;
	bob.y = 2000.0f;
	const auto outside = session.Send(Session::Outside(4100.0f, 2000.0f));
	REQUIRE(outside);
	bob.Frames(outside, 10);
	REQUIRE(bob.Shown());
	// We go in; until her state says she is in too, she is not drawn; then she is, that frame.
	bob.interior = HOUSE;
	bob.Frames(outside, 5);
	CHECK(!bob.standIn);
	auto inside = Session::Outside(120.0f, -40.0f);
	inside.worldspace = 0;
	inside.cell = HOUSE;
	const auto in = session.Send(inside);
	REQUIRE(in);
	bob.Frame(in);
	CHECK(bob.Shown());
	// Out again together, her state and ours in the same frame: her stand-in, left in the house, is replaced that frame,
	// not after a wait.
	bob.interior = 0;
	const auto out = session.Send(Session::Outside(4150.0f, 2000.0f));
	REQUIRE(out);
	bob.Frame(out);
	CHECK(bob.Shown());
	CHECK(bob.leftBehind == 1);
	CHECK(bob.respawnWaits == 0);
	CHECK(bob.spawns == 3);
}
