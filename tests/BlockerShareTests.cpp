// Invisible walls a script takes down when it opens the way (src/game/BlockerShare.h), driven through the session
// server: Grandchester Mystery Mansion's quest, which each game runs for itself, unlocks and opens a door and disables a
// CollisionMarker in its doorway, and the friend's game took the door's state but never the wall's. A wall a friend's
// game took down comes down here too; one put back up never goes up here; a world state repeats it harmlessly.

#include "ServerFixture.h"

#include "game/BlockerShare.h"
#include "game/DoorShare.h"

#include <map>
#include <set>

using namespace TestServer;

namespace
{
	using RS = Protocol::RefState;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t PARLOR_WALL = 0x04050557;     // DLC04_ParlorBalconyCollisionRef
	constexpr std::uint32_t FIREPLACE_WALL = 0x04050556;  // DLC04_FireplaceRoomCollisionRef
	constexpr std::uint32_t HALL_WALL = 0x04050555;       // DLC04_UpstairsHallCollisionRef
	constexpr std::uint32_t READING_DOOR = 0x0402F981;    // DLC04MS03_ReadingRoomDoor_Ref

	// One game's walls, judged as WorldSync judges them: what it reads (ReadRefState), what the poll reports when a wall
	// comes down (LookAt), and what it takes from the server (OfferRefState, TryApplyRefState).
	struct WallGame
	{
		struct Wall
		{
			bool disabled = false;
			bool has3D = true;
		};
		std::map<std::uint32_t, Wall> walls;
		std::map<std::uint32_t, RS>   lastSent;  // what the session last heard of each from us, or where it stood when the poll found it
		std::set<std::uint32_t>       listed;    // on the poll's list (found by the walk or the load event)
		DoorShare::Book               book;
		int                           applied = 0;
		int                           reported = 0;

		WallGame()
		{
			walls[PARLOR_WALL] = {};
			walls[FIREPLACE_WALL] = {};
			walls[HALL_WALL] = {};
		}

		// The walk (FindDoors): a wall that stands is put on the list; one that is down is not.
		void Walk()
		{
			for (const auto& [ref, wall] : walls) {
				if (BlockerShare::Polled(true, Protocol::IsShareableRef(ref), wall.disabled, wall.has3D, false)) {
					listed.insert(ref);
				}
			}
		}

		// The script takes the wall down: a disabled reference has no 3D.
		void ScriptDisables(std::uint32_t a_ref)
		{
			walls[a_ref].disabled = true;
			walls[a_ref].has3D = false;
		}

		// The script puts it back up (a trap's, the quest's reset).
		void ScriptEnables(std::uint32_t a_ref)
		{
			walls[a_ref].disabled = false;
			walls[a_ref].has3D = true;
		}

		// One look of the poll (PollDoor, LookAt): the report, if the wall's coming down is news and we are near.
		std::optional<std::vector<std::uint8_t>> Poll(std::uint32_t a_ref, bool a_near = true)
		{
			if (!listed.contains(a_ref)) {
				return std::nullopt;
			}
			const auto& wall = walls.at(a_ref);
			const bool  said = lastSent.contains(a_ref) && lastSent.at(a_ref).open == RS::kYes;
			if (!BlockerShare::WorthReading(wall.disabled, wall.has3D, false, said)) {
				listed.erase(a_ref);  // forgotten: down and said
				return std::nullopt;
			}
			const auto state = BlockerShare::Read(a_ref, wall.disabled);
			if (state.open != RS::kYes && lastSent.contains(a_ref)) {
				lastSent[a_ref] = state;  // a wall put back up is nothing to say
				return std::nullopt;
			}
			DoorShare::Watch look;
			look.quiet = true;
			look.seen = lastSent.contains(a_ref);
			look.changed = BlockerShare::Opened(state, lastSent.contains(a_ref) ? lastSent.at(a_ref) : RS{});
			look.near = a_near;
			switch (DoorShare::OnLook(look)) {
			case DoorShare::Look::kBaseline:
				lastSent[a_ref] = state;
				return std::nullopt;
			case DoorShare::Look::kSend:
				lastSent[a_ref] = state;
				book.Sent(state);
				++reported;
				return Protocol::Encode(state, MT::kReportRefState);
			default:
				return std::nullopt;
			}
		}

