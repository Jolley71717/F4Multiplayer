// Deaths that end: a raider killed in the session is dead for everyone, until its cell resets in the host's game
// and the host's game, which knows the session's deaths (SessionDeaths), offers to run it again. The tests play the
// players against a real server and feed what it sends into each game's own ledger and claim book, as WorldSync and
// NpcSync do; each game's NPCs (loaded or not, alive or not) are modelled, since the engine decides those.

#include "ServerFixture.h"

#include "game/NpcClaims.h"
#include "game/SessionDeaths.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

	// An NPC as one game has it. Essential: essential in this game only (its quest is in another state there), so a
	// console kill leaves it standing.
	struct Npc
	{
		bool loaded = false;
		bool alive = true;
		bool essential = false;
	};

	// One player's game: its peer, what it knows of the session's deaths, its claims, and its NPCs.
	struct Game
	{
		Fixture&                                         f;
		PeerId                                           peer;
		std::uint32_t                                    id = 0;
		SessionDeaths::Ledger                            deaths;
		NpcClaims::Book                                  claims;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;
		std::unordered_map<std::uint32_t, Npc>           world;
		std::unordered_set<std::uint32_t>                pending;  // deaths waiting for the NPC to load (WorldSync's)

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

		[[nodiscard]] bool Alive(std::uint32_t a_ref) { return world[a_ref].alive; }

		// WorldSync::TryKill, and the death event it brings: a loaded copy is killed with the console, a corpse is
		// seen dead, one not loaded waits.
		void TryKill(std::uint32_t a_ref)
		{
			auto& npc = world[a_ref];
			if (!npc.alive) {
				deaths.SawDead(a_ref);
				pending.erase(a_ref);
				return;
			}
			if (!npc.loaded) {
				pending.insert(a_ref);
				return;
			}
			pending.erase(a_ref);
			if (!npc.essential) {
				npc.alive = false;
				deaths.SawDead(a_ref);  // its death event
			}
		}

		// WorldSync::ApplyRemoteDeath.
		void Learn(std::uint32_t a_ref)
		{
			if (deaths.Add(a_ref, Clock::now())) {
				TryKill(a_ref);
			}
		}

		// What the server sent this game, applied as WorldSync and NpcSync do: a world state's dead (then it is
		// settled), each death as it is relayed, and the owners.
		Packets Pump()
		{
			auto packets = Received(f, peer);
			for (const auto& packet : packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kWorldState)) {
					const auto state = Protocol::DecodeWorldState(packet);
					REQUIRE(state);
					for (const auto ref : state->deadActors) {
						Learn(ref);
					}
					deaths.Settle();
				} else if (packet[0] == Type(MT::kActorDied)) {
					const auto death = Protocol::DecodeActorDeath(packet);
					REQUIRE(death);
					Learn(death->refId);
				} else if (packet[0] == Type(MT::kActorOwners)) {
					const auto batch = Protocol::DecodeOwners(packet);
					REQUIRE(batch);
					for (const auto& entry : *batch) {
						claims.Answered(entry.refId, entry.playerId, id, Clock::now());
						if (entry.playerId == 0) {
							owners.erase(entry.refId);
						} else {
							owners[entry.refId] = entry.playerId;
						}
					}
				}
			}
			return packets;
		}

		// Its game loads the NPC (alive or not) at a_now: WorldSync::TrackLoads, then the pending kills it retries.
		// True when the ledger takes it for a respawn.
		bool Loads(std::uint32_t a_ref, bool a_alive, Clock::time_point a_now)
		{
			auto& npc = world[a_ref];
			npc.loaded = true;
			npc.alive = a_alive;
			const bool back = deaths.OnLoaded(a_ref, a_alive, pending.contains(a_ref), a_now);
			if (!back && !a_alive) {
				deaths.SawDead(a_ref);  // its corpse
			}
			if (pending.contains(a_ref)) {
				TryKill(a_ref);
			}
			return back;
		}

		void Unloads(std::uint32_t a_ref) { world[a_ref].loaded = false; }

		// NpcSync's scan of one NPC at a_now: a loaded, living one is offered when NpcClaims::Book::Offer says so (with
		// WorldSync::MayClaim from the ledger). True if a claim was sent.
		bool Scan(std::uint32_t a_ref, Clock::time_point a_now)
		{
			const auto& npc = world[a_ref];
			if (!npc.loaded || !npc.alive) {
				return false;
			}
			const auto owner = owners.contains(a_ref) ? owners[a_ref] : 0;
			const auto reason = claims.Offer(a_ref, owner, id, false, deaths.MayClaim(a_ref, a_now), a_now);
			if (!reason) {
				return false;
			}
			f.fake->Deliver(peer, Claim(a_ref, *reason));
			return true;
		}

		// Its player kills the NPC: its own death event (WorldSync::Frame) and the report.
		void Kills(std::uint32_t a_ref, float a_x = 0.0f)
		{
			world[a_ref].alive = false;
			deaths.Add(a_ref, Clock::now());
			deaths.SawDead(a_ref);
			f.fake->Deliver(peer, Kill(a_ref, a_x));
		}

		// A save was loaded, with these NPCs loaded and alive in it: WorldSync::Reset, and the request for the
		// session's world (Session::OnGameLoaded).
		void LoadsASave(std::initializer_list<std::uint32_t> a_aliveHere)
		{
			deaths.Reset(Clock::now());
			pending.clear();
			world.clear();
			for (const auto ref : a_aliveHere) {
				world[ref] = { .loaded = true, .alive = true };
			}
			f.fake->Deliver(peer, Protocol::Encode(Protocol::WorldRequest{}));
		}
	};
}

