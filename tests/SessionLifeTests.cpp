// A host's saved session through what players live through: friends arriving before the host's own
// game, the host restarting from the session file, saves loaded, reconnects. Each test drives the real
// Server with the messages real games send, as two or three players.

#include "ServerFixture.h"
#include "SessionFile.h"
#include "game/ContainerLedger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <random>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint64_t HOST = 0x40575;        // the host's game's Hello::identity
	constexpr std::uint64_t FRIEND = 0xF41E2D;     // a friend's
	constexpr std::uint32_t HOST_HASH = 0x1234;    // Fixture::MakeHello's load order
	constexpr std::uint32_t OTHER_HASH = 0x9999;   // a friend with another mod list (an extra texture pack's esm)
	constexpr std::uint32_t RAIDER = 0x0001F001;   // NPCs and things from plugin files (shareable)
	constexpr std::uint32_t RAIDER2 = 0x0001F002;
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t STIMPAK = 0x00023736;

	// A session file of its own for each test: test runs (and other worktrees' runs) overlap.
	struct TempFile
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("f4mp-life-{:016X}.session", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
		~TempFile() { std::filesystem::remove(path); }
	};

	Server::Options HostedOptions(const TempFile& a_file)
	{
		Server::Options options;
		options.hostIdentity = HOST;
		options.sessionFile = a_file.path.string();
		return options;
	}

	Protocol::Hello Hello(std::string a_name, std::uint64_t a_identity, std::uint32_t a_hash = HOST_HASH)
	{
		auto hello = Fixture::MakeHello(std::move(a_name), a_identity);
		hello.contentHash = a_hash;
		return hello;
	}

	// The host's own game: its identity, from this machine.
	std::optional<Protocol::Welcome> JoinHost(Fixture& a_f, PeerId a_peer, std::uint32_t a_hash = HOST_HASH)
	{
		a_f.fake->SetLocal(a_peer);
		return a_f.Join(a_peer, Hello("Host", HOST, a_hash));
	}

	std::optional<Protocol::Reject> Refused(Fixture& a_f, PeerId a_peer, const Protocol::Hello& a_hello)
	{
		a_f.fake->Connect(a_peer);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kReject));
		return packet ? Protocol::DecodeReject(*packet) : std::nullopt;
	}

	// Returns once the server has handled everything delivered so far (it answers a heartbeat at once).
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

	std::vector<Protocol::WorldState> WorldStates(const Packets& a_packets)
	{
		std::vector<Protocol::WorldState> out;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(MT::kWorldState)) {
				auto state = Protocol::DecodeWorldState(packet);
				REQUIRE(state);
				out.push_back(std::move(*state));
			}
		}
		return out;
	}

	std::set<std::uint32_t> DeadIn(const std::vector<Protocol::WorldState>& a_states)
	{
		std::set<std::uint32_t> dead;
		for (const auto& state : a_states) {
			dead.insert(state.deadActors.begin(), state.deadActors.end());
		}
		return dead;
	}

	auto Kill(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true }, MT::kReportDeath);
	}

	// A session the host played before: one raider dead, one chest looted, by load order HOST_HASH.
	SessionFile::Snapshot PlayedSession()
	{
		SessionFile::Snapshot saved;
		saved.contentHash = HOST_HASH;
		saved.sessionId = 0x5E55;
		saved.nextId = 3;
		saved.identities = { { HOST, 1 }, { FRIEND, 2 } };
		saved.deadActors = { RAIDER };
		saved.containerChanges = { { 0, 2, { CHEST, STIMPAK, -3 } } };
		return saved;
	}
}

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

TEST("session life: a dedicated server, with no host, still lets the first arrival decide")
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

// ---- container changes through saves, reloads and restarts --------------------------------------

namespace
{
	constexpr std::int32_t STIMPAKS_IN_CHEST = 10;  // in every player's world, before anyone loots