		void Offer(const RS& a_state)
		{
			if (!walls.contains(a_state.refId) || !book.Take(a_state)) {
				return;
			}
			auto&      wall = walls.at(a_state.refId);
			const auto current = BlockerShare::Read(a_state.refId, wall.disabled);
			lastSent[a_state.refId] = DoorShare::Heard(a_state, lastSent.contains(a_state.refId) ? lastSent.at(a_state.refId) : current, true);
			if (BlockerShare::Decide(a_state, wall.disabled) == BlockerShare::Apply::kOpen) {
				wall.disabled = true;
				wall.has3D = false;
				++applied;
			}
		}

		void Take(const Packets& a_packets)
		{
			for (const auto& state : Decoded(a_packets, MT::kRefStateChanged, Protocol::DecodeRefState)) {
				Offer(state);
			}
			for (const auto& world : WorldStates(a_packets)) {
				for (const auto& state : world.refStates) {
					Offer(state);
				}
			}
		}
	};

	Server::Options Unhurried()
	{
		Server::Options options;
		options.worldStateRefill = std::chrono::milliseconds(1);
		return options;
	}
}

TEST("blocker share: only a plugin's collision marker is an invisible wall, and a wall down needs no 3D to be read")
{
	CHECK(BlockerShare::IsBlockerBase(0x00000021));
	CHECK(!BlockerShare::IsBlockerBase(0x00000034));  // XMarkerHeading
	CHECK(!BlockerShare::IsBlockerBase(0x0003D001));  // a door
	CHECK(!BlockerShare::IsBlockerBase(0));

	CHECK(BlockerShare::Read(PARLOR_WALL, true).open == RS::kYes);
	CHECK(BlockerShare::Read(PARLOR_WALL, false).open == RS::kNo);
	CHECK(BlockerShare::Read(PARLOR_WALL, true).locked == RS::kUnknown);  // the lock is never said
	CHECK(DoorShare::Says(BlockerShare::Read(PARLOR_WALL, false)));

	// The walk lists a wall that stands, loaded, a plugin's; never one that is down, unloaded, deleted or a runtime reference's.
	CHECK(BlockerShare::Polled(true, true, false, true, false));
	CHECK(!BlockerShare::Polled(true, true, true, false, false));
	CHECK(!BlockerShare::Polled(true, true, false, false, false));
	CHECK(!BlockerShare::Polled(true, true, false, true, true));
	CHECK(!BlockerShare::Polled(true, Protocol::IsShareableRef(0xFF000123), false, true, false));
	CHECK(!BlockerShare::Polled(false, true, false, true, false));
}

TEST("blocker share: a friend's wall coming down is news; a wall put back up never is; ours down already takes nothing")
{
	RS up = BlockerShare::Read(HALL_WALL, false), down = BlockerShare::Read(HALL_WALL, true);
	CHECK(BlockerShare::Opened(down, up));
	CHECK(BlockerShare::Opened(down, RS{}));  // nothing said of it yet
	CHECK(!BlockerShare::Opened(down, down));
	CHECK(!BlockerShare::Opened(up, down));  // the wall coming back is nothing to say
	CHECK(!BlockerShare::Opened(up, up));

	CHECK(BlockerShare::Decide(down, false) == BlockerShare::Apply::kOpen);
	CHECK(BlockerShare::Decide(down, true) == BlockerShare::Apply::kNothing);
	CHECK(BlockerShare::Decide(up, false) == BlockerShare::Apply::kNothing);  // theirs stands: ours is left as it is
	CHECK(BlockerShare::Decide(up, true) == BlockerShare::Apply::kNothing);   // and never put up
	RS unknown;
	unknown.refId = HALL_WALL;
	CHECK(BlockerShare::Decide(unknown, false) == BlockerShare::Apply::kNothing);

	// Worth reading while it stands loaded, and once more when it is down (no 3D) until that is said; then forgotten.
	CHECK(BlockerShare::WorthReading(false, true, false, false));
	CHECK(BlockerShare::WorthReading(true, false, false, false));
	CHECK(!BlockerShare::WorthReading(true, false, false, true));
	CHECK(!BlockerShare::WorthReading(false, false, false, false));  // unloaded and standing: not here to read
	CHECK(!BlockerShare::WorthReading(true, false, true, false));
}

