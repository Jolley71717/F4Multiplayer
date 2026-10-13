// Deaths that end: a raider killed in the session is dead for everyone, until its cell resets in the host's game
// and the host's game, which knows the session's deaths (SessionDeaths), offers to run it again. The tests play the
// players against a real server and feed what it sends into each game's own ledger, as WorldSync does.

#include "ServerFixture.h"

#include "game/SessionDeaths.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Reason = Protocol::ClaimReason;
	using SessionDeaths::Clock;

	constexpr std::uint32_t RAIDER = 0x0001F101;
	constexpr std::uint32_t RAIDER2 = 0x0001F102;
	constexpr std::uint64_t HOST_IDENTITY = 0x1111;
	constexpr auto          GRACE = 300ms;  // Options::aliveAgainGrace in these tests
	// What a cell reset takes: days of game time. Only the games' ledgers are told it passed.
	constexpr auto DAYS_LATER = std::chrono::hours(1);

	Server::Options HostedOptions()
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		options.aliveAgainGrace = GRACE;
		return options;
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

	std::size_t Count(const Packets& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return !a_packet.empty() && a_packet[0] == Type(a_type); }));
	}

	std::vector<Protocol::ActorOwner> Owners(const Packets& a_packets)
	{
		std::vector<Protocol::ActorOwner> out;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(MT::kActorOwners)) {
				const auto batch = Protocol::DecodeOwners(packet);
				REQUIRE(batch);
				out.insert(out.end(), batch->begin(), batch->end());
			}
		}
		return out;
	}

	bool HasOwner(const std::vector<Protocol::ActorOwner>& a_owners, std::uint32_t a_ref, std::uint32_t a_player)
	{
		return std::ranges::any_of(a_owners, [&](const auto& a_owner) { return a_owner.refId == a_ref && a_owner.playerId == a_player; });
	}

	// Every NPC the session's world state in a_packets lists dead.
	std::vector<std::uint32_t> WorldDead(const Packets& a_packets)
	{
		std::vector<std::uint32_t> dead;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(MT::kWorldState)) {
				const auto world = Protocol::DecodeWorldState(packet);
				REQUIRE(world);
				dead.insert(dead.end(), world->deadActors.begin(), world->deadActors.end());
			}
		}
		return dead;
	}

	std::vector<std::uint8_t> Claim(std::uint32_t a_ref, Reason a_reason = Reason::kUnowned)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } });
	}

	std::vector<std::uint8_t> Kill(std::uint32_t a_ref, float a_x = 0.0f)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true, a_x, 0.0f, 0.0f }, MT::kReportDeath);
	}

	// One player's game: its peer and what it knows of the session's deaths.
	struct Game
	{
		Fixture&              f;
		PeerId                peer;
		std::uint32_t         id = 0;
		SessionDeaths::Ledger deaths;

		// Joins (as the host's own game, or not) and takes the welcome's world state.
		Game(Fixture& a_f, PeerId a_peer, std::string a_name, bool a_host = false) :
			f(a_f),
			peer(a_peer)
		{
			if (a_host) {
				f.fake->SetLocal(a_peer);
			}
			const auto welcome = f.Join(a_peer, Fixture::MakeHello(std::move(a_name), a_host ? HOST_IDENTITY : 0));
			REQUIRE(welcome);
			id = welcome->playerId;
			deaths.Expect(Clock::now());  // WorldSync::OnWelcome
			Pump();
		}

		// What the server sent this game, applied as WorldSync does: a world state's dead (then it is settled), and
		// each death as it is relayed.
		Packets Pump()
		{
			auto packets = Received(f, peer);
			for (const auto& packet : packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kWorldState)) {
					const auto world = Protocol::DecodeWorldState(packet);
					REQUIRE(world);
					for (const auto ref : world->deadActors) {
						deaths.Add(ref, Clock::now());
					}
					deaths.Settle();
				} else if (packet[0] == Type(MT::kActorDied)) {
					const auto death = Protocol::DecodeActorDeath(packet);
					REQUIRE(death);
					deaths.Add(death->refId, Clock::now());
				}
			}
			return packets;
		}

		// Its game loaded the actor (alive or not), at a_now: true when it came back, as WorldSync::TrackLoads says.
		bool Loads(std::uint32_t a_ref, bool a_alive, Clock::time_point a_now) { return deaths.OnLoaded(a_ref, a_alive, false, a_now); }

		// NpcSync's scan: a loaded, living NPC nobody runs is offered when WorldSync::MayClaim allows. True if sent.
		bool Scan(std::uint32_t a_ref, Clock::time_point a_now)
		{
			if (!deaths.MayClaim(a_ref, a_now)) {
				return false;
			}
			f.fake->Deliver(peer, Claim(a_ref));
			return true;
		}

		// Its player kills the actor: its own death event (WorldSync::Frame) and the report.
		void Kills(std::uint32_t a_ref, float a_x = 0.0f)
		{
			deaths.Add(a_ref, Clock::now());
			f.fake->Deliver(peer, Kill(a_ref, a_x));
		}

		// A save was loaded: WorldSync::Reset, and the request for the session's world (Session::OnGameLoaded).
		void LoadsASave()
		{
			deaths.Reset(Clock::now());
			f.fake->Deliver(peer, Protocol::Encode(Protocol::WorldRequest{}));
		}
	};
}

