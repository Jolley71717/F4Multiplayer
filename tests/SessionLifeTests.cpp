// A host's saved session through what players live through: friends arriving before the host's own
// game, the host restarting from the session file, saves loaded, reconnects. Each test drives the real
// Server with the messages real games send, as two or three players (see PlayerGame.h).

#include "PlayerGame.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

using namespace TestGame;


// ---- the host's game is first ----------------------------------------------------------------

TEST("session life: a friend whose Join lands before the host's game is turned away, and the saved session survives it")
{
	TempFile file;
	REQUIRE(SessionFile::Save(file.path, PlayedSession()));
	{
		Fixture f{ HostedOptions(file) };
		// The host is still at the main menu. A friend with another mod list clicks Join Game.
		const auto early = Refused(f, 2, Hello("Friend", FRIEND, OTHER_HASH));
		REQUIRE(early);
		CHECK(early->reason == Protocol::REJECT_HOST_NOT_READY);
		CHECK(!f.Logged("load order changed"));

		// The host's game comes in: the session is the saved one, with its dead.
		const auto host = JoinHost(f, 1);
		REQUIRE(host);
		CHECK(host->sessionId == 0x5E55);
		CHECK(host->playerId == 1);
		CHECK(DeadIn(WorldStates(Received(f, 1))) == std::set<std::uint32_t>{ RAIDER });

		// Now the host's load order is the session's: the friend's differs and is refused for that.
		const auto again = Refused(f, 3, Hello("Friend", FRIEND, OTHER_HASH));
		REQUIRE(again);
		CHECK(again->reason != Protocol::REJECT_HOST_NOT_READY);
		CHECK(!f.Logged("load order changed"));

		// With the same mods the friend gets in, as the player they were, into the same world.
		const auto friendIn = f.Join(4, Hello("Friend", FRIEND));
		REQUIRE(friendIn);
		CHECK(friendIn->sessionId == 0x5E55);
		CHECK(friendIn->playerId == 2);
		const auto world = WorldStates(Received(f, 4));
		CHECK(DeadIn(world) == std::set<std::uint32_t>{ RAIDER });
		REQUIRE(!world.empty());
		CHECK(world[0].containerChanges.size() == 1);
	}  // stopping saves

	const auto after = SessionFile::Load(file.path);
	REQUIRE(after);
	CHECK(after->sessionId == 0x5E55);
	CHECK(after->contentHash == HOST_HASH);
	CHECK(after->deadActors == std::vector<std::uint32_t>{ RAIDER });
	CHECK(after->containerChanges.size() == 1);
}

TEST("session life: a friend sending the host's identity from another machine, or another game on the host's PC, waits like anyone")
{
	TempFile file;
	REQUIRE(SessionFile::Save(file.path, PlayedSession()));
	Fixture f{ HostedOptions(file) };

	// The identity isn't a secret: from elsewhere it is no host.
	const auto faker = Refused(f, 2, Hello("Faker", HOST, OTHER_HASH));
	REQUIRE(faker);
	CHECK(faker->reason == Protocol::REJECT_HOST_NOT_READY);
	// A bot (or a tunnel to another PC) connects from this machine, without the host's identity.
	f.fake->SetLocal(3);
	const auto bot = Refused(f, 3, Hello("Bot", 0xB07, OTHER_HASH));
	REQUIRE(bot);
	CHECK(bot->reason == Protocol::REJECT_HOST_NOT_READY);
	CHECK(!f.Logged("load order changed"));

	const auto host = JoinHost(f, 1);
	REQUIRE(host);
	CHECK(host->sessionId == 0x5E55);
}

TEST("session life: when everyone has left, a friend coming back waits for the host's game again")
{
	TempFile file;
	Fixture  f{ HostedOptions(file) };
	const auto host = JoinHost(f, 1);
	REQUIRE(host);
	REQUIRE(f.Join(2, Hello("Friend", FRIEND)));
	f.fake->Deliver(2, Kill(RAIDER2));
	Sync(f, 2);
	f.fake->Drop(2);
	f.fake->Drop(1);

	// The host's connection dropped (it is restarting its game); the friend's reconnect comes first.
	const auto early = Refused(f, 3, Hello("Friend", FRIEND, OTHER_HASH));
	REQUIRE(early);
	CHECK(early->reason == Protocol::REJECT_HOST_NOT_READY);

	const auto back = JoinHost(f, 4);
	REQUIRE(back);
	CHECK(back->sessionId == host->sessionId);
	CHECK(DeadIn(WorldStates(Received(f, 4))) == std::set<std::uint32_t>{ RAIDER2 });
}

