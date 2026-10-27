// Which doors' open and lock states a game shares, and which of the session's it takes again
// (src/game/DoorShare.h), driven through the session server: an elevator's open state is its script's,
// and a world state that repeats what a game knows doesn't force its doors again.

#include "ServerFixture.h"

#include "game/DoorShare.h"
#include "game/SharedGates.h"

#include <map>
#include <set>
#include <string_view>

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
	constexpr std::uint32_t CHAINED = 0x000645DA;   // UtilMetalDoor01 chained shut from one side (ChainedDoor)
	constexpr std::uint32_t LOAD = 0x0003D004;      // a door to another cell

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
		std::map<std::uint32_t, RS>   lastSent;  // what the session last heard of each from us, or where it stood when the poll found it
		std::set<std::uint32_t>       quiet;     // on watch by the poll, not by the player's use
		std::set<std::uint32_t>       held;      // changed too far from the thing to report
		DoorShare::Book               book;
		int                           applied = 0;

		DoorGame()
		{
			doors[DOOR] = { { .door = true } };
			doors[ELEVATOR] = { { .door = false } };
			doors[CAR] = { { .door = false } };
			doors[SCRIPTED] = { { .door = true, .scripts = 1 } };
			doors[AUTOMATIC] = { { .door = true, .doorFlags = DoorShare::DOOR_AUTOMATIC } };
			doors[CHAINED] = { { .door = true, .scripts = 1, .quietScripts = 1 } };
			doors[LOAD] = { { .door = true } };
		}

		RS Read(std::uint32_t a_ref) const
		{
			const auto& door = doors.at(a_ref);
			return DoorShare::Shared(RS{ a_ref, door.open, door.locked }, DoorShare::SharesOpen(door.facts));
		}

		// The watcher's look at a_ref (WatchRefStates, DoorShare::OnLook): its report, if it says something new.
		std::optional<std::vector<std::uint8_t>> Look(std::uint32_t a_ref, bool a_near, bool a_pending = false)
		{
			const auto state = Read(a_ref);
			if (!DoorShare::Says(state)) {
				return std::nullopt;
			}
			DoorShare::Watch look;
			look.quiet = quiet.contains(a_ref);
			look.seen = lastSent.contains(a_ref);
			look.changed = !look.seen || !state.SameState(lastSent.at(a_ref));
			look.near = a_near;
			look.pending = a_pending;
			switch (DoorShare::OnLook(look)) {
			case DoorShare::Look::kBaseline:
				lastSent[a_ref] = state;
				return std::nullopt;
			case DoorShare::Look::kHold:
				held.insert(a_ref);
				return std::nullopt;
			case DoorShare::Look::kSend:
				lastSent[a_ref] = state;
				held.erase(a_ref);
				book.Sent(state);
				return Protocol::Encode(state, MT::kReportRefState);
			case DoorShare::Look::kNothing:
				return std::nullopt;
			}
			return std::nullopt;
		}

		// The watcher's report of a_ref the player used, standing at it.
		std::optional<std::vector<std::uint8_t>> Report(std::uint32_t a_ref)
		{
			return Look(a_ref, true);
		}

		// The poll comes across a_ref loaded (PollDoors): a plain door goes on watch quietly, and is read.
		std::optional<std::vector<std::uint8_t>> Poll(std::uint32_t a_ref, bool a_near)
		{
			if (!DoorShare::SharesOpen(doors.at(a_ref).facts)) {
				return std::nullopt;  // not polled: an activator's, an automatic door's, a scripted one's
			}
			if (!lastSent.contains(a_ref)) {
				quiet.insert(a_ref);
			}
			return Look(a_ref, a_near);
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
	CHECK(!DoorShare::SharesOpen({ .door = true, .scripts = 1 }));
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

TEST("door share: a reference nothing is known of does not share its open state")
{
	// Facts left as they come (not a door, no flags, no script) say nothing a player opens.
	CHECK(!DoorShare::SharesOpen(DoorShare::Facts{}));
	CHECK(!DoorShare::SharesOpen({ .doorFlags = 0x08 }));
	CHECK(!DoorShare::SharesOpen({ .scripts = 1, .quietScripts = 1 }));
	CHECK(!DoorShare::Facts{}.door);
	CHECK(DoorShare::Facts{}.scripts == 0);
	CHECK(!DoorShare::Facts{}.listedOut);
	CHECK(DoorShare::Facts{}.doorFlags == 0);
}

TEST("door share: a door whose scripts are all quiet shares its open state; any other script, or the story's, doesn't")
{
	CHECK(DoorShare::SharesOpen({ .door = true, .scripts = 1, .quietScripts = 1 }));                  // ChainedDoor
	CHECK(DoorShare::SharesOpen({ .door = true, .scripts = 2, .quietScripts = 2 }));                  // DefaultRefEnableLinkedRefOnActivate and DefaultRefOnActivate
	CHECK(!DoorShare::SharesOpen({ .door = true, .scripts = 2, .quietScripts = 1 }));                 // POI_OnOpenActivateAmbush beside PulowskShelterScript
	CHECK(!DoorShare::SharesOpen({ .door = true, .scripts = 1, .quietScripts = 1, .listedOut = true }));  // Mass Fusion's
	CHECK(!DoorShare::SharesOpen({ .door = true, .doorFlags = DoorShare::DOOR_AUTOMATIC, .scripts = 1, .quietScripts = 1 }));
	CHECK(!DoorShare::SharesOpen({ .door = false, .scripts = 1, .quietScripts = 1 }));  // an activator with such a script

	// By the VM's names, whatever their case (a reference's copy is often lower case in the game's files).
	CHECK(DoorShare::IsQuiet("ChainedDoor"));
	CHECK(DoorShare::IsQuiet("defaultrefonactivate"));
	CHECK(DoorShare::IsQuiet("DefaultRefEnableLinkedRefOnActivate"));
	CHECK(DoorShare::IsQuiet("DefaultActivateLinkOnUnlock"));
	CHECK(DoorShare::IsQuiet("defaultblockfolloweractivation"));
	// Scripts that answer the door opening, open or close it themselves, block it or are each player's own.
	for (const auto loud : { "DefaultRefOnOpen", "POI_OnOpenActivateAmbush", "DLC04:DLC04_GauntletMonkeyTrapScript", "PulowskShelterScript", "Vault111LeverScript",
			 "loadelevatordoorscript", "LoadElevatorDoorScript", "ElevatorDoorForwardActivationScript", "InvisibleDoorBlockActivationScript", "DefaultBlockActivationOnLoad",
			 "DisableEnableLinkOnActivateOnce", "StimObjectScript", "SentinelSiteDoorBlockAfterActivate", "MS17CovenantDoor", "MS17DoorLockOnLoadScript",
			 "DN130_CambridgePDDoorStateToggle", "DLC01:DLC01Lair_SecurityDoorScript", "DLC03:DLC03POI04LockedBunkerDoorScript", "REObject02FakeDoorScript",
			 "CharGenFridgeDoorScript", "ChainedDoorX", "" }) {
		CHECK(!DoorShare::IsQuiet(loud));
	}
}

TEST("door share: the quiet scripts and the never list name each once, with a reason")
{
	std::set<std::string_view> names;
	for (const auto& quiet : DoorShare::QUIET_SCRIPTS) {
		CHECK(!quiet.why.empty());
		CHECK(names.insert(quiet.script).second);
	}
	std::set<std::uint32_t> ids;
	for (const auto& form : DoorShare::NEVER) {
		CHECK(form.file == "Fallout4.esm");
		CHECK(form.id != 0 && form.id < 0x01000000);
		CHECK(!form.what.empty());
		CHECK(ids.insert(form.id).second);
	}
	CHECK(ids.contains(0x0009D568));
}

TEST("door share: a chained door the host opens from its unchained side opens for the friend")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;
	host.doors[CHAINED].locked = RS::kYes;
	friendGame.doors[CHAINED].locked = RS::kYes;

	// The chain (an activator) unlocks it, then the host opens it.
	host.doors[CHAINED].locked = RS::kNo;
	host.doors[CHAINED].open = RS::kYes;
	const auto report = host.Report(CHAINED);
	REQUIRE(report);
	f.fake->Deliver(1, *report);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[CHAINED].locked == RS::kNo);
	CHECK(friendGame.doors[CHAINED].open == RS::kYes);
	CHECK(friendGame.applied == 2);
}

