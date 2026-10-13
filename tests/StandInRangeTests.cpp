// Which friends outside get a stand-in here, and what becomes of a stand-in whose cell the game stops drawing
// (game/StandInRange.h). The rules on their own first, then a friend walking about as our game hears of them through a
// real server, with the engine's loaded cells played by a small model.

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

namespace
{
	// The engine's side: which exterior cells are attached. Its square is centred on its own grid cell, which can lag
	// behind our player's while it loads after a cell edge; cells just outside it sit loaded in its buffer.
	struct Engine
	{
		int                             grids = GRIDS;
		std::int32_t                    centreX = 0;
		std::int32_t                    centreY = 0;
		std::set<std::pair<int, int>>   never;  // cells in the square it never attaches (a broken cell)

		Cell StateOf(std::int32_t a_x, std::int32_t a_y) const
		{
			if (never.contains({ a_x, a_y })) {
				return Cell::kGone;
			}
			if (StandInRange::InLoadedGrid(a_x, a_y, centreX, centreY, grids)) {
				return Cell::kAttached;
			}
			return StandInRange::InLoadedGrid(a_x, a_y, centreX, centreY, grids + 2) ? Cell::kOnItsWay : Cell::kGone;
		}
	};

	// Our game, as far as one friend's stand-in goes, frame by frame, deciding with StandInRange as RemotePlayers does.
	// A stand-in stays in the cell it was placed in while it walks (the engine does not move it to the next).
	struct Ours
	{
		struct StandIn
		{
			std::uint32_t     interior = 0;
			std::int32_t      x = 0;
			std::int32_t      y = 0;
			bool              drawn = false;
			Clock::time_point spawnedAt{};
		};

		Engine                 engine;
		float                  x = 0.0f;
		float                  y = 0.0f;
		std::uint32_t          interior = 0;  // our player's interior cell; 0 outside
		std::optional<StandIn> standIn;
		Clock::time_point      now = Clock::time_point{} + 1h;
		Clock::time_point      replacedLaterAt{};
		int                    spawns = 0;
		int                    leftBehind = 0;

		void Frame(const std::optional<Protocol::PlayerState>& a_friend)
		{
			now += 16ms;
			bool visible = a_friend && (interior ? a_friend->cell == interior : a_friend->cell == 0 && a_friend->worldspace == COMMONWEALTH);
			if (visible && !interior) {
				visible = StandInRange::Visible(a_friend->x, a_friend->y, x, y, engine.grids);
			}
			if (!visible) {
				standIn.reset();
				return;
			}
			const auto ourX = WorkshopCells::Grid(x);
			const auto ourY = WorkshopCells::Grid(y);
			if (standIn && (standIn->interior != interior || (!interior && (standIn->x != ourX || standIn->y != ourY)))) {
				StandInRange::LeftFacts facts;
				facts.weInside = interior != 0;
				facts.cell = standIn->interior ? Cell::kInterior : engine.StateOf(standIn->x, standIn->y);
				facts.cellInGrid = !standIn->interior && StandInRange::InLoadedGrid(standIn->x, standIn->y, ourX, ourY, engine.grids);
				facts.drawn = standIn->drawn;
				facts.sinceSpawned = now - standIn->spawnedAt;
				if (const auto left = StandInRange::ChooseLeft(facts); left != Left::kKeep) {
					++leftBehind;
					if (left == Left::kReplaceLater) {
						replacedLaterAt = now;
					}
					standIn.reset();
				}
			}
			if (!standIn) {
				if (!StandInRange::MayRespawn(now - replacedLaterAt)) {
					return;
				}
				standIn = StandIn{ a_friend->cell, WorkshopCells::Grid(a_friend->x), WorkshopCells::Grid(a_friend->y), false, now };
				++spawns;
			}
			standIn->drawn = standIn->drawn || (standIn->interior ? standIn->interior == interior : engine.StateOf(standIn->x, standIn->y) == Cell::kAttached);
		}

		void Frames(const std::optional<Protocol::PlayerState>& a_friend, int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				Frame(a_friend);
			}
		}

		bool Shown() const { return standIn && standIn->drawn; }
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

		// Alice's game sends a state; what Bob's game is told of her, if anything.
		std::optional<Protocol::PlayerState> Send(Protocol::PlayerState a_state, std::optional<std::uint32_t> a_sequence = std::nullopt)
		{
			a_state.sequence = a_sequence.value_or(++sequence);
			std::this_thread::sleep_for(20ms);  // her game sends 30 a second; the server takes 60 at most
			f.fake->Deliver(2, Protocol::Encode(a_state));
			while (const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerStates), a_sequence ? 150ms : 2000ms)) {
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

TEST("stand-in range: a late packet from where a friend was does not take their stand-in away again")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;
	bob.y = 2000.0f;
	const auto out = session.Send(Session::Outside(12500.0f, 2000.0f));  // in the band
	REQUIRE(out);
	bob.Frames(out, 10);
	CHECK(!bob.standIn);
	const auto back = session.Send(Session::Outside(6000.0f, 2000.0f));
	REQUIRE(back);
	bob.Frames(back, 10);
	REQUIRE(bob.Shown());
	// The band state again, older than the one she is at now, and then twice the newest: the server passes on neither.
	CHECK(!session.Send(Session::Outside(12500.0f, 2000.0f), out->sequence));
	CHECK(!session.Send(Session::Outside(6000.0f, 2000.0f), back->sequence));
	bob.Frames(back, 60);
	CHECK(bob.Shown());
	CHECK(bob.spawns == 1);
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
	// is still centred on our old cell for a while as it loads, so her cell is only in its buffer.
	bob.x = 4200.0f;
	bob.Frame(alice);
	REQUIRE(bob.standIn);
	CHECK(!bob.standIn->drawn);
	bob.Frames(alice, 60);  // a second of loading
	CHECK(bob.standIn);
	CHECK(bob.leftBehind == 0);
	bob.engine.centreX = 1;  // the engine's square catches up and attaches her cell
	bob.Frames(alice, 2);
	CHECK(bob.Shown());
	CHECK(bob.spawns == 1);
}

TEST("stand-in range: a stand-in placed where the game never draws anything is placed again at most once a second")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;
	bob.y = 2000.0f;
	bob.engine.never.insert({ 2, 0 });  // her cell is in the square, but the engine never attaches it
	const auto alice = session.Send(Session::Outside(9000.0f, 2000.0f));
	REQUIRE(alice);
	// Ten seconds of frames (600): every frame, as before, would be 600 stand-ins.
	bob.Frames(alice, 600);
	CHECK(bob.spawns >= 1);
	CHECK(bob.spawns <= 11);
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
	CHECK(bob.spawns == 3);
}