TEST("session life: a dedicated server, with no host, carries on the saved session for a first arrival with its load order")
{
	TempFile file;
	REQUIRE(SessionFile::Save(file.path, PlayedSession()));
	Server::Options options;
	options.sessionFile = file.path.string();
	Fixture    f{ options };
	const auto first = f.Join(1, Hello("Friend", FRIEND));
	REQUIRE(first);
	CHECK(first->sessionId == 0x5E55);
	CHECK(DeadIn(WorldStates(Received(f, 1))) == std::set<std::uint32_t>{ RAIDER });
}

TEST("session life: on a dedicated server a first arrival with another load order starts the session over, and the others follow it")
{
	TempFile file;
	REQUIRE(SessionFile::Save(file.path, PlayedSession()));
	Server::Options options;
	options.sessionFile = file.path.string();
	std::uint64_t session = 0;
	{
		Fixture    f{ options };
		const auto first = f.Join(1, Hello("Friend", FRIEND, OTHER_HASH));
		REQUIRE(first);
		session = first->sessionId;
		CHECK(session != 0x5E55);
		CHECK(f.Logged("load order changed"));
		CHECK(DeadIn(WorldStates(Received(f, 1))).empty());

		// The next player with the same mods joins that session; one with the saved session's mods is now the odd one out.
		const auto second = f.Join(2, Hello("Other", 0x07E2, OTHER_HASH));
		REQUIRE(second);
		CHECK(second->sessionId == session);
		const auto late = Refused(f, 3, Hello("Host", HOST));
		REQUIRE(late);
		CHECK(late->reason != Protocol::REJECT_HOST_NOT_READY);
		f.fake->Deliver(2, Kill(RAIDER2));
		Sync(f, 2);
	}
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(saved->sessionId == session);
	CHECK(saved->contentHash == OTHER_HASH);
	CHECK(saved->deadActors == std::vector<std::uint32_t>{ RAIDER2 });
}

TEST("session life: a friend whose Join lands before the host's game is told once, tries again within seconds, and gets in once the host is")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.password = "rad";
	Machine machine;
	machine.Start(options);
	Game me{ machine, FRIEND, FRIEND_COOP };
	me.password = "rad";
	me.JoinFriend();
	CHECK(!me.welcome);
	REQUIRE(me.notices.size() == 1);
	CHECK(me.notices[0] == Protocol::REJECT_HOST_NOT_READY);
	const auto soon = me.lastRefusal.retryIn;
	CHECK(!me.Retry());  // the host is still loading: not said again
	CHECK(me.notices.size() == 1);

	// Anything else (a wrong password) is said at every try, and isn't tried every few seconds.
	Game stranger{ machine, 0x57A, FRIEND_SOLO };
	stranger.JoinFriend();
	CHECK(!stranger.Retry());
	CHECK(stranger.notices.size() == 2);
	CHECK(stranger.lastRefusal.retryIn > soon);

	// The host's game comes in: the next try gets in, and the refusal is forgotten.
	Game host{ machine, HOST, HOST_COOP };
	host.password = "rad";
	host.JoinOwnServer();
	REQUIRE(host.welcome);
	REQUIRE(me.Retry());
	CHECK(me.refusals.Last().empty());
	CHECK(me.notices.size() == 1);
}

// ---- container changes through saves, reloads and restarts --------------------------------------