TEST("door share: a look at a watched door: a polled door's first read is its baseline, a change goes from near and waits from far")
{
	using DoorShare::Look;
	using DoorShare::OnLook;
	// The player used it: its state goes at once (the first read too), from where they stand.
	CHECK(OnLook({ .quiet = false, .seen = false, .changed = true, .near = true }) == Look::kSend);
	CHECK(OnLook({ .quiet = false, .seen = true, .changed = true, .near = true }) == Look::kSend);
	CHECK(OnLook({ .quiet = false, .seen = true, .changed = false, .near = true }) == Look::kNothing);
	// A terminal's door down the corridor: a friend would refuse the report from here, so it waits for us to come near.
	CHECK(OnLook({ .quiet = false, .seen = false, .changed = true, .near = false }) == Look::kHold);
	CHECK(OnLook({ .quiet = false, .seen = true, .changed = true, .near = false }) == Look::kHold);
	// The poll found it loaded: where it stands is kept, not reported; a change from that is, as above.
	CHECK(OnLook({ .quiet = true, .seen = false, .changed = true, .near = true }) == Look::kBaseline);
	CHECK(OnLook({ .quiet = true, .seen = false, .changed = true, .near = false }) == Look::kBaseline);
	CHECK(OnLook({ .quiet = true, .seen = true, .changed = false, .near = true }) == Look::kNothing);
	CHECK(OnLook({ .quiet = true, .seen = true, .changed = true, .near = true }) == Look::kSend);
	CHECK(OnLook({ .quiet = true, .seen = true, .changed = true, .near = false }) == Look::kHold);
	// The session's state of it waits to be applied here: what we read is the old one, not news.
	CHECK(OnLook({ .quiet = true, .seen = true, .changed = true, .near = true, .pending = true }) == Look::kNothing);
	CHECK(OnLook({ .quiet = false, .seen = false, .changed = true, .near = true, .pending = true }) == Look::kNothing);
	CHECK(OnLook({}) == Look::kNothing);
	// From how near: a friend takes a door's report from 1500 (WorldSync's ACTIVATION_REACH), a gate's from
	// SharedGates::REACH, measured from where they last saw us.
	static_assert(DoorShare::REPORT_REACH < 1500.0f);
	static_assert(DoorShare::REPORT_REACH == DoorShare::NPC_REACH);
	static_assert(SharedGates::REPORT_REACH < SharedGates::REACH);
	static_assert(SharedGates::REACH - SharedGates::REPORT_REACH >= 1500.0f - DoorShare::REPORT_REACH);
}

