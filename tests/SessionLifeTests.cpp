// A host's saved session through what players live through: friends arriving before the host's own
// game, the host restarting from the session file, saves loaded, reconnects. Each test drives the real
// Server with the messages real games send, as two or three players.

#include "ServerFixture.h"
#include "SessionFile.h"

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