TEST("alive again: a camp cleared together comes back after a cell reset in the host's game, for everyone")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };

	// Bob's game runs the raider and he kills it; the host's game hears of it and kills its copy.
	f.fake->Deliver(2, Claim(RAIDER));
	bob.Kills(RAIDER, 40.0f);
	host.Pump();
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Scan(RAIDER, Clock::now()));  // its corpse is nobody's to run
	bob.Pump();

	// Days later the camp's cell resets in the host's game: the raider loads alive.
	std::this_thread::sleep_for(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	CHECK(host.Loads(RAIDER, true, later));
	REQUIRE(host.Scan(RAIDER, later));
	CHECK(HasOwner(Owners(bob.Pump()), RAIDER, host.id));
	CHECK(HasOwner(Owners(host.Pump()), RAIDER, host.id));
	CHECK(f.Logged(std::format("{:08X} is alive again in the host's world", RAIDER)));

	// It fights again: its hits on Bob's stand-in reach Bob.
	f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerHit{ bob.id, 12.0f, false, RAIDER }, MT::kPlayerHit));
	CHECK(Count(bob.Pump(), MT::kPlayerDamaged) == 1);

	// Someone joining now is not told it is dead.
	Game carol{ f, 3, "Carol" };
	CHECK(!carol.deaths.Contains(RAIDER));
	CHECK(carol.deaths.Settled(Clock::now()));

	// And it can die again: Bob fights it (his takeover is refused, the host's game just took it) and kills it.
	f.fake->Deliver(2, Claim(RAIDER, Reason::kInteract));
	bob.Kills(RAIDER);
	const auto toHost = host.Pump();
	CHECK(Count(toHost, MT::kActorDied) == 1);
	CHECK(host.deaths.Contains(RAIDER));  // the host's game kills its copy again
	CHECK(HasOwner(Owners(toHost), RAIDER, 0));
	carol.Pump();
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: a newcomer is told who runs a raider that came back instead of finding it dead")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	host.Kills(RAIDER, 10.0f);  // the host kills it alone
	host.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	REQUIRE(host.Loads(RAIDER, true, later));
	REQUIRE(host.Scan(RAIDER, later));
	host.Pump();

	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Bob")));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWelcome)));
	const auto toBob = Received(f, 2);
	CHECK(!std::ranges::contains(WorldDead(toBob), RAIDER));
	CHECK(Count(toBob, MT::kActorDied) == 0);  // no corpse position for it either
	CHECK(HasOwner(Owners(toBob), RAIDER, host.id));
}

TEST("alive again: a joiner's game can't bring the dead back, whatever it claims")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.Kills(RAIDER);
	bob.Pump();
	host.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);

	// Bob's camp reset first. His own game knows its raider is back and offers it; the server says nothing.
	const auto later = Clock::now() + DAYS_LATER;
	CHECK(bob.Loads(RAIDER, true, later));
	REQUIRE(bob.Scan(RAIDER, later));
	f.fake->Deliver(2, Claim(RAIDER, Reason::kCompanion));
	f.fake->Deliver(2, Claim(RAIDER, Reason::kInteract));
	CHECK(Owners(bob.Pump()).empty());
	CHECK(Owners(host.Pump()).empty());
	// Nor do the host's game's takeover or companion claims (a hit or a press can be on a corpse).
	f.fake->Deliver(1, Claim(RAIDER, Reason::kInteract));
	f.fake->Deliver(1, Claim(RAIDER, Reason::kCompanion));
	CHECK(Owners(host.Pump()).empty());

	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: the host loading a save from before the kill doesn't bring the raider back")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	bob.Kills(RAIDER);
	host.Pump();
	bob.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);

	// The host loads an older save: its raider is alive there. Its game knows nothing until the session's world
	// state is in, so it offers nothing...
	host.LoadsASave();
	CHECK(!host.Scan(RAIDER, Clock::now()));
	// ...and a game that offered it at once anyway (an older build, or a race) is not believed: it only just asked.
	f.fake->Deliver(1, Claim(RAIDER));
	const auto toHost = host.Pump();  // the world state, which settles it: the raider is dead after all
	CHECK(Owners(toHost).empty());
	CHECK(std::ranges::contains(WorldDead(toHost), RAIDER));
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Scan(RAIDER, Clock::now() + DAYS_LATER));  // its game kills it (and never offers a corpse)
	CHECK(Owners(bob.Pump()).empty());

	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: a kill the host's game has not heard of yet is not undone by its claim")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	std::this_thread::sleep_for(GRACE + 50ms);  // long past the host's hello

	// Bob kills a raider nobody runs; the host's game, which has it alive and unowned, offers to run it before the
	// news arrives (its claim crossed the death on the wire).
	bob.Kills(RAIDER2);
	f.fake->Deliver(1, Claim(RAIDER2));
	const auto toHost = host.Pump();
	CHECK(Count(toHost, MT::kActorDied) == 1);
	CHECK(Owners(toHost).empty());
	CHECK(!f.Logged("alive again"));
	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER2));
}

