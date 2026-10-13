#pragma once

// Players' games as far as the session goes, for tests that play two or three of them against the real Server:
// which character is loaded, the stimpaks in one chest in that world, and what Session and WorldSync keep of the
// session (CharacterBinding's tracker, the loot engine, the refusals). What the server sends goes through the
// steps Session takes (SessionSteps, the loot engine, Refusals); what Session does around them (connect, drop, start and
// stop the server) is a line or two here.

#include "ChestWorld.h"
#include "ServerFixture.h"
#include "SessionFile.h"
#include "game/CharacterBinding.h"
#include "net/Refusals.h"
#include "net/SessionSteps.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace TestGame
{
	using namespace TestServer;
	using namespace std::chrono_literals;
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Step = CharacterBinding::Step;

	constexpr std::uint64_t HOST = 0x40575;       // the host's game's Hello::identity
	constexpr std::uint64_t FRIEND = 0xF41E2D;    // a friend's
	constexpr std::uint32_t HOST_HASH = 0x1234;   // Fixture::MakeHello's load order
	constexpr std::uint32_t OTHER_HASH = 0x9999;  // a friend with another mod list (an extra texture pack's esm)
	constexpr std::uint32_t RAIDER = 0x0001F001;  // NPCs and things from plugin files (shareable)
	constexpr std::uint32_t RAIDER2 = 0x0001F002;
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t STIMPAK = 0x00023736;
	constexpr std::int32_t  STIMPAKS_IN_CHEST = 10;  // in every world, before anyone loots

	// Characters (the game's BGSSaveLoadManager::currentPlayerID): each player's co-op one and solo one.
	constexpr std::uint64_t HOST_COOP = 0x00A1B2C3D4E5F601;
	constexpr std::uint64_t HOST_SOLO = 0x00A1B2C3D4E5F602;
	constexpr std::uint64_t FRIEND_COOP = 0x0071E2D3C4B5A601;
	constexpr std::uint64_t FRIEND_SOLO = 0x0071E2D3C4B5A602;

	using TempFile = TempSessionFile;

	inline Server::Options HostedOptions(const TempFile& a_file)
	{
		Server::Options options;
		options.hostIdentity = HOST;
		options.sessionFile = a_file.path.string();
		return options;
	}

	inline Protocol::Hello Hello(std::string a_name, std::uint64_t a_identity, std::uint32_t a_hash = HOST_HASH)
	{
		auto hello = Fixture::MakeHello(std::move(a_name), a_identity);
		hello.contentHash = a_hash;
		return hello;
	}

	// The host's own game: its identity, from this machine.
	inline std::optional<Protocol::Welcome> JoinHost(Fixture& a_f, PeerId a_peer, std::uint32_t a_hash = HOST_HASH)
	{
		a_f.fake->SetLocal(a_peer);
		return a_f.Join(a_peer, Hello("Host", HOST, a_hash));
	}

	inline std::optional<Protocol::Reject> Refused(Fixture& a_f, PeerId a_peer, const Protocol::Hello& a_hello)
	{
		a_f.fake->Connect(a_peer);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kReject));
		return packet ? Protocol::DecodeReject(*packet) : std::nullopt;
	}

	// Sync, Received and WorldStates are the fixture's (ServerFixture.h).

	inline std::set<std::uint32_t> DeadIn(const std::vector<Protocol::WorldState>& a_states)
	{
		std::set<std::uint32_t> dead;
		for (const auto& state : a_states) {
			dead.insert(state.deadActors.begin(), state.deadActors.end());
		}
		return dead;
	}

	inline std::vector<std::uint8_t> Kill(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true }, MT::kReportDeath);
	}

	// A session the host played before: one raider dead, one chest looted, by load order HOST_HASH.
	inline SessionFile::Snapshot PlayedSession()
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

	// The F4SE folder under Documents, for one test.
	struct TempFolder
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("f4mp-game-{:016X}", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
		TempFolder() { std::filesystem::create_directories(path); }
		~TempFolder()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	struct Game;

	// The host's PC: the server running in the host's game (or one the test started), and the F4SE folder the host's
	// session files go in.
	struct Machine
	{
		TempFolder               folder;
		std::optional<Fixture>   server;
		std::vector<std::string> said;  // what CharacterBinding::PrepareFile told the host
		std::vector<Game*>       games;
		PeerId                   nextPeer = 1;

		// These games die and reload, or drop and rejoin, within moments of each other, more often than the
		// world-state budget allows a real game in ten seconds. Unless a test sets it, the budget refills at once:
		// a rejoin here isn't held back as a real one would be (the server's budget tests try that).
		void Start(Server::Options a_options)
		{
			if (a_options.worldStateRefill == Server::Options{}.worldStateRefill) {
				a_options.worldStateRefill = std::chrono::milliseconds{ 0 };
			}
			server.emplace(a_options);
		}
		// The server stops (writing its file): every connection to it goes.
		void Stop();
	};

	struct Game
	{
		// A save: its character, its world's chest, and WorldSync's co-save record (none for session 0).
		struct Save
		{
			std::uint64_t                         character = 0;
			std::int32_t                          stimpaks = STIMPAKS_IN_CHEST;
			std::uint64_t                         session = 0;
			std::uint32_t                         next = 0;
			std::vector<LootSync::Pending>        waiting;
		};

		Machine&                         machine;
		std::uint64_t                    identity;
		std::uint64_t                    character;
		bool                             wantHost = false;  // bHost
		bool                             inOpening = false;
		std::string                      password;  // sPassword
		// Session's side.
		bool                             hosting = false;
		bool                             joining = false;  // serverAddress is a friend's (or the test's server)
		bool                             connected = false;
		PeerId                           peer = 0;
		std::optional<Protocol::Welcome> welcome;
		CharacterBinding::Tracker        binding;
		LootSync::Loot                   loot;
		// The world as the loot engine sees it: everything is loaded, and only the stimpaks in the chest matter.
		ChestWorld                       chests{ [](std::uint32_t) { return true; }, [this](const Protocol::ContainerChange& a_change) { Apply(a_change); } };
		Refusals                         refusals;
		Refusals::Answer                 lastRefusal;
		std::vector<std::string>         notices;  // what Session would tell the player
		// The world.
		std::int32_t  stimpaks = STIMPAKS_IN_CHEST;
		std::uint32_t applied = 0;  // changes applied to this world, to catch one applied twice

		Game(Machine& a_machine, std::uint64_t a_identity, std::uint64_t a_character) :
			machine(a_machine), identity(a_identity), character(a_character)
		{
			machine.games.push_back(this);
		}
		Game(const Game&) = delete;
		Game& operator=(const Game&) = delete;

		[[nodiscard]] CharacterBinding::Link Link() const { return { wantHost, hosting, joining }; }
		[[nodiscard]] Fixture&               TheServer() const { return *machine.server; }

		// WorldSync's TryApplyContainer: the chest is always loaded here.
		bool Apply(const Protocol::ContainerChange& a_change)
		{
			if (a_change.container == CHEST && a_change.item == STIMPAK) {
				stimpaks += a_change.count;
				++applied;
			}
			return true;
		}

		// Session::Frame's once-a-second check, with the player in the world.
		void Tick()
		{
			switch (binding.Check(Link(), { character, true, inOpening })) {
			case Step::kStopHosting:
				StopHosting();
				break;
			case Step::kStartHosting:
				StartHosting();
				break;
			default:
				break;
			}
		}

		// Session::StartHosting: the loaded character's session file (as the loaded save's record decides), and our
		// own game connects to the server.
		void StartHosting()
		{
			Server::Options options;
			options.hostIdentity = identity;
			options.sessionFile = CharacterBinding::PrepareFile(machine.folder.path, character, false, loot.Point().sessionId, [this](const std::string& a_line) {
				machine.said.push_back(a_line);
			}).string();
			HostOn(options);
		}

		// Session::StartHosting with a server of the test's options.
		void HostOn(const Server::Options& a_options)
		{
			machine.Start(a_options);
			JoinOwnServer();
		}

		// The host's game reaching the server that runs in it (started by HostOn, or by the test).
		void JoinOwnServer()
		{
			binding.OnHostingStarted(character);
			hosting = true;
			ServerChanged();
			Connect();
		}

		// Session::StopHosting: our own connection first, then the server.
		void StopHosting()
		{
			ServerChanged();
			hosting = false;
			machine.Stop();
		}

		// The menu's Join (or a Steam join): the server the test or the host's game runs.
		void JoinFriend()
		{
			ServerChanged();
			joining = true;
			Connect();
		}

		// Session::Leave.
		void Leave()
		{
			ServerChanged();
			joining = false;
		}

		// Session::SwitchServer.
		void ServerChanged()
		{
			if (connected && machine.server) {
				TheServer().fake->Drop(peer);
			}
			Disconnected();
			refusals.Clear();
			SessionSteps::OnServerChanged(binding, loot);
		}

		// Session::OnDisconnected.
		void Disconnected()
		{
			connected = false;
			welcome.reset();
			SessionSteps::OnDisconnected(loot);
		}

		// The connection drops (a network hiccup); Session tries again by itself (Retry).
		void ConnectionLost()
		{
			if (connected && machine.server) {
				TheServer().fake->Drop(peer);
			}
			Disconnected();
		}

		// Session::Frame reconnecting, every few seconds while set to reach a server.
		bool Retry()
		{
			return !connected && (hosting || joining) && Connect();
		}

		// Connects (a new connection), says hello as Session does once connected, and takes in the answer and all that
		// comes with it. False when there's no server, or it didn't let us in.
		bool Connect()
		{
			if (!machine.server) {
				return false;
			}
			auto& f = TheServer();
			peer = machine.nextPeer++;
			if (hosting) {
				f.fake->SetLocal(peer);
			}
			f.fake->Connect(peer);
			connected = true;
			auto hello = Hello(identity == HOST ? "Host" : "Friend", identity);
			hello.world = SessionSteps::OnHello(binding, loot, character);
			hello.password = password;
			f.fake->Deliver(peer, Protocol::Encode(hello));

			Packets    got;
			const auto answered = [&] {
				return std::ranges::any_of(got, [](const auto& a_packet) { return a_packet[0] == Type(MT::kWelcome) || a_packet[0] == Type(MT::kReject); });
			};
			for (const auto until = std::chrono::steady_clock::now() + 2s; !answered() && std::chrono::steady_clock::now() < until;) {
				auto more = f.fake->Take(peer);
				got.insert(got.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
				if (!answered()) {
					std::this_thread::sleep_for(2ms);
				}
			}
			REQUIRE(answered());
			if (std::ranges::any_of(got, [](const auto& a_packet) { return a_packet[0] == Type(MT::kWelcome); })) {
				auto rest = Received(f, peer);
				got.insert(got.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
			}
			Handle(got);
			return welcome.has_value();
		}

		// Everything the server sent so far, in order.
		void Process()
		{
			if (connected && machine.server) {
				Handle(Received(TheServer(), peer));
			}
		}

		// Session::HandlePacket, for what the session and the chest take; then what Frame does after its poll.
		void Handle(const Packets& a_packets)
		{
			bool       leave = false;
			bool       refused = false;
			for (const auto& packet : a_packets) {
				if (leave) {
					break;  // nothing more of this connection is taken
				}
				switch (static_cast<MT>(packet[0])) {
				case MT::kWelcome:
					if (!welcome) {
						const auto msg = Protocol::DecodeWelcome(packet);
						REQUIRE(msg);
						if (SessionSteps::OnWelcome(binding, loot, hosting, *msg) == Step::kLeave) {
							leave = true;
							break;
						}
						welcome = msg;
						refusals.Clear();
					}
					break;
				case MT::kReject:
					{
						const auto msg = Protocol::DecodeReject(packet);
						REQUIRE(msg);
						lastRefusal = refusals.OnReject(msg->reason);
						if (lastRefusal.notify) {
							notices.push_back(msg->reason);
						}
						refused = true;
					}
					break;
				case MT::kWorldState:
					{
						const auto state = Protocol::DecodeWorldState(packet);
						REQUIRE(state);
						loot.CatchUp(state->containerFirstIndex, state->containerChanges, LootSync::Clock::now(), chests);
					}
					break;
				case MT::kContainerChanged:
					{
						const auto change = Protocol::DecodeIndexedContainerChange(packet);
						REQUIRE(change);
						loot.Live(*change, LootSync::Clock::now(), chests);
					}
					break;
				default:
					break;
				}
			}
			if (refused) {
				Disconnected();  // the server closes a refused connection
			}
			if (leave) {
				Leave();
				notices.push_back("left: the host is in another session now");
			}
		}

		// Sends as the player's game.
		void Send(std::vector<std::uint8_t> a_packet) { TheServer().fake->Deliver(peer, std::move(a_packet)); }

		// The player takes stimpaks from the chest (or puts some in): in their own world at once, then reported.
		void Loot(std::int32_t a_count)
		{
			stimpaks += a_count;
			Send(Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, a_count }, MT::kReportContainer));
		}

		[[nodiscard]] Save MakeSave() const { return { character, stimpaks, loot.Point().sessionId, loot.Point().containerFrom, loot.Waiting() }; }

		// Loads a save: the world goes back to the save's and its co-save record comes back (or none: WorldSync's
		// revert and load callbacks), then Session::OnGameLoaded. What the server sent meanwhile is still to be
		// processed, as in the game.
		void Load(const Save& a_save) { LoadWorld(a_save, false); }

		// New Game: a fresh world in Vault 111's opening, its character still the last one's until named.
		void NewGame() { LoadWorld({ character }, true); }

		// In the opening the player confirms the new character's name: it has its own ID from now on.
		void NameCharacter(std::uint64_t a_character) { character = a_character; }

		void LeaveOpening() { inOpening = false; }

	private:
		void LoadWorld(const Save& a_save, bool a_newGame)
		{
			character = a_save.character;
			stimpaks = a_save.stimpaks;
			inOpening = a_newGame;
			loot.LoadRecord(a_save.session, a_save.next, a_save.waiting);  // session 0: the save has no record
			SessionSteps::OnGameLoaded(
				binding, loot, Link(), a_newGame, character,
				[this](Step a_step) {
					if (a_step == Step::kStopHosting) {
						StopHosting();
					} else {
						Leave();
					}
				},
				[this](const Protocol::WorldRequest& a_request) { Send(Protocol::Encode(a_request)); });
		}
	};

	inline void Machine::Stop()
	{
		server.reset();
		for (auto* game : games) {
			if (game->connected) {
				game->Disconnected();
			}
		}
	}
}