TEST("alive again: a camp cleared together comes back after a cell reset in the host's game, for everyone")
{
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", true };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	host.Loads(RAIDER, true, t0);
	bob.Loads(RAIDER, true, t0);

	// Bob's game runs the raider and he kills it; the host's game hears of it and kills its copy.
	REQUIRE(bob.Scan(RAIDER, t0));
	bob.Kills(RAIDER, 40.0f);
	host.Pump();
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Alive(RAIDER));
	CHECK(!host.Scan(RAIDER, Clock::now()));  // its corpse is nobody's to run
	bob.Pump();

	// Days later the camp's cell resets in the host's game: the raider loads alive.
	std::this_thread::sleep_for(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
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

	// And it can die again: Bob's cell reset too, he fights it (his takeover is refused, the host's game just took
	// it) and kills it.
	bob.Unloads(RAIDER);
	CHECK(bob.Loads(RAIDER, true, later));
	f.fake->Deliver(2, Claim(RAIDER, Reason::kInteract));
	bob.Kills(RAIDER);
	const auto toHost = host.Pump();
	CHECK(Count(toHost, MT::kActorDied) == 1);
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Alive(RAIDER));  // the host's game kills its copy again
	CHECK(HasOwner(Owners(toHost), RAIDER, 0));
	carol.Pump();
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: a newcomer is told who runs a raider that came back instead of finding it dead")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	host.Loads(RAIDER, true, Clock::now());
	host.Kills(RAIDER, 10.0f);  // the host kills it alone
	host.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
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
	bob.Loads(RAIDER, true, Clock::now());
	host.Kills(RAIDER);
	bob.Pump();
	CHECK(!bob.Alive(RAIDER));
	host.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);

	// Bob's camp reset first. His own game knows its raider is back and offers it; the server says nothing.
	const auto later = Clock::now() + DAYS_LATER;
	bob.Unloads(RAIDER);
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
	host.Loads(RAIDER, true, Clock::now());
	bob.Kills(RAIDER);
	host.Pump();
	bob.Pump();
	std::this_thread::sleep_for(GRACE + 50ms);

	// The host loads an older save: its raider is alive there. Its game knows nothing until the session's world
	// state is in, so it offers nothing...
	host.LoadsASave({ RAIDER });
	CHECK(!host.Scan(RAIDER, Clock::now()));
	// ...and a game that offered it at once anyway (an older build, or a race) is not believed: it only just asked.
	f.fake->Deliver(1, Claim(RAIDER));
	const auto toHost = host.Pump();  // the world state, which settles it: the raider is dead after all
	CHECK(Owners(toHost).empty());
	CHECK(std::ranges::contains(WorldDead(toHost), RAIDER));
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Alive(RAIDER));  // its game kills it
	CHECK(!host.Scan(RAIDER, Clock::now() + DAYS_LATER));
	CHECK(Owners(bob.Pump()).empty());

	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: a raider essential only in the host's game, which the kill command leaves standing, doesn't undo Bob's kill when it loads again")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.world[RAIDER] = { .loaded = true, .alive = true, .essential = true };
	bob.Kills(RAIDER);
	host.Pump();
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(host.Alive(RAIDER));  // it went down and got up again
	CHECK(host.pending.empty());  // the kill ran

	// The host walks off and comes back: it loads alive, as it has been all along.
	std::this_thread::sleep_for(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
	CHECK(!host.Loads(RAIDER, true, later));
	CHECK(!host.Scan(RAIDER, later));
	CHECK(!f.Logged("alive again"));
	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: a raider the host's game heard of while its cell was unloaded is killed when it loads, not taken for a respawn")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	bob.Kills(RAIDER);
	host.Pump();
	CHECK(host.pending.contains(RAIDER));
	std::this_thread::sleep_for(GRACE + 50ms);

	// Days later the host walks into the camp: the raider loads alive, its death still waiting.
	const auto later = Clock::now() + DAYS_LATER;
	CHECK(!host.Loads(RAIDER, true, later));
	CHECK(!host.Alive(RAIDER));
	CHECK(!host.Scan(RAIDER, later));
	CHECK(Owners(host.Pump()).empty());
	CHECK(!f.Logged("alive again"));

	// When that camp does reset, later still, it comes back.
	host.Unloads(RAIDER);
	REQUIRE(host.Loads(RAIDER, true, later + DAYS_LATER));
	REQUIRE(host.Scan(RAIDER, later + DAYS_LATER));
	CHECK(HasOwner(Owners(bob.Pump()), RAIDER, host.id));
}

