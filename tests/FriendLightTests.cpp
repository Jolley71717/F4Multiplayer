// A friend's Pip-Boy light at their stand-in (game/FriendLight.h). The rules on their own first, then a friend walking
// about as our game hears of them through a real server, their light switched on and off with their status: each frame
// the stand-in is kept or placed as StandInRange::Frame says and the light as FriendLight::Frame says, as
// RemotePlayers::Update does, with the engine's loaded cells played by a model.

#include "ServerFixture.h"

#include "game/FriendLight.h"
#include "game/StandInRange.h"

#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using FriendLight::Action;
	using FriendLight::Place;
	using StandInRange::CellState;
	using StandInRange::Left;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t NUKA_WORLD = 0x0600BB3D;  // another worldspace
	constexpr std::uint32_t HOUSE = 0x0001F2A4;       // an interior cell
	constexpr std::uint32_t SHACK = 0x0001F2A5;       // another one
	constexpr int           GRIDS = 5;                // the game's default uGridsToLoad

	// A made-up form ID for the exterior cell at grid (a_x, a_y).
	constexpr std::uint32_t OutsideCell(std::int32_t a_x, std::int32_t a_y)
	{
		return 0x00100000u | (static_cast<std::uint32_t>(a_x + 128) & 0xFFu) << 8 | (static_cast<std::uint32_t>(a_y + 128) & 0xFFu);
	}

	constexpr Place Outside(std::int32_t a_x, std::int32_t a_y, std::uint32_t a_world = COMMONWEALTH)
	{
		// Another worldspace has cells of its own at the same grid.
		return { .cell = OutsideCell(a_x, a_y) + (a_world == COMMONWEALTH ? 0u : 0x00010000u), .interior = false, .worldspace = a_world };
	}

	constexpr Place Inside(std::uint32_t a_cell)
	{
		return { .cell = a_cell, .interior = true };
	}

	// A friend's light and stand-in, both outside in neighbouring cells, the light's cell in the state a_state.
	FriendLight::Facts Lit(CellState a_state, bool a_attached = false, Clock::duration a_sincePlaced = 200ms)
	{
		return { .wanted = true, .lit = true, .light = Outside(1, 0), .lightCell = a_state, .attached = a_attached, .sincePlaced = a_sincePlaced, .standIn = Outside(0, 0) };
	}
}

TEST("friend light: two references are in one place in the same cell, or both outside in the same worldspace")
{
	CHECK(FriendLight::SamePlace(Outside(0, 0), Outside(0, 0)) == true);
	CHECK(FriendLight::SamePlace(Outside(0, 0), Outside(1, 0)) == true);   // a neighbouring cell: the stand-in walked on
	CHECK(FriendLight::SamePlace(Outside(-3, 4), Outside(2, -1)) == true);
	CHECK(FriendLight::SamePlace(Outside(0, 0), Outside(0, 0, NUKA_WORLD)) == false);
	CHECK(FriendLight::SamePlace(Outside(0, 0), Inside(HOUSE)) == false);  // through a door
	CHECK(FriendLight::SamePlace(Inside(HOUSE), Outside(0, 0)) == false);
	CHECK(FriendLight::SamePlace(Inside(HOUSE), Inside(HOUSE)) == true);
	CHECK(FriendLight::SamePlace(Inside(HOUSE), Inside(SHACK)) == false);
	// Two interiors are never one place by their (absent) worldspace.
	CHECK(FriendLight::SamePlace({ .cell = HOUSE, .interior = true, .worldspace = 0 }, { .cell = SHACK, .interior = true, .worldspace = 0 }) == false);
	// Still attaching: not known.
	CHECK(!FriendLight::SamePlace(Place{}, Outside(0, 0)).has_value());
	CHECK(!FriendLight::SamePlace(Inside(HOUSE), Place{}).has_value());
}