TEST("alive again: a raider back in the host's world stays alive after the server restarts, and the rest stay dead")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-alive-again-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	auto options = HostedOptions();
	options.sessionFile = path;
	{
		Fixture f{ options };
		Game    host{ f, 1, "Host", true };
		host.Kills(RAIDER);
		host.Kills(RAIDER2);
		host.Pump();
		std::this_thread::sleep_for(GRACE + 50ms);
		const auto later = Clock::now() + DAYS_LATER;
		REQUIRE(host.Loads(RAIDER, true, later));
		REQUIRE(host.Scan(RAIDER, later));
		host.Pump();
	}  // stopping saves

	{
		Fixture f{ options };
		Game    host{ f, 1, "Host", true };
		Game    bob{ f, 2, "Bob" };
		CHECK(!bob.deaths.Contains(RAIDER));
		CHECK(bob.deaths.Contains(RAIDER2));
		CHECK(host.deaths.Contains(RAIDER2));

		// The other raider's cell resets later: a death from the saved session is old enough at once, the host's
		// hello is what is waited for.
		f.fake->Deliver(1, Claim(RAIDER2));
		CHECK(Owners(host.Pump()).empty());
		std::this_thread::sleep_for(GRACE + 50ms);
		const auto later = Clock::now() + DAYS_LATER;
		REQUIRE(host.Loads(RAIDER2, true, later));
		REQUIRE(host.Scan(RAIDER2, later));
		CHECK(HasOwner(Owners(bob.Pump()), RAIDER2, host.id));
	}
	std::filesystem::remove(path);
}

// --- the ledger on its own -------------------------------------------------------------------

TEST("session deaths: nothing is offered between a load and the session's dead list, or for long if it never comes")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.Add(RAIDER, t0);
	deaths.Reset(t0);
	CHECK(!deaths.Contains(RAIDER));
	CHECK(!deaths.MayClaim(RAIDER2, t0 + 1s));
	deaths.Settle();
	CHECK(deaths.MayClaim(RAIDER2, t0 + 1s));

	deaths.Expect(t0 + 1min);  // welcomed again
	CHECK(!deaths.MayClaim(RAIDER2, t0 + 1min + 1s));
	CHECK(deaths.MayClaim(RAIDER2, t0 + 1min + SessionDeaths::SETTLE_FALLBACK));  // the server refused to send it
}

TEST("session deaths: only a kill that landed, a while ago, and an actor loading alive mean it came back")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.Settle();
	deaths.Add(RAIDER, t0);
	CHECK(!deaths.OnLoaded(RAIDER, false, false, t0 + DAYS_LATER));                      // a corpse loading
	CHECK(!deaths.OnLoaded(RAIDER, true, true, t0 + DAYS_LATER));                        // not killed here yet
	CHECK(!deaths.OnLoaded(RAIDER, true, false, t0 + SessionDeaths::RESPAWN_AFTER / 2));  // our kill still landing
	CHECK(!deaths.MayClaim(RAIDER, t0 + DAYS_LATER));
	CHECK(deaths.OnLoaded(RAIDER, true, false, t0 + DAYS_LATER));
	CHECK(deaths.MayClaim(RAIDER, t0 + DAYS_LATER));
	CHECK(deaths.Revived() == 1);
	CHECK(!deaths.OnLoaded(RAIDER2, true, false, t0 + DAYS_LATER));  // never dead here: nothing came back
}