	// A player's game as far as one chest goes: the stimpaks in it in this player's world, the ledger WorldSync
	// keeps of the session's container changes, and what a save holds of both. What the server sends it goes
	// where Session and WorldSync send it.
	struct Game
	{
		struct Save
		{
			std::int32_t                          stimpaks = STIMPAKS_IN_CHEST;
			std::uint64_t                         session = 0;  // the co-save record (WorldSync writes none for 0)
			std::uint32_t                         next = 0;
			std::vector<ContainerLedger::Pending> waiting;
		};

		Fixture*                         f;
		PeerId                           peer;
		std::uint64_t                    identity;
		ContainerLedger                  ledger;
		std::int32_t                     stimpaks = STIMPAKS_IN_CHEST;
		std::uint32_t                    applied = 0;  // changes applied to this world, to catch one applied twice
		std::optional<Protocol::Welcome> welcome;

		bool Apply(const Protocol::ContainerChange& a_change)
		{
			if (a_change.container == CHEST && a_change.item == STIMPAK) {
				stimpaks += a_change.count;
				++applied;
			}
			return true;
		}

		// Connects and says hello with what the world holds, as Session does, and takes in what comes back.
		std::optional<Protocol::Welcome> Join(bool a_local = false)
		{
			auto hello = Hello(identity == HOST ? "Host" : "Friend", identity);
			hello.world = ledger.ResyncPoint();
			if (a_local) {
				f->fake->SetLocal(peer);
			}
			f->fake->Connect(peer);
			f->fake->Deliver(peer, Protocol::Encode(hello));
			Process();
			return welcome;
		}

		// Everything the server sent so far, in order.
		void Process()
		{
			const auto tryApply = [this](const Protocol::ContainerChange& a_change) { return Apply(a_change); };
			for (const auto& packet : Received(*f, peer)) {
				switch (static_cast<MT>(packet[0])) {
				case MT::kWelcome:
					welcome = Protocol::DecodeWelcome(packet);
					REQUIRE(welcome);
					ledger.OnWelcome(welcome->sessionId, welcome->playerId);
					break;
				case MT::kWorldState:
					{
						const auto state = Protocol::DecodeWorldState(packet);
						REQUIRE(state);
						ledger.OnWorldState(state->containerFirstIndex, !state->containerChanges.empty());
						for (const auto& change : state->containerChanges) {
							ledger.Offer(change.index, change.playerId, change.change, tryApply);
						}
					}
					break;
				case MT::kContainerChanged:
					{
						const auto change = Protocol::DecodeIndexedContainerChange(packet);
						REQUIRE(change);
						ledger.Offer(change->index, change->playerId, change->change, tryApply);
					}
					break;
				default:
					break;
				}
			}
		}

		// The player takes stimpaks from the chest (or puts some in): in their own world at once, then reported.
		void Loot(std::int32_t a_count)
		{
			stimpaks += a_count;
			f->fake->Deliver(peer, Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, a_count }, MT::kReportContainer));
		}

		[[nodiscard]] Save MakeSave() const { return { stimpaks, ledger.RecordSession(), ledger.Next(), ledger.Waiting() }; }

		// Loads a save: the world goes back to the save's, the co-save record comes back (or none), and while
		// connected Session asks for the session's changes since (WorldSync's Revert, Load and BindToLiveSession,
		// then Session::OnGameLoaded). What the server sent meanwhile is still to be processed, as in the game.
		void Load(const Save& a_save)
		{
			stimpaks = a_save.stimpaks;
			ledger.OnRevert();
			if (a_save.session != 0) {
				ledger.OnRecordLoaded(a_save.session, a_save.next, a_save.waiting);
			}
			ledger.BindToLiveSession();
			if (welcome) {
				f->fake->Deliver(peer, Protocol::Encode(ledger.ResyncPoint()));
			}
		}

		void Drop()
		{
			f->fake->Drop(peer);
			ledger.OnDisconnected();
			welcome.reset();
		}
	};
}