TEST("blocker share: the wall the friend's script took down comes down in the host's game, and only that one")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	WallGame host, friendGame;
	host.Walk();
	friendGame.Walk();
	// The poll's first look is where each stands, not news.
	for (const auto ref : { PARLOR_WALL, FIREPLACE_WALL, HALL_WALL }) {
		CHECK(!host.Poll(ref));
		CHECK(!friendGame.Poll(ref));
	}

	// The friend's stage 13 unlocks and opens the dining room's door and disables the fireplace room's wall: the host's
	// game gets the door through the door's state (DoorShare) and the wall through this.
	friendGame.ScriptDisables(FIREPLACE_WALL);
	const auto report = friendGame.Poll(FIREPLACE_WALL);
	REQUIRE(report);
	f.fake->Deliver(2, *report);
	host.Take(Received(f, 1));
	CHECK(host.walls[FIREPLACE_WALL].disabled);
	CHECK(!host.walls[PARLOR_WALL].disabled);
	CHECK(!host.walls[HALL_WALL].disabled);
	CHECK(host.applied == 1);

	// The host's watcher doesn't say it back, nor does it read as news in the next look.
	CHECK(!host.Poll(FIREPLACE_WALL));
	CHECK(host.reported == 0);

	// The friend's wall is forgotten once said: no more reads of it.
	CHECK(!friendGame.Poll(FIREPLACE_WALL));
	CHECK(!friendGame.listed.contains(FIREPLACE_WALL));
	CHECK(friendGame.reported == 1);
}

TEST("blocker share: a wall put back up is neither said nor put up in the other game, and a wall that comes down again is said again")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	WallGame host, friendGame;
	host.Walk();
	friendGame.Walk();
	CHECK(!friendGame.Poll(HALL_WALL));  // the baseline

	friendGame.ScriptDisables(HALL_WALL);
	f.fake->Deliver(2, *friendGame.Poll(HALL_WALL));
	host.Take(Received(f, 1));
	CHECK(host.walls[HALL_WALL].disabled);

	// The friend's own script puts it back up (the quest resets): the walk finds it again, and nothing is said.
	friendGame.ScriptEnables(HALL_WALL);
	friendGame.Walk();
	CHECK(!friendGame.Poll(HALL_WALL));  // its baseline again
	CHECK(!friendGame.Poll(HALL_WALL));
	CHECK(Count(Received(f, 1), MT::kRefStateChanged) == 0);
	CHECK(host.walls[HALL_WALL].disabled);  // the host's stays down: a way once open is not shut by a friend's script

	// Down again: said again.
	friendGame.ScriptDisables(HALL_WALL);
	const auto again = friendGame.Poll(HALL_WALL);
	CHECK(again);
	CHECK(friendGame.reported == 2);
}

TEST("blocker share: a friend who joins later, or a host that loads a save, has the wall taken down once; the host's own script is left alone after")
{
	Fixture f{ Unhurried() };
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	Clear(f, { 1 });
	WallGame host, friendGame;
	friendGame.Walk();
	friendGame.Poll(PARLOR_WALL);
	friendGame.ScriptDisables(PARLOR_WALL);
	f.Join(2, Fixture::MakeHello("Bob"));
	f.fake->Deliver(2, *friendGame.Poll(PARLOR_WALL));
	Clear(f, { 1, 2 });

	// The host asks for the world: the wall is taken down, once.
	f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
	host.Take(Received(f, 1));
	CHECK(host.walls[PARLOR_WALL].disabled);
	CHECK(host.applied == 1);

	// Its repeats change nothing, even where the host's own script has put the wall up again.
	host.ScriptEnables(PARLOR_WALL);
	f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
	host.Take(Received(f, 1));
	CHECK(!host.walls[PARLOR_WALL].disabled);
	CHECK(host.applied == 1);
}

TEST("blocker share: a wall's state is a reference's state to the server, like a door's, and keeps no lock")
{
	Fixture f{ Unhurried() };
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	WallGame friendGame;
	friendGame.Walk();
	friendGame.Poll(HALL_WALL);
	friendGame.ScriptDisables(HALL_WALL);
	f.fake->Deliver(1, *friendGame.Poll(HALL_WALL));
	// A door beside it, opened by the same stage, is its own reference with its own state.
	f.fake->Deliver(1, Protocol::Encode(RS{ READING_DOOR, RS::kYes, RS::kNo }, MT::kReportRefState));
	Clear(f, { 1 });

	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	const auto worlds = WorldStates(Received(f, 2));
	REQUIRE(worlds.size() == 1);
	std::map<std::uint32_t, RS> byRef;
	for (const auto& state : worlds[0].refStates) {
		byRef[state.refId] = state;
	}
	REQUIRE(byRef.contains(HALL_WALL));
	CHECK(byRef[HALL_WALL].open == RS::kYes);
	CHECK(byRef[HALL_WALL].locked == RS::kUnknown);
	CHECK(byRef[READING_DOOR].open == RS::kYes);
	CHECK(byRef[READING_DOOR].locked == RS::kNo);
}