TEST("session life: loot a friend took is taken once in my world, through a death reload to before I joined and a reload of a later autosave")
{
	TempFile file;
	Machine  machine;
	Game     host{ machine, HOST, HOST_COOP };
	Game     me{ machine, FRIEND, FRIEND_COOP };
	const auto beforeJoining = me.MakeSave();  // my own save, from before the session: no record of it
	host.HostOn(HostedOptions(file));
	REQUIRE(host.welcome);
	me.JoinFriend();
	REQUIRE(me.welcome);

	host.Loot(-3);
	host.Process();
	me.Process();
	CHECK(host.stimpaks == 7);
	CHECK(me.stimpaks == 7);

	// I die and the game loads my last save, from before I joined: the session's loot comes again, once.
	me.Load(beforeJoining);
	me.Process();
	CHECK(me.stimpaks == 7);
	const auto autosave = me.MakeSave();  // a door's autosave: it must say which of the session's loot it holds
	CHECK(autosave.session == host.welcome->sessionId);

	host.Loot(-2);
	host.Process();
	me.Process();
	CHECK(me.stimpaks == 5);

	// I die again and load the autosave: only what came after it comes again.
	me.Load(autosave);
	me.Process();
	CHECK(me.stimpaks == 5);

	// My connection drops and comes back: nothing again.
	me.ConnectionLost();
	REQUIRE(me.Retry());
	CHECK(me.stimpaks == 5);
	CHECK(host.stimpaks == 5);
	CHECK(me.applied == 4);  // 3 then 2 live, and each once more after the loads back to before them
}

TEST("session life: a resent loot message, or a second hello's world state, doesn't take the loot again")
{
	TempFile file;
	Machine  machine;
	Game     host{ machine, HOST, HOST_COOP };
	Game     me{ machine, FRIEND, FRIEND_COOP };
	host.HostOn(HostedOptions(file));
	me.JoinFriend();
	REQUIRE(me.welcome);
	host.Loot(-3);
	host.Process();
	me.Process();
	REQUIRE(me.stimpaks == 7);

	// The same change again, as the server sent it (a resend after a lost ack), and the world asked for again.
	me.Handle({ Protocol::Encode(Protocol::IndexedContainerChange{ 0, host.welcome->playerId, { CHEST, STIMPAK, -3 } }) });
	me.Send(Protocol::Encode(me.loot.Point()));
	me.Process();
	CHECK(me.stimpaks == 7);
	CHECK(me.applied == 1);
}

TEST("session life: loot taken while my save loads reaches me once, together with the loot before it")
{
	TempFile file;
	Machine  machine;
	Game     host{ machine, HOST, HOST_COOP };
	Game     me{ machine, FRIEND, FRIEND_COOP };
	host.HostOn(HostedOptions(file));
	me.JoinFriend();
	REQUIRE(me.welcome);
	host.Loot(-1);  // #0
	host.Process();
	me.Process();
	const auto save = me.MakeSave();  // holds #0
	host.Loot(-1);  // #1
	host.Loot(-1);  // #2
	host.Process();
	me.Process();
	REQUIRE(me.stimpaks == 7);

	// The host loots again (#3) while my save loads: #3 reaches me before the world state I ask for after the
	// load, which brings #1 to #3.
	host.Loot(-1);
	host.Process();
	me.Load(save);
	me.Process();
	CHECK(me.stimpaks == 6);
	CHECK(me.applied == 3 + 1 + 2);  // #0 to #2 live; after the load #3 live, then #1 and #2
}

TEST("session life: when the host's game quit before its last loot was saved, the host's next loot still reaches a friend")
{
	TempFile   file;
	Game::Save mine;
	{
		Machine machine;
		Game    host{ machine, HOST, HOST_COOP };
		Game    me{ machine, FRIEND, FRIEND_COOP };
		host.HostOn(HostedOptions(file));
		me.JoinFriend();
		REQUIRE(me.welcome);
		host.Loot(-3);  // #0
		host.Loot(-2);  // #1
		host.Process();
		me.Process();
		REQUIRE(me.stimpaks == 5);
		mine = me.MakeSave();  // I save with both
		host.StopHosting();
	}
	// The host's game quit before the server wrote #1: the file has only #0.
	auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	REQUIRE(saved->containerChanges.size() == 2);
	saved->containerChanges.resize(1);
	REQUIRE(SessionFile::Save(file.path, *saved));

	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	me.Load(mine);  // next evening: I start my game from that save, and join
	host.HostOn(HostedOptions(file));
	me.JoinFriend();
	REQUIRE(me.welcome);
	CHECK(machine.server->Logged("has container changes up to #2, the session only 1"));
	CHECK(me.stimpaks == 5);

	// The server numbers the host's next loot #1, the number my game had counted for the lost one.
	host.Loot(-4);
	host.Process();
	me.Process();
	CHECK(me.stimpaks == 1);
}