TEST("door share: a plain door a script unlocks and opens in the host's game does the same for the friend once the host is near it")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;
	host.doors[DOOR].locked = RS::kYes;
	friendGame.doors[DOOR].locked = RS::kYes;

	// The poll comes across the door locked and shut, in both games: where it stands, nothing sent.
	CHECK(!host.Poll(DOOR, false));
	CHECK(!friendGame.Poll(DOOR, true));
	CHECK(!host.Poll(DOOR, true));
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);

	// A quest stage unlocks it in the host's game, with the host across the building: kept until they come near.
	host.doors[DOOR].locked = RS::kNo;
	CHECK(!host.Poll(DOOR, false));
	CHECK(host.held.contains(DOOR));
	CHECK(!host.Poll(DOOR, false));
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);

	// Near: reported once; the friend's unlocks, and their own poll doesn't send it back.
	const auto report = host.Poll(DOOR, true);
	REQUIRE(report);
	CHECK(!host.held.contains(DOOR));
	CHECK(!host.Poll(DOOR, true));
	f.fake->Deliver(1, *report);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[DOOR].locked == RS::kNo);
	CHECK(friendGame.applied == 1);
	CHECK(!friendGame.Poll(DOOR, true));

	// The script opens it too, with the host still near: the friend's opens.
	host.doors[DOOR].open = RS::kYes;
	const auto opened = host.Poll(DOOR, true);
	REQUIRE(opened);
	f.fake->Deliver(1, *opened);
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[DOOR].open == RS::kYes);
	CHECK(friendGame.applied == 2);

	// A change that goes back before the host comes near is nothing: the session's word still stands.
	host.doors[DOOR].open = RS::kNo;
	CHECK(!host.Poll(DOOR, false));
	host.doors[DOOR].open = RS::kYes;
	CHECK(!host.Poll(DOOR, true));
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);
}