TEST("friend light: a light in the stand-in's place stays and is moved along; one behind a door or in another worldspace is made again")
{
	FriendLight::Facts facts;
	CHECK(FriendLight::Choose(facts) == Action::kNone);
	facts.lit = true;
	CHECK(FriendLight::Choose(facts) == Action::kRemove);  // their light went off, or their stand-in went
	facts = {};
	facts.wanted = true;
	CHECK(FriendLight::Choose(facts) == Action::kPlace);

	// In a neighbouring cell outside, drawn: it stays, however long it has stood.
	CHECK(FriendLight::Choose(Lit(CellState::kAttached)) == Action::kMove);
	CHECK(FriendLight::Choose(Lit(CellState::kAttached, true, 10min)) == Action::kMove);
	// The stand-in went through a door, or into another worldspace.
	auto in = Lit(CellState::kAttached, true);
	in.standIn = Inside(HOUSE);
	CHECK(FriendLight::Choose(in) == Action::kReplace);
	auto away = Lit(CellState::kAttached, true);
	away.standIn = Outside(0, 0, NUKA_WORLD);
	CHECK(FriendLight::Choose(away) == Action::kReplace);
	auto inside = Lit(CellState::kAttached, true);
	inside.light = Inside(HOUSE);
	inside.standIn = Inside(HOUSE);
	CHECK(FriendLight::Choose(inside) == Action::kMove);
	auto out = inside;
	out.standIn = Outside(0, 0);
	CHECK(FriendLight::Choose(out) == Action::kReplace);
	// The light or the stand-in in no cell yet: it waits.
	auto attaching = Lit(CellState::kNotLoaded);
	attaching.light = {};
	CHECK(FriendLight::Choose(attaching) == Action::kMove);
	auto standInAttaching = Lit(CellState::kAttached, true);
	standInAttaching.standIn = {};
	CHECK(FriendLight::Choose(standInAttaching) == Action::kMove);
}

TEST("friend light: a light whose cell the engine is still bringing in waits for it; one whose cell it let go of is made again")
{
	for (const auto state : { CellState::kLoadingData, CellState::kLoading, CellState::kLoaded, CellState::kAttachQueued, CellState::kAttaching }) {
		CHECK(FriendLight::Choose(Lit(state)) == Action::kMove);
		CHECK(FriendLight::Choose(Lit(state, false, FriendLight::ATTACH_LIMIT - 1ms)) == Action::kMove);
		CHECK(FriendLight::Choose(Lit(state, false, FriendLight::ATTACH_LIMIT)) == Action::kReplace);  // it never came
		CHECK(FriendLight::Choose(Lit(state, true)) == Action::kReplace);  // drawn before: its cell is on its way out
	}
	for (const auto state : { CellState::kNotLoaded, CellState::kUnloading, CellState::kDetaching }) {
		CHECK(FriendLight::Choose(Lit(state)) == Action::kReplace);
		CHECK(FriendLight::Choose(Lit(state, true)) == Action::kReplace);
	}

	// From frame to frame: the track remembers that its cell was drawn.
	const auto placed = Clock::time_point{} + 1h;
	FriendLight::Track track;
	FriendLight::Placed(track, placed);
	auto facts = Lit(CellState::kAttaching);
	CHECK(FriendLight::Frame(track, facts, placed + 16ms) == Action::kMove);
	CHECK(!track.attached);
	facts.lightCell = CellState::kAttached;
	CHECK(FriendLight::Frame(track, facts, placed + 32ms) == Action::kMove);
	CHECK(track.attached);
	facts.lightCell = CellState::kLoaded;  // the engine let go of it: it sits in its buffer, drawn no more
	CHECK(FriendLight::Frame(track, facts, placed + 48ms) == Action::kReplace);
	FriendLight::Placed(track, placed + 48ms);
	CHECK(!track.attached);
	CHECK(FriendLight::Frame(track, Lit(CellState::kLoaded), placed + 64ms) == Action::kMove);
}

namespace
{
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

	// A cell: an interior, or a grid cell outside.
	struct Spot
	{
		std::uint32_t interior = 0;
		std::int32_t  x = 0;
		std::int32_t  y = 0;
	};

	Place PlaceOf(const Spot& a_spot)
	{
		return a_spot.interior ? Inside(a_spot.interior) : Outside(a_spot.x, a_spot.y);
	}

	Spot SpotOf(const Protocol::PlayerState& a_state)
	{
		return a_state.cell ? Spot{ a_state.cell } : Spot{ 0, WorkshopCells::Grid(a_state.x), WorkshopCells::Grid(a_state.y) };
	}

	// Our game, as far as one friend's stand-in and light go, frame by frame, as RemotePlayers::Update does. A reference
	// stays in the cell it was placed in, the stand-in while it walks and the light while it is moved along with it; the
	// light goes with the stand-in (DeleteActor), and is placed where the stand-in stands.
	struct Ours
	{
		Engine                 engine;
		float                  x = 0.0f;
		float                  y = 0.0f;
		std::uint32_t          interior = 0;  // our player's interior cell; 0 outside
		std::optional<Spot>    standIn;
		StandInRange::Track    range;
		bool                   lightOn = false;  // as their last status said
		std::optional<Spot>    light;
		FriendLight::Track     lightTrack;
		Clock::time_point      now = Clock::time_point{} + 1h;
		int                    spawns = 0;
		int                    lightsPlaced = 0;
		int                    lightsRemoved = 0;
		int                    oldRuleRemakes = 0;  // frames the rule before this one would have made the light again