// ---- the session file keeps up ----------------------------------------------------------------------

namespace
{
	constexpr std::uint32_t DOOR = 0x0003D001;
	constexpr std::uint32_t WALL = 0xFF000042;  // something built: a runtime reference

	auto DoorReport(std::uint8_t a_open)
	{
		return Protocol::Encode(Protocol::RefState{ DOOR, a_open, Protocol::RefState::kNo }, MT::kReportRefState);
	}

	auto WallReport(float a_x)
	{
		Protocol::WorkshopItem item;
		item.refId = WALL;
		item.base = 0x0001F2A1;
		item.workshop = 0x000250FE;
		item.position[0] = a_x;
		return Protocol::Encode(item, MT::kReportWorkshopItem);
	}

	// Waits (up to 30 s) for the session file to read back as a_ready says; the server writes it on its own thread.
	template <class Ready>
	std::optional<SessionFile::Snapshot> WaitForFile(const TempFile& a_file, Ready a_ready)
	{
		const auto until = std::chrono::steady_clock::now() + 30s;
		do {
			if (auto saved = SessionFile::Load(a_file.path); saved && a_ready(*saved)) {
				return saved;
			}
			std::this_thread::sleep_for(10ms);
		} while (std::chrono::steady_clock::now() < until);
		return std::nullopt;
	}

	std::optional<std::uint8_t> DoorIn(const SessionFile::Snapshot& a_saved)
	{
		const auto it = std::ranges::find(a_saved.refStates, DOOR, &Protocol::RefState::refId);
		return it == a_saved.refStates.end() ? std::nullopt : std::optional{ it->open };
	}

	std::optional<float> WallXIn(const SessionFile::Snapshot& a_saved)
	{
		const auto it = std::ranges::find(a_saved.workshopItems, WALL, &Protocol::WorkshopItem::refId);
		return it == a_saved.workshopItems.end() ? std::nullopt : std::optional{ it->position[0] };
	}
}

TEST("session life: a door closed again and a wall moved reach the file at the next check, and stand that way after the host restarts")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 50ms;
	{
		Fixture f{ options };
		REQUIRE(JoinHost(f, 1));
		REQUIRE(f.Join(2, Hello("Friend", FRIEND)));
		f.fake->Deliver(2, DoorReport(Protocol::RefState::kYes));
		f.fake->Deliver(1, WallReport(10.0f));
		REQUIRE(WaitForFile(file, [](const auto& a_saved) { return DoorIn(a_saved) == Protocol::RefState::kYes && WallXIn(a_saved) == 10.0f; }));

		// Nothing more or less is kept, only different: the file follows anyway.
		f.fake->Deliver(2, DoorReport(Protocol::RefState::kNo));
		f.fake->Deliver(1, WallReport(20.0f));
		CHECK(WaitForFile(file, [](const auto& a_saved) { return DoorIn(a_saved) == Protocol::RefState::kNo && WallXIn(a_saved) == 20.0f; }));
	}

	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	REQUIRE(f.Join(2, Hello("Friend", FRIEND)));
	const auto received = Received(f, 2);
	const auto world = WorldStates(received);
	REQUIRE(!world.empty());
	REQUIRE(world[0].refStates.size() == 1);
	CHECK(world[0].refStates[0].open == Protocol::RefState::kNo);
	bool wallAt20 = false;
	for (const auto& packet : received) {
		if (packet[0] == Type(MT::kWorkshopItems)) {
			const auto items = Protocol::DecodeWorkshopItems(packet);
			REQUIRE(items);
			wallAt20 |= std::ranges::any_of(*items, [](const auto& a_item) { return a_item.refId == WALL && a_item.position[0] == 20.0f; });
		}
	}
	CHECK(wallAt20);
}