TEST("alive again: a kill the host's game has not heard of yet is not undone by its claim")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.Loads(RAIDER2, true, Clock::now());
	std::this_thread::sleep_for(GRACE + 50ms);  // long past the host's hello

	// Bob kills a raider nobody runs; the host's game, which has it alive and unowned, offers to run it before the
	// news arrives (its claim crossed the death on the wire).
	bob.Kills(RAIDER2);
	REQUIRE(host.Scan(RAIDER2, Clock::now()));
	const auto toHost = host.Pump();
	CHECK(Count(toHost, MT::kActorDied) == 1);
	CHECK(Owners(toHost).empty());
	CHECK(!host.Alive(RAIDER2));
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
		host.Loads(RAIDER, true, Clock::now());
		host.Loads(RAIDER2, true, Clock::now());
		host.Kills(RAIDER);
		host.Kills(RAIDER2);
		host.Pump();
		std::this_thread::sleep_for(GRACE + 50ms);
		const auto later = Clock::now() + DAYS_LATER;
		host.Unloads(RAIDER);
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

		// The other raider's corpse is still there in the host's game; its cell resets later. A death from the saved
		// session is old enough at once, the host's hello is what is waited for.
		host.Loads(RAIDER2, false, Clock::now());
		f.fake->Deliver(1, Claim(RAIDER2));
		CHECK(Owners(host.Pump()).empty());
		std::this_thread::sleep_for(GRACE + 50ms);
		const auto later = Clock::now() + DAYS_LATER;
		host.Unloads(RAIDER2);
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

TEST("session deaths: only a death seen here, learned of a while ago, and an actor loading alive mean it came back")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.Settle();
	deaths.Add(RAIDER, t0);
	CHECK(!deaths.OnLoaded(RAIDER, true, false, t0 + DAYS_LATER));  // never seen dead here: the kill didn't take
	deaths.SawDead(RAIDER);
	CHECK(!deaths.OnLoaded(RAIDER, false, false, t0 + DAYS_LATER));                      // a corpse loading
	CHECK(!deaths.OnLoaded(RAIDER, true, true, t0 + DAYS_LATER));                        // not killed here yet
	CHECK(!deaths.OnLoaded(RAIDER, true, false, t0 + SessionDeaths::RESPAWN_AFTER / 2));  // our kill still landing
	CHECK(!deaths.MayClaim(RAIDER, t0 + DAYS_LATER));
	CHECK(deaths.OnLoaded(RAIDER, true, false, t0 + DAYS_LATER));
	CHECK(deaths.MayClaim(RAIDER, t0 + DAYS_LATER));
	CHECK(deaths.Revived() == 1);
	CHECK(!deaths.OnLoaded(RAIDER2, true, false, t0 + DAYS_LATER));  // never dead here: nothing came back
	deaths.SawDead(RAIDER2);                                          // nor does seeing one we don't know of add it
	CHECK(!deaths.Contains(RAIDER2));
}