		CellState StateOf(const Spot& a_spot) const
		{
			if (a_spot.interior) {
				return a_spot.interior == interior ? CellState::kAttached : CellState::kNotLoaded;
			}
			return interior ? CellState::kNotLoaded : engine.StateOf(a_spot.x, a_spot.y);
		}

		bool Drawn() const { return standIn && StateOf(*standIn) == CellState::kAttached; }
		bool LightShown() const { return light && StateOf(*light) == CellState::kAttached; }

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
			read.state = StateOf(*standIn);
			read.x = standIn->x;
			read.y = standIn->y;
			read.has3D = Drawn();
			return read;
		}

		void RemoveStandIn()
		{
			standIn.reset();
			if (light) {
				light.reset();
				++lightsRemoved;
			}
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
			const auto look = StandInRange::Frame(range, sight, now);
			if (!look.visible) {
				RemoveStandIn();
				return;
			}
			if (standIn && look.left != Left::kKeep) {
				RemoveStandIn();
			}
			if (!standIn) {
				if (!look.mayPlace) {
					return;
				}
				standIn = SpotOf(*a_friend);
				++spawns;
				StandInRange::Placed(range, now);
			}

			FriendLight::Facts facts;
			facts.wanted = lightOn && Drawn();
			facts.lit = light.has_value();
			if (light) {
				facts.light = PlaceOf(*light);
				facts.lightCell = StateOf(*light);
			}
			facts.standIn = PlaceOf(*standIn);
			// The rule before: a light in another cell than the stand-in's, or in a cell not drawn, was made again.
			oldRuleRemakes += facts.wanted && light && (facts.light.cell != facts.standIn.cell || facts.lightCell != CellState::kAttached);
			switch (FriendLight::Frame(lightTrack, facts, now)) {
			case Action::kRemove:
				light.reset();
				++lightsRemoved;
				break;
			case Action::kReplace:
				light.reset();
				++lightsRemoved;
				[[fallthrough]];
			case Action::kPlace:
				light = SpotOf(*a_friend);  // where the stand-in stands now
				++lightsPlaced;
				FriendLight::Placed(lightTrack, now);
				break;
			default:
				break;
			}
		}

		void Frames(const std::optional<Protocol::PlayerState>& a_friend, int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				Frame(a_friend);
			}
		}
	};

	// Alice (peer 2) and Bob (peer 1), both in. Bob's game is the one whose stand-ins and lights are looked at.
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

		static Protocol::PlayerState At(float a_x, float a_y)
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

		// Alice switches her Pip-Boy light on or off, and her game sends its status as Party does; Bob's game keeps what
		// it is told (RemotePlayers::SetLight).
		void SwitchLight(Ours& a_bob, bool a_on)
		{
			Protocol::PlayerStatus status;
			status.health = 100;
			status.level = 12;
			status.light = a_on;
			f.fake->Deliver(2, Protocol::Encode(status, MT::kReportStatus));
			while (const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerStatus), 2000ms)) {
				const auto heard = Protocol::DecodePlayerStatus(*packet);
				REQUIRE(heard);
				if (heard->playerId == alice && heard->light == a_on) {
					a_bob.lightOn = heard->light;
					return;
				}
			}
			REQUIRE(false);  // her status never reached Bob
		}
	};
}

TEST("friend light: a friend who switches their light on and walks across three cells outside gets one light, which goes along with them")
{
	Session session;
	Ours    bob;
	bob.x = 2000.0f;  // cell 0
	bob.y = 2000.0f;
	std::optional<Protocol::PlayerState> alice;
	// Alice sets off from two cells west, her light off: her stand-in is placed there, and stays in that cell as she walks.
	for (float x = -5000.0f; x < 1000.0f; x += 300.0f) {
		alice = session.Send(Session::At(x, 2000.0f));
		REQUIRE(alice);
		bob.Frames(alice, 4);
	}
	REQUIRE(bob.Drawn());
	CHECK(bob.standIn->x == -2);
	CHECK(!bob.light);
	// In cell 0 she switches her light on, and walks on east into cell 2.
	session.SwitchLight(bob, true);
	int frames = 0;
	int shown = 0;
	for (float x = 1000.0f; x <= 10400.0f; x += 300.0f) {
		alice = session.Send(Session::At(x, 2000.0f));
		REQUIRE(alice);
		for (int i = 0; i < 4; ++i) {
			bob.Frame(alice);
			++frames;
			shown += bob.LightShown();
		}
	}
	CHECK(bob.spawns == 1);
	CHECK(bob.lightsPlaced == 1);  // made once, in cell 0, and moved along with her
	CHECK(bob.lightsRemoved == 0);
	CHECK(shown == frames);
	CHECK(bob.light && bob.light->x == 0);
	// The light's cell was never the stand-in's: the rule before made it again on every one of those frames.
	CHECK(bob.oldRuleRemakes >= frames - 1);

	// She switches it off: it goes, once; on again: one more.
	session.SwitchLight(bob, false);
	bob.Frames(alice, 30);
	CHECK(!bob.light);
	CHECK(bob.lightsRemoved == 1);
	session.SwitchLight(bob, true);
	bob.Frames(alice, 30);
	CHECK(bob.LightShown());
	CHECK(bob.lightsPlaced == 2);
	CHECK(bob.lightsRemoved == 1);
}