TEST("session life: the time of day alone doesn't rewrite the file, but the host's game saving writes it at once")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 50ms;
	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	REQUIRE(WaitForFile(file, [](const auto&) { return true; }));  // the host's identity is new to the session

	Protocol::WorldTime time;
	time.gameHour = 9.0f;
	f.fake->Deliver(1, Protocol::Encode(time, MT::kReportTime));
	Sync(f, 1);
	std::this_thread::sleep_for(300ms);  // six checks
	const auto before = SessionFile::Load(file.path);
	REQUIRE(before);
	CHECK(!before->worldTime);

	f.server.RequestSave();  // the host's game saves (Session::OnBeforeSaveOrLoad)
	const auto after = WaitForFile(file, [](const auto& a_saved) { return a_saved.worldTime.has_value(); });
	REQUIRE(after);
	CHECK(after->worldTime->gameHour == 9.0f);
}

TEST("session life: the host's game dropping out writes what it did at once, not at the next check")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 60s;
	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	f.fake->Deliver(1, Kill(RAIDER));
	Sync(f, 1);
	f.fake->Drop(1);  // its game quit (or crashed)
	const auto saved = WaitForFile(file, [](const auto& a_saved) { return !a_saved.deadActors.empty(); });
	REQUIRE(saved);
	CHECK(saved->deadActors == std::vector<std::uint32_t>{ RAIDER });
}

TEST("session life: a session file that can't be written is tried at every check until it can, and said once, not at every check")
{
	TempFile file;
	std::filesystem::create_directory(file.path);  // where the file should go is a folder: every write fails
	auto options = HostedOptions(file);
	options.saveInterval = 20ms;
	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	f.fake->Deliver(1, Kill(RAIDER));
	Sync(f, 1);
	const auto until = std::chrono::steady_clock::now() + 30s;
	while (!f.Logged("could not save the session") && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(10ms);
	}
	REQUIRE(f.Logged("could not save the session"));
	std::this_thread::sleep_for(200ms);  // ten more checks, each failing
	CHECK(f.LoggedCount("could not save the session") == 1);

	// The folder goes (the player moved it away): the next check writes the kill, with nothing new to write.
	std::filesystem::remove(file.path);
	const auto saved = WaitForFile(file, [](const auto& a_saved) { return !a_saved.deadActors.empty(); });
	REQUIRE(saved);
	CHECK(saved->deadActors == std::vector<std::uint32_t>{ RAIDER });
	CHECK(f.Logged("saved again"));
}

TEST("session life: a save the host's game asked for that failed is tried again by itself, though only the time of day changed")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 20ms;
	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	REQUIRE(WaitForFile(file, [](const auto&) { return true; }));  // the host's identity is new to the session

	// The file can't be replaced (as when antivirus holds it): its place is a folder for now.
	std::filesystem::remove(file.path);
	std::filesystem::create_directory(file.path);

	Protocol::WorldTime time;
	time.gameHour = 9.0f;
	f.fake->Deliver(1, Protocol::Encode(time, MT::kReportTime));
	Sync(f, 1);
	f.server.RequestSave();  // the host's game saves: the time is written, and can't be
	const auto until = std::chrono::steady_clock::now() + 3s;
	while (!f.Logged("could not save the session") && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(10ms);
	}
	REQUIRE(f.Logged("could not save the session"));

	std::filesystem::remove(file.path);  // the file is free again; nothing but the retry writes the time now
	const auto after = WaitForFile(file, [](const auto& a_saved) { return a_saved.worldTime.has_value(); });
	REQUIRE(after);
	CHECK(after->worldTime->gameHour == 9.0f);
	CHECK(f.LoggedCount("could not save the session") == 1);
}

TEST("session life: a session file that can't be written when the server stops is tried one last time")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 60s;
	{
		Fixture f{ options };
		REQUIRE(JoinHost(f, 1));
		f.fake->Deliver(1, Kill(RAIDER));
		Sync(f, 1);
		std::filesystem::remove(file.path);
		std::filesystem::create_directory(file.path);
		f.server.RequestSave();
		const auto until = std::chrono::steady_clock::now() + 3s;
		while (!f.Logged("could not save the session") && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(10ms);
		}
		REQUIRE(f.Logged("could not save the session"));
		std::filesystem::remove(file.path);  // free again, but the wait for the next try is long
	}
	// The server stopping (the fixture going) tried once more and wrote the kill.
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(saved->deadActors == std::vector<std::uint32_t>{ RAIDER });
}

