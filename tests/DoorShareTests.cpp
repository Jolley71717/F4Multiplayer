// Which doors' open and lock states a game shares, and which of the session's it takes again
// (src/game/DoorShare.h), driven through the session server: an elevator's open state is its script's,
// and a world state that repeats what a game knows doesn't force its doors again.

#include "ServerFixture.h"

#include "game/DoorShare.h"

#include <map>

using namespace TestServer;

namespace
{
	using RS = Protocol::RefState;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t DOOR = 0x0003D001;      // a plain door a player opens
	constexpr std::uint32_t ELEVATOR = 0x000A9E27;  // an elevator (an activator its script opens and closes)
	constexpr std::uint32_t CAR = 0x000A9E29;       // its car
	constexpr std::uint32_t SCRIPTED = 0x0003D002;  // a door with a script on it
	constexpr std::uint32_t AUTOMATIC = 0x0003D003; // a door that opens itself

	// One game's doors, judged as WorldSync judges them: what it reads of a door (ReadRefState), what it
	// reports when that changes (WatchRefStates), and what it takes from the server (OfferRefState and
	// TryApplyRefState).
	struct DoorGame
	{
		struct Door
		{
			DoorShare::Facts facts;
			std::uint8_t     open = RS::kNo;
			std::uint8_t     locked = RS::kUnknown;
		};
		std::map<std::uint32_t, Door> doors;
		std::map<std::uint32_t, RS>   lastSent;
		DoorShare::Book               book;
		int                           applied = 0;

		DoorGame()
		{
			doors[DOOR] = { { .door = true } };
			doors[ELEVATOR] = { { .door = false } };
			doors[CAR] = { { .door = false } };
			doors[SCRIPTED] = { { .door = true, .scripted = true } };
			doors[AUTOMATIC] = { { .door = true, .doorFlags = DoorShare::DOOR_AUTOMATIC } };
		}

		RS Read(std::uint32_t a_ref) const
		{
			const auto& door = doors.at(a_ref);
			return DoorShare::Shared(RS{ a_ref, door.open, door.locked }, DoorShare::SharesOpen(door.facts));
		}

		// The watcher's report of a_ref, if it says something new.
		std::optional<std::vector<std::uint8_t>> Report(std::uint32_t a_ref)
		{
			const auto state = Read(a_ref);
			if (!DoorShare::Says(state) || state.SameState(lastSent[a_ref])) {
				return std::nullopt;
			}
			lastSent[a_ref] = state;
			book.Sent(state);
			return Protocol::Encode(state, MT::kReportRefState);
		}

		void Offer(const RS& a_state)
		{
			if (!doors.contains(a_state.refId) || !book.Take(a_state)) {
				return;
			}
			lastSent[a_state.refId] = a_state;
			const auto current = Read(a_state.refId);
			if (!DoorShare::Says(current)) {
				return;
			}
			auto& door = doors.at(a_state.refId);
			if (a_state.locked != RS::kUnknown && current.locked != RS::kUnknown && a_state.locked != current.locked) {
				door.locked = a_state.locked;
				++applied;
			}
			if (a_state.open != RS::kUnknown && current.open != RS::kUnknown && a_state.open != current.open) {
				door.open = a_state.open;
				++applied;
			}
		}

		// Everything about doors in what the server sent: live changes and world states.
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

	// A server that answers every request for the world at once (a game asks again once a minute).
	Server::Options Unhurried()
	{
		Server::Options options;
		options.worldStateRefill = std::chrono::milliseconds(1);
		return options;
	}

	void AskForWorld(Fixture& a_f, PeerId a_peer, std::uint64_t a_session)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::WorldRequest{ a_session, 0 }));
	}
}

TEST("door share: only a plain door's open state is a player's; locks are shared on everything")
{
	CHECK(DoorShare::SharesOpen({ .door = true }));
	CHECK(!DoorShare::SharesOpen({ .door = false }));
	CHECK(!DoorShare::SharesOpen({ .door = true, .scripted = true }));
	CHECK(!DoorShare::SharesOpen({ .door = true, .doorFlags = DoorShare::DOOR_AUTOMATIC }));
	CHECK(DoorShare::SharesOpen({ .door = true, .doorFlags = 0x08 }));  // minimal use: NPCs seldom use it, a player opens it

	const auto elevator = DoorShare::Shared(RS{ ELEVATOR, RS::kYes, RS::kUnknown }, false);
	CHECK(elevator.open == RS::kUnknown);
	CHECK(!DoorShare::Says(elevator));
	const auto lockedGate = DoorShare::Shared(RS{ SCRIPTED, RS::kYes, RS::kYes }, false);
	CHECK(lockedGate.open == RS::kUnknown);
	CHECK(lockedGate.locked == RS::kYes);
	CHECK(DoorShare::Says(lockedGate));
}