TEST("door share: the poll reports no door the save has open already, and reads only plain doors")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	DoorGame host;

	// A save with the door open, and a locked chained door: where they stand, nothing sent as the host walks past.
	host.doors[DOOR].open = RS::kYes;
	host.doors[CHAINED].locked = RS::kYes;
	CHECK(!host.Poll(DOOR, true));
	CHECK(!host.Poll(CHAINED, true));
	CHECK(host.quiet.contains(DOOR) && host.quiet.contains(CHAINED));

	// An elevator's doors, an automatic door, a scripted door: not polled, whatever they do (their locks are still
	// shared when the player uses them).
	for (const auto ref : { ELEVATOR, CAR, AUTOMATIC, SCRIPTED }) {
		host.doors[ref].open = RS::kYes;
		CHECK(!host.Poll(ref, true));
		CHECK(!host.quiet.contains(ref));
	}
	host.doors[SCRIPTED].locked = RS::kNo;
	CHECK(!host.Poll(SCRIPTED, true));
	CHECK(host.Report(SCRIPTED));

	// The player then uses the open door: shut, reported as ever.
	host.doors[DOOR].open = RS::kNo;
	CHECK(host.Report(DOOR));
	CHECK(!host.Poll(DOOR, true));

	// After a load the game's doors are the save's: the poll takes them as they stand, once more.
	host.lastSent.clear();
	host.quiet.clear();
	host.doors[DOOR].open = RS::kYes;
	CHECK(!host.Poll(DOOR, true));
	CHECK(host.lastSent.at(DOOR).open == RS::kYes);
}

TEST("door share: only a plain door near our player that an NPC our game runs opens is watched")
{
	const DoorShare::NpcOpen ours{ .runsHere = true, .sharesOpen = true, .loadDoor = false, .playerDistance = 400.0f };
	CHECK(DoorShare::WatchesNpcOpen(ours));
	auto open = ours;
	open.runsHere = false;  // a friend's NPC (our copy has its AI off), or a stand-in
	CHECK(!DoorShare::WatchesNpcOpen(open));
	open = ours;
	open.sharesOpen = false;  // an elevator's door, an automatic door, a scripted one
	CHECK(!DoorShare::WatchesNpcOpen(open));
	open = ours;
	open.loadDoor = true;
	CHECK(!DoorShare::WatchesNpcOpen(open));
	open = ours;
	open.playerDistance = DoorShare::NPC_REACH + 1.0f;  // the friend would refuse our report of it
	CHECK(!DoorShare::WatchesNpcOpen(open));
	open.playerDistance = DoorShare::NPC_REACH;
	CHECK(DoorShare::WatchesNpcOpen(open));
	static_assert(DoorShare::NPC_REACH < 1500.0f);
	CHECK(!DoorShare::WatchesNpcOpen(DoorShare::NpcOpen{}));
}

TEST("door share: a door an NPC the host's game runs walks through opens and closes behind it in the friend's game")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DoorGame host, friendGame;

	// Our NPC opens the door; the door goes on watch.
	REQUIRE(DoorShare::WatchesNpcOpen({ .runsHere = true, .sharesOpen = DoorShare::SharesOpen(host.doors[DOOR].facts), .playerDistance = 300.0f }));
	host.doors[DOOR].open = RS::kYes;
	f.fake->Deliver(1, *host.Report(DOOR));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[DOOR].open == RS::kYes);

	// It closes behind the NPC a moment later: still watched, so the friend's closes too.
	host.doors[DOOR].open = RS::kNo;
	f.fake->Deliver(1, *host.Report(DOOR));
	friendGame.Take(Received(f, 2));
	CHECK(friendGame.doors[DOOR].open == RS::kNo);
	CHECK(friendGame.applied == 2);

	// The friend's own NPC opens it later: the host takes it the same way, and doesn't report it back.
	friendGame.doors[DOOR].open = RS::kYes;
	f.fake->Deliver(2, *friendGame.Report(DOOR));
	host.Take(Received(f, 1));
	CHECK(host.doors[DOOR].open == RS::kYes);
	CHECK(!host.Report(DOOR));

	// An NPC through a load door or a scripted door: not watched, nothing sent.
	CHECK(!DoorShare::WatchesNpcOpen({ .runsHere = true, .sharesOpen = true, .loadDoor = true, .playerDistance = 300.0f }));
	CHECK(!DoorShare::WatchesNpcOpen({ .runsHere = true, .sharesOpen = DoorShare::SharesOpen(host.doors[SCRIPTED].facts), .playerDistance = 300.0f }));
}