TEST("session life: loot a friend took is taken once in my world, through a death reload to before I joined and a reload of a later autosave")
{
	TempFile   file;
	Fixture    f{ HostedOptions(file) };
	Game       host{ &f, 1, HOST };
	Game       me{ &f, 2, FRIEND };
	const auto beforeJoining = me.MakeSave();  // my own save, from before the session: no record of it
	REQUIRE(host.Join(true));
	REQUIRE(me.Join());

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
	me.Drop();
	REQUIRE(me.Join());
	CHECK(me.stimpaks == 5);
	CHECK(host.stimpaks == 5);
	CHECK(me.applied == 4);  // 3 then 2 live, and each once more after the loads back to before them
}

TEST("session life: a resent loot message, or a second hello's world state, doesn't take the loot again")
{
	TempFile file;
	Fixture  f{ HostedOptions(file) };
	Game     host{ &f, 1, HOST };
	Game     me{ &f, 2, FRIEND };
	REQUIRE(host.Join(true));
	REQUIRE(me.Join());
	host.Loot(-3);
	host.Process();
	me.Process();
	REQUIRE(me.stimpaks == 7);

	const auto tryApply = [&](const Protocol::ContainerChange& a_change) { return me.Apply(a_change); };
	CHECK(me.ledger.Offer(0, host.welcome->playerId, { CHEST, STIMPAK, -3 }, tryApply) == ContainerLedger::Offered::kHave);
	f.fake->Deliver(2, Protocol::Encode(me.ledger.ResyncPoint()));  // asked again (a load with the same record)
	me.Process();
	CHECK(me.stimpaks == 7);
	CHECK(me.applied == 1);
}

TEST("session life: loot taken while my save loads reaches me once, together with the loot before it")
{
	TempFile file;
	Fixture  f{ HostedOptions(file) };
	Game     host{ &f, 1, HOST };
	Game     me{ &f, 2, FRIEND };
	REQUIRE(host.Join(true));
	REQUIRE(me.Join());
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
		Fixture f{ HostedOptions(file) };
		Game    host{ &f, 1, HOST };
		Game    me{ &f, 2, FRIEND };
		REQUIRE(host.Join(true));
		REQUIRE(me.Join());
		host.Loot(-3);  // #0
		host.Loot(-2);  // #1
		host.Process();
		me.Process();
		REQUIRE(me.stimpaks == 5);
		mine = me.MakeSave();  // I save with both
	}
	// The host's game quit before the server wrote #1: the file has only #0.
	auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	REQUIRE(saved->containerChanges.size() == 2);
	saved->containerChanges.resize(1);
	REQUIRE(SessionFile::Save(file.path, *saved));

	Fixture f{ HostedOptions(file) };
	Game    host{ &f, 1, HOST };
	Game    me{ &f, 2, FRIEND };
	me.Load(mine);  // next evening: I start my game from that save, and join
	REQUIRE(host.Join(true));
	REQUIRE(me.Join());
	CHECK(f.Logged("has container changes up to #2, the session only 1"));
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

	// Waits (up to 3 s) for the session file to read back as a_ready says; the server writes it on its own thread.
	template <class Ready>
	std::optional<SessionFile::Snapshot> WaitForFile(const TempFile& a_file, Ready a_ready)
	{
		const auto until = std::chrono::steady_clock::now() + 3s;
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

TEST("session life: a session file that can't be written is tried again at the next check, not taken as written")
{
	TempFile file;
	std::filesystem::create_directory(file.path);  // where the file should go is a folder: every write fails
	auto options = HostedOptions(file);
	options.saveInterval = 50ms;
	{
		Fixture f{ options };
		REQUIRE(JoinHost(f, 1));
		f.fake->Deliver(1, Kill(RAIDER));
		Sync(f, 1);
		const auto until = std::chrono::steady_clock::now() + 3s;
		while (f.LoggedCount("could not save the session") < 2 && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(10ms);
		}
		CHECK(f.LoggedCount("could not save the session") >= 2);
	}
	std::filesystem::remove(file.path.string() + ".tmp");
	std::filesystem::remove(file.path);
}