TEST("door share: the host calling an elevator sends nothing, and the friend's elevator is left to its script")
{
	Fixture    f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;

	// The host calls the elevator: its doors and its car open and close as it comes and goes.
	for (int i = 0; i < 20; ++i) {
		for (const auto ref : { ELEVATOR, CAR }) {
			host.doors[ref].open = (i % 2) ? RS::kNo : RS::kYes;
			CHECK(!host.Report(ref));
		}
	}
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);

	// A game that still shares them (an older version) says the elevator is open: the friend's is left alone.
	f.fake->Deliver(1, Protocol::Encode(RS{ ELEVATOR, RS::kYes, RS::kUnknown }, MT::kReportRefState));
	f.fake->Deliver(1, Protocol::Encode(RS{ CAR, RS::kYes, RS::kUnknown }, MT::kReportRefState));
	f.fake->Deliver(1, Protocol::Encode(RS{ AUTOMATIC, RS::kYes, RS::kUnknown }, MT::kReportRefState));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.applied == 0);
	CHECK(friendGame.doors[ELEVATOR].open == RS::kNo);
	CHECK(friendGame.doors[CAR].open == RS::kNo);
	CHECK(friendGame.doors[AUTOMATIC].open == RS::kNo);
}

TEST("door share: a scripted door's lock is still shared, its open state isn't")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;
	host.doors[SCRIPTED].locked = RS::kYes;
	friendGame.doors[SCRIPTED].locked = RS::kYes;

	host.doors[SCRIPTED].locked = RS::kNo;  // picked
	host.doors[SCRIPTED].open = RS::kYes;   // and its script opens it
	const auto report = host.Report(SCRIPTED);
	REQUIRE(report);
	f.fake->Deliver(1, *report);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[SCRIPTED].locked == RS::kNo);
	CHECK(friendGame.doors[SCRIPTED].open == RS::kNo);
	CHECK(friendGame.applied == 1);
}

TEST("door share: a world state asked for again doesn't force a door the game has since closed")
{
	Fixture    f{ Unhurried() };
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;

	// The host opens the door; the friend's opens with it.
	host.doors[DOOR].open = RS::kYes;
	f.fake->Deliver(1, *host.Report(DOOR));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[DOOR].open == RS::kYes);

	// Later an NPC closes it in the host's game, after the host stopped watching it: nobody reports it.
	host.doors[DOOR].open = RS::kNo;
	host.lastSent.clear();

	// The host's game misses a container change and asks for the world again, and again: the session
	// still says the door is open, which the host's game knew. It stays closed.
	for (int i = 0; i < 3; ++i) {
		AskForWorld(f, 1, welcome->sessionId);
		const auto got = Received(f, 1);
		REQUIRE(WorldStates(got).size() == 1);
		REQUIRE(WorldStates(got)[0].refStates.size() == 1);
		host.Take(got);
	}
	CHECK(host.doors[DOOR].open == RS::kNo);
	CHECK(host.applied == 0);

	// The friend closes and opens it: live news, taken.
	friendGame.doors[DOOR].open = RS::kNo;
	f.fake->Deliver(2, *friendGame.Report(DOOR));
	host.Take(Received(f, 1));
	friendGame.doors[DOOR].open = RS::kYes;
	f.fake->Deliver(2, *friendGame.Report(DOOR));
	host.Take(Received(f, 1));
	CHECK(host.doors[DOOR].open == RS::kYes);
	CHECK(host.applied == 1);  // the close found it closed already

	// After a load the game's doors are the save's: the next world state is applied in full, once.
	host.doors[DOOR].open = RS::kNo;
	host.book.Clear();
	host.lastSent.clear();
	AskForWorld(f, 1, welcome->sessionId);
	host.Take(Received(f, 1));
	CHECK(host.doors[DOOR].open == RS::kYes);
	CHECK(host.applied == 2);
}

TEST("door share: a joiner's world state opens the doors the session opened, and its repeats change nothing")
{
	Fixture f{ Unhurried() };
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	DoorGame host, joiner;
	host.doors[DOOR].open = RS::kYes;
	f.fake->Deliver(1, *host.Report(DOOR));
	Clear(f, { 1 });

	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	joiner.Take(Received(f, 2));
	CHECK(joiner.doors[DOOR].open == RS::kYes);
	CHECK(joiner.applied == 1);

	joiner.doors[DOOR].open = RS::kNo;  // its own NPC closes it
	AskForWorld(f, 2, welcome->sessionId);
	joiner.Take(Received(f, 2));
	CHECK(joiner.doors[DOOR].open == RS::kNo);
	CHECK(joiner.applied == 1);
}