TEST("friend light: walking back together, a friend's light is made again once as the engine lets go of its cell, not every frame")
{
	Session session;
	Ours    bob;
	bob.x = 2000.0f;  // cell 0
	bob.y = 2000.0f;
	// Alice's stand-in is placed in cell 0; she walks east into cell 2 and switches her light on there.
	std::optional<Protocol::PlayerState> alice;
	for (float x = 3000.0f; x <= 10000.0f; x += 500.0f) {
		alice = session.Send(Session::At(x, 2000.0f));
		REQUIRE(alice);
		bob.Frames(alice, 4);
	}
	session.SwitchLight(bob, true);
	bob.Frames(alice, 10);
	REQUIRE(bob.LightShown());
	REQUIRE(bob.light->x == 2);
	// We walk west together, Bob from cell 0 into cell -1, Alice from cell 2 into cell -2. Her stand-in's cell (0) stays
	// drawn; the light's (2) leaves the square around us, and the light is made again where she has got to.
	int shown = 0;
	int frames = 0;
	for (float x = 10000.0f; x >= -6000.0f; x -= 300.0f) {
		bob.x = 2000.0f + (x - 10000.0f) / 4.0f;
		alice = session.Send(Session::At(x, 2000.0f));
		REQUIRE(alice);
		for (int i = 0; i < 4; ++i) {
			bob.Frame(alice);
			++frames;
			shown += bob.LightShown();
		}
	}
	bob.Frames(alice, 120);
	CHECK(WorkshopCells::Grid(bob.x) == -1);
	CHECK(bob.spawns == 1);
	CHECK(bob.standIn->x == 0);
	CHECK(bob.lightsPlaced == 2);
	CHECK(bob.lightsRemoved == 1);
	CHECK(shown == frames);  // never a frame without it
	CHECK(bob.LightShown());
	CHECK(bob.light->x == 0);  // made again in cell 0, where she was as Bob stepped into cell -1
}

TEST("friend light: a light made in a cell the engine is still bringing in waits for it, and one in a cell it never attaches is made again every ten seconds")
{
	Session session;
	Ours    bob;
	bob.x = 4000.0f;  // cell 0, by its east edge
	bob.y = 2000.0f;
	auto alice = session.Send(Session::At(6000.0f, 2000.0f));  // cell 1
	REQUIRE(alice);
	bob.Frames(alice, 10);
	REQUIRE(bob.Drawn());
	// We step over our east edge while the engine is still loading: its square stays on cell 0 (to cell 2) for a while.
	// Alice walks on into cell 3, which it only has in its buffer, and switches her light on there.
	bob.engine.lagging = true;
	bob.x = 4200.0f;
	alice = session.Send(Session::At(12600.0f, 2000.0f));
	REQUIRE(alice);
	bob.Frames(alice, 2);
	session.SwitchLight(bob, true);
	bob.Frames(alice, 60);  // a second of loading
	CHECK(bob.light && bob.light->x == 3);
	CHECK(!bob.LightShown());
	CHECK(bob.lightsPlaced == 1);  // made again every frame of it before
	CHECK(bob.oldRuleRemakes >= 59);
	bob.engine.lagging = false;  // the square catches up and attaches cell 3
	bob.Frames(alice, 2);
	CHECK(bob.LightShown());
	CHECK(bob.lightsPlaced == 1);
	CHECK(bob.lightsRemoved == 0);

	// A cell in the square the engine never attaches: the light waits ten seconds for it each time, not one frame.
	Ours stuck;
	stuck.x = 4200.0f;
	stuck.y = 2000.0f;
	stuck.lightOn = true;
	stuck.Frames(alice, 10);
	REQUIRE(stuck.LightShown());
	const auto stuckAt = Session::At(10000.0f, 5000.0f);  // cell (2, 1)
	stuck.engine.stuck.insert({ 2, 1 });
	stuck.light = Spot{ 0, 2, 1 };  // the light was last made in that cell as she stepped into it
	FriendLight::Placed(stuck.lightTrack, stuck.now);
	stuck.Frames(stuckAt, 3750);  // a minute
	CHECK(stuck.lightsPlaced >= 1 + 5);
	CHECK(stuck.lightsPlaced <= 1 + 6);
}