TEST("session life: a session file that still can't be written when the server stops is said")
{
	TempFile file;
	auto     options = HostedOptions(file);
	options.saveInterval = 60s;
	Fixture f{ options };
	REQUIRE(JoinHost(f, 1));
	f.fake->Deliver(1, Kill(RAIDER));
	Sync(f, 1);
	std::filesystem::remove(file.path);
	std::filesystem::create_directory(file.path);  // shut for good
	f.server.Stop();
	CHECK(f.Logged("could not save the session when the server stopped"));
	CHECK(f.LoggedCount("could not save the session to") == 0);  // the stop's own line, not a streak's
}

// ---- a session file that goes wrong on disk ---------------------------------------------------------

namespace
{
	// Two sittings: RAIDER dies in the first, RAIDER2 in the second (the file is written when each ends).
	std::uint64_t PlayTwoSittings(const TempFile& a_file)
	{
		std::uint64_t session = 0;
		for (const auto raider : { RAIDER, RAIDER2 }) {
			Fixture    f{ HostedOptions(a_file) };
			const auto host = JoinHost(f, 1);
			REQUIRE(host);
			session = host->sessionId;
			REQUIRE(f.Join(2, Hello("Friend", FRIEND)));
			f.fake->Deliver(2, Kill(raider));
			Sync(f, 2);
		}
		return session;
	}
}

TEST("session life: a session file damaged on disk is kept aside, and the host carries on from the sitting before")
{
	TempFile   file;
	const auto session = PlayTwoSittings(file);
	{
		std::ofstream damaged{ file.path, std::ios::binary | std::ios::trunc };
		damaged << "FMPS";  // a disk that filled up, a sync tool that cut it short
	}
	{
		Fixture    f{ HostedOptions(file) };
		const auto host = JoinHost(f, 1);
		REQUIRE(host);
		CHECK(host->sessionId == session);
		CHECK(DeadIn(WorldStates(Received(f, 1))) == std::set<std::uint32_t>{ RAIDER });
		CHECK(f.Logged("can't be read"));
		CHECK(f.Logged("backup"));
		// The friend is still who they were in it.
		const auto friendWelcome = f.Join(2, Hello("Friend", FRIEND));
		REQUIRE(friendWelcome);
		f.fake->Deliver(2, Kill(RAIDER2));
		Sync(f, 2);
	}
	CHECK(std::filesystem::file_size(SessionFile::SetAsideOf(file.path)) == 4);  // the damaged one, for a look
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK((std::set<std::uint32_t>(saved->deadActors.begin(), saved->deadActors.end()) == std::set<std::uint32_t>{ RAIDER, RAIDER2 }));
}

TEST("session life: a game that stopped while replacing the session file carries on from what it wrote")
{
	TempFile   file;
	const auto session = PlayTwoSittings(file);
	// Save's state between its two renames: the new file still temporary, the old one the backup.
	std::filesystem::rename(file.path, file.path.string() + ".tmp");
	{
		Fixture    f{ HostedOptions(file) };
		const auto host = JoinHost(f, 1);
		REQUIRE(host);
		CHECK(host->sessionId == session);
		CHECK((DeadIn(WorldStates(Received(f, 1))) == std::set<std::uint32_t>{ RAIDER, RAIDER2 }));
	}
}

TEST("session life: deleting the session file starts the session over, even with a backup beside it")
{
	TempFile   file;
	const auto session = PlayTwoSittings(file);
	REQUIRE(std::filesystem::exists(SessionFile::BackupOf(file.path)));
	std::filesystem::remove(file.path);
	{
		Fixture    f{ HostedOptions(file) };
		const auto host = JoinHost(f, 1);
		REQUIRE(host);
		CHECK(host->sessionId != session);
		CHECK(DeadIn(WorldStates(Received(f, 1))).empty());
	}
}
