// Deaths that end: a raider killed in the session is dead for everyone, until its cell resets in the host's game
// and the host's game, which knows the session's deaths (SessionDeaths), offers to run it again. The tests play the
// players against a real server and feed what it sends into each game's own ledger and claim book, as WorldSync and
// NpcSync do; each game's NPCs (loaded or not, alive or not) are modelled, since the engine decides those.

#include "ServerFixture.h"

#include "SessionFile.h"
#include "game/NpcClaims.h"
#include "game/SessionDeaths.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
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
	constexpr auto          GRACE = 300s;   // Options::aliveAgainGrace in these tests (also the wait before a first reminder): long, so a busy PC can't end it early; a test ends it with Fixture::Advance
	constexpr auto          REMIND = 150s;  // Options::remindDeadEvery in these tests, crossed with Fixture::Advance too
	// What a cell reset takes: days of game time. Only the games' ledgers are told it passed.
	constexpr auto DAYS_LATER = std::chrono::hours(1);

	Server::Options HostedOptions()
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		options.aliveAgainGrace = GRACE;
		options.remindDeadEvery = REMIND;
		return options;
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

	std::vector<std::uint8_t> Claim(std::uint32_t a_ref, Reason a_reason = Reason::kUnowned, bool a_essential = false)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason, a_essential } });
	}

	std::vector<std::uint8_t> Kill(std::uint32_t a_ref, float a_x = 0.0f)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true, a_x, 0.0f, 0.0f }, MT::kReportDeath);
	}

	// An NPC as one game has it. Essential: the game won't let it die (in this game only, when its quest is in another
	// state there), so its death is held rather than applied (WorldSync::TryKill). Following: our companion.
	struct Npc
	{
		bool loaded = false;
		bool alive = true;
		bool essential = false;
		bool following = false;
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
		std::unordered_set<std::uint32_t>                pending;  // deaths waiting for the NPC to load, or held while it is essential (WorldSync's)

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
		// seen dead, one not loaded waits, and one essential here is held (the kill would only knock it down).
		void TryKill(std::uint32_t a_ref)
		{
			auto& npc = world[a_ref];
			if (!npc.alive) {
				deaths.SawDead(a_ref);
				pending.erase(a_ref);
				return;
			}
			if (!npc.loaded || npc.essential) {
				pending.insert(a_ref);
				return;
			}
			pending.erase(a_ref);
			npc.alive = false;
			deaths.SawDead(a_ref);  // its death event
		}

		// WorldSync::ApplyRemoteDeath.
		void Learn(std::uint32_t a_ref)
		{
			if (deaths.Add(a_ref, Clock::now())) {
				TryKill(a_ref);
			}
		}

		// What the server sent this game, applied as WorldSync and NpcSync do: a world state's dead (then it is
		// settled), each death as it is relayed, and the owners (at a_now on the game's clock: days later, for a reset).
		Packets Pump(Clock::time_point a_now = Clock::now())
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
						// WorldSync::OnActorOwners: the server gives nobody a dead NPC, so one we have dead is alive for the
						// session: a death held here was undone, and a corpse here lies until its cell resets.
						if (entry.playerId != 0 && deaths.OnOwned(entry.refId, a_now)) {
							pending.erase(entry.refId);
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
		// WorldSync::MayClaim from the ledger), marked essential when it is here. True if a claim was sent.
		bool Scan(std::uint32_t a_ref, Clock::time_point a_now)
		{
			const auto& npc = world[a_ref];
			if (!npc.loaded || !npc.alive) {
				return false;
			}
			const auto owner = owners.contains(a_ref) ? owners[a_ref] : 0;
			const auto reason = claims.Offer(a_ref, owner, id, npc.following, deaths.MayClaim(a_ref, a_now, npc.essential), a_now);
			if (!reason) {
				return false;
			}
			f.fake->Deliver(peer, Claim(a_ref, *reason, npc.essential));
			return true;
		}

		// Its player kills the NPC: its own death event (WorldSync::Frame) and the report.
		void Kills(std::uint32_t a_ref, float a_x = 0.0f)
		{
			world[a_ref].alive = false;
			deaths.Add(a_ref, Clock::now());
			deaths.SawDead(a_ref);
			deaths.Reported(a_ref);
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
	f.Advance(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
	CHECK(host.Loads(RAIDER, true, later));
	REQUIRE(host.Scan(RAIDER, later));
	CHECK(HasOwner(Owners(bob.Pump(later)), RAIDER, host.id));
	CHECK(HasOwner(Owners(host.Pump(later)), RAIDER, host.id));
	CHECK(f.Logged(std::format("{:08X} is alive again in the host's world", RAIDER)));

	// It fights again: its hits on Bob's stand-in reach Bob.
	f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerHit{ bob.id, 12.0f, false, RAIDER }, MT::kPlayerHit));
	CHECK(Count(bob.Pump(), MT::kPlayerDamaged) == 1);

	// Someone joining now is not told it is dead.
	Game carol{ f, 3, "Carol" };
	CHECK(!carol.deaths.Contains(RAIDER));
	CHECK(carol.deaths.Settled(Clock::now()));

	// Bob's game no longer has it dead either: the host's game runs it (SessionDeaths::OnOwned). Before, his copy's
	// corpse kept it dead for him, and the raider could neither hurt him nor be hurt by him.
	CHECK(!bob.deaths.Contains(RAIDER));
	CHECK(bob.deaths.Corrected() == 1);
	// And it can die again: Bob's cell reset too (nothing to revive in his ledger), he fights it (his takeover is
	// refused, the host's game just took it) and kills it.
	bob.Unloads(RAIDER);
	CHECK(!bob.Loads(RAIDER, true, later));
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
	f.Advance(GRACE + 50ms);
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
	f.Advance(GRACE + 50ms);

	// Bob's camp reset first. His own game knows its raider is back and offers it; the server grants nothing, and says
	// nothing yet (the host's game may be about to offer it too, which brings it back for everyone).
	const auto later = Clock::now() + DAYS_LATER;
	bob.Unloads(RAIDER);
	CHECK(bob.Loads(RAIDER, true, later));
	REQUIRE(bob.Scan(RAIDER, later));
	f.fake->Deliver(2, Claim(RAIDER, Reason::kCompanion));
	f.fake->Deliver(2, Claim(RAIDER, Reason::kInteract));
	auto toBob = bob.Pump();
	CHECK(Owners(toBob).empty());
	CHECK(Count(toBob, MT::kActorDied) == 0);
	CHECK(bob.Alive(RAIDER));
	CHECK(Owners(host.Pump()).empty());
	// His hits on it (as its health, since nobody runs it) are dropped and logged. Once his game has gone on about it
	// for aliveAgainGrace with the raider still dead, it is told the death again, so it kills the raider instead of
	// fighting it alone for good.
	// (The server answers at most 10 heartbeats a second, and each Pump sends one: the claims below go without one.)
	f.fake->Deliver(2, Protocol::Encode(Protocol::ActorHealth{ RAIDER, 0.5f }, MT::kReportHealth));
	CHECK(Count(host.Pump(), MT::kActorHealth) == 0);
	CHECK(f.Logged(std::format("health of {:08X} from player {} dropped: the session has it dead", RAIDER, bob.id)));
	f.Advance(200s);
	f.fake->Deliver(2, Claim(RAIDER));  // 200 s in: still nothing
	f.fake->Settle();
	f.Advance(200s);
	f.fake->Deliver(2, Claim(RAIDER));  // 400 s in: told
	toBob = bob.Pump();
	CHECK(Count(toBob, MT::kActorDied) == 1);
	CHECK(bob.deaths.Contains(RAIDER));
	CHECK(!bob.Alive(RAIDER));
	// No second reminder so soon, then one per remindDeadEvery while his game keeps reporting about it.
	f.fake->Deliver(2, Claim(RAIDER));
	f.fake->Settle();
	f.Advance(REMIND + 50ms);
	f.fake->Deliver(2, Claim(RAIDER));
	CHECK(Count(bob.Pump(), MT::kActorDied) == 1);
	// Nor do the host's game's takeover or companion claims (a hit or a press can be on a corpse), and the host's game is
	// never told again: its own offer is what brings one back.
	f.fake->Deliver(1, Claim(RAIDER, Reason::kInteract));
	f.fake->Deliver(1, Claim(RAIDER, Reason::kCompanion));
	const auto toHost = host.Pump();
	CHECK(Owners(toHost).empty());
	CHECK(Count(toHost, MT::kActorDied) == 0);

	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: both camps reset together, the joiner's offer first: the host's offer brings the raider back, and the joiner's copy isn't killed")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.Loads(RAIDER, true, Clock::now());
	bob.Loads(RAIDER, true, Clock::now());
	host.Kills(RAIDER);
	bob.Pump();
	host.Pump();
	CHECK(!bob.Alive(RAIDER));
	f.Advance(GRACE + 50ms);

	// The players walk back into the camp together, days later: both games find the raider alive again and offer it in
	// the same moment, Bob's a packet ahead. The host's offer brings it back for everyone; Bob's game is not told the
	// death again meanwhile (that would have killed his copy just before it came back: a corpse for him, hitting him
	// unseen, for the rest of the session).
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
	bob.Unloads(RAIDER);
	CHECK(bob.Loads(RAIDER, true, later));
	CHECK(host.Loads(RAIDER, true, later));
	REQUIRE(bob.Scan(RAIDER, later));
	REQUIRE(host.Scan(RAIDER, later));
	const auto toBob = bob.Pump(later);
	CHECK(Count(toBob, MT::kActorDied) == 0);
	CHECK(HasOwner(Owners(toBob), RAIDER, host.id));
	CHECK(HasOwner(Owners(host.Pump(later)), RAIDER, host.id));
	CHECK(f.Logged(std::format("{:08X} is alive again in the host's world", RAIDER)));
	CHECK(bob.Alive(RAIDER));
	CHECK(!bob.deaths.Contains(RAIDER));
	// Nor later: the raider lives for the session, and its reports go through.
	f.Advance(GRACE + 50ms);
	f.fake->Deliver(2, Claim(RAIDER, Reason::kInteract));
	f.fake->Deliver(2, Protocol::Encode(Protocol::ActorHealth{ RAIDER, 0.5f }, MT::kReportDamage));
	CHECK(Count(bob.Pump(later), MT::kActorDied) == 0);
	CHECK(Count(host.Pump(later), MT::kActorDamaged) == 1);
}

TEST("alive again: a joiner whose copy lies dead no longer has the raider dead once the host's game runs it, and takes its hits")
{
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", true };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	host.Loads(RAIDER, true, t0);
	bob.Loads(RAIDER, true, t0);
	host.Kills(RAIDER, 40.0f);
	bob.Pump();
	CHECK(!bob.Alive(RAIDER));
	CHECK(bob.deaths.Contains(RAIDER));

	// The host's camp resets; Bob's hasn't: his copy is a corpse. Once the host's game runs the raider, Bob's game has it
	// alive for the session: the corpse lies until his own cell resets, but the raider's hits on his stand-in in the
	// host's game reach him (PlayerHits judges those by the host's distance, not the corpse), and a death of it is news.
	f.Advance(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
	REQUIRE(host.Loads(RAIDER, true, later));
	REQUIRE(host.Scan(RAIDER, later));
	CHECK(HasOwner(Owners(bob.Pump(later)), RAIDER, host.id));
	CHECK(!bob.deaths.Contains(RAIDER));
	CHECK(!bob.Alive(RAIDER));  // the corpse stays
	CHECK(bob.deaths.Corrected() == 1);
	f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerHit{ bob.id, 12.0f, false, RAIDER }, MT::kPlayerHit));
	CHECK(Count(bob.Pump(), MT::kPlayerDamaged) == 1);
	host.Kills(RAIDER);
	bob.Pump();
	CHECK(bob.deaths.Contains(RAIDER));  // news again
}

TEST("session deaths: a game that runs the NPC means it lives, unless our own report of its death just went out")
{
	SessionDeaths::Ledger ledger;
	const auto            t0 = Clock::now();
	CHECK(!ledger.OnOwned(RAIDER, t0));  // nothing known

	// Learned from a friend's report: forgotten as soon as anyone runs it.
	ledger.Add(RAIDER, t0, 2);
	ledger.SawDead(RAIDER);
	CHECK(ledger.OnOwned(RAIDER, t0 + 100ms));
	CHECK(!ledger.Contains(RAIDER));
	CHECK(ledger.Corrected() == 1);

	// Our own death event, reported: a grant to another game within OWNED_CROSSING crossed our report on the wire (the
	// server ends the ownership on it), later than that it is a revival.
	ledger.Add(RAIDER2, t0);
	ledger.Reported(RAIDER2);
	CHECK(!ledger.OnOwned(RAIDER2, t0 + SessionDeaths::OWNED_CROSSING - 1ms));
	CHECK(ledger.Contains(RAIDER2));
	CHECK(ledger.OnOwned(RAIDER2, t0 + SessionDeaths::OWNED_CROSSING));
	CHECK(!ledger.Contains(RAIDER2));
	CHECK(ledger.Corrected() == 2);

	// How long a death has been known (a kill told again is retried only after KILL_RETRY_AFTER).
	ledger.Add(RAIDER, t0);
	CHECK(!ledger.KnownFor(RAIDER, SessionDeaths::KILL_RETRY_AFTER, t0 + 1s));
	CHECK(ledger.KnownFor(RAIDER, SessionDeaths::KILL_RETRY_AFTER, t0 + SessionDeaths::KILL_RETRY_AFTER));
	CHECK(!ledger.KnownFor(RAIDER2, 0s, t0));
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
	f.Advance(GRACE + 50ms);

	// The host loads an older save: its raider is alive there. Its game knows nothing until the session's world
	// state is in, so it offers nothing...
	host.LoadsASave({ RAIDER });
	CHECK(!host.Scan(RAIDER, Clock::now()));
	// ...and a game that offered it at once anyway (an older build, or a race) is not believed: it only just asked.
	f.fake->Deliver(1, Claim(RAIDER));
	const auto toHost = host.Pump();  // the world state, which settles it: the raider is dead after all
	CHECK(Owners(toHost).empty());
	CHECK(Count(toHost, MT::kActorDied) == 0);  // the host's game is never told a death again: its offer is the revival
	CHECK(std::ranges::contains(WorldDead(toHost), RAIDER));
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Alive(RAIDER));  // its game kills it
	CHECK(!host.Scan(RAIDER, Clock::now() + DAYS_LATER));
	CHECK(Owners(bob.Pump()).empty());

	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));
}

TEST("alive again: the host quickloading so often that the server refuses its world request doesn't undo a friend's kills")
{
	auto options = HostedOptions();
	options.worldStateRefill = 3600s;  // owed until the test moves the clock past it
	Fixture f{ options };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.Loads(RAIDER, true, Clock::now());
	bob.Kills(RAIDER);  // Bob clears the camp
	host.Pump();
	bob.Pump();
	CHECK(!host.Alive(RAIDER));
	f.Advance(GRACE + 50ms);

	// The host is in a hard fight and quickloads a save from before the kill, again and again. Its budget has three
	// world states and its hello took one: the server answers two requests in a row and owes the third until the
	// budget refills, so meanwhile its game doesn't hear the raider is dead.
	for (int load = 0; load < 2; ++load) {
		host.LoadsASave({ RAIDER });
		const auto toHost = host.Pump();
		REQUIRE(Count(toHost, MT::kWorldState) >= 1);
		CHECK(!host.Alive(RAIDER));
	}
	host.LoadsASave({ RAIDER });
	CHECK(Count(host.Pump(), MT::kWorldState) == 0);
	CHECK(f.Logged("has asked for the world too often"));
	CHECK(host.Alive(RAIDER));

	// Its game gives up waiting and offers the raider it has alive, well after the request (on the server's clock).
	f.Advance(GRACE + 50ms);
	const auto afterFallback = Clock::now() + SessionDeaths::SETTLE_FALLBACK;
	REQUIRE(host.Scan(RAIDER, afterFallback));
	CHECK(Owners(host.Pump()).empty());
	CHECK(Owners(bob.Pump()).empty());
	CHECK(!f.Logged("alive again"));

	// The kill stands: a newcomer finds the raider dead.
	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));

	// And once the host's budget refills, the world state it was owed comes: its game kills the raider too.
	f.Advance(3600s);
	bool owedCame = false;
	for (const auto deadline = std::chrono::steady_clock::now() + 30s; !owedCame && std::chrono::steady_clock::now() < deadline;) {
		owedCame = Count(host.Pump(), MT::kWorldState) >= 1;
		if (!owedCame) {
			std::this_thread::sleep_for(200ms);  // one heartbeat each time, fewer than the server answers (ten a second)
		}
	}
	CHECK(owedCame);
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(!host.Alive(RAIDER));
	CHECK(!f.Logged("alive again"));
}

TEST("alive again: a raider essential only in the host's game doesn't undo Bob's kill: its death waits there, and its claims change nothing")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	host.world[RAIDER] = { .loaded = true, .alive = true, .essential = true };
	bob.Kills(RAIDER);
	host.Pump();
	CHECK(host.deaths.Contains(RAIDER));
	CHECK(host.Alive(RAIDER));             // the kill command would only knock it down
	CHECK(host.pending.contains(RAIDER));  // so its death is held

	// The host walks off and comes back: it loads alive, as it has been all along. Its game offers to run it, marked
	// essential; a death seen this session is no mistake, so nobody gets it and it stays dead.
	f.Advance(GRACE + 50ms);
	const auto later = Clock::now() + DAYS_LATER;
	host.Unloads(RAIDER);
	CHECK(!host.Loads(RAIDER, true, later));
	CHECK(host.Scan(RAIDER, later));
	CHECK(Owners(host.Pump()).empty());
	CHECK(Owners(bob.Pump()).empty());
	CHECK(!f.Logged("alive again"));
	CHECK(!f.Logged("no longer dead"));
	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(RAIDER));

	// Its quest moves on in the host's game and the flag goes: it dies there too.
	host.world[RAIDER].essential = false;
	host.TryKill(RAIDER);
	CHECK(!host.Alive(RAIDER));
	CHECK(host.pending.empty());
}

TEST("alive again: a raider the host's game heard of while its cell was unloaded is killed when it loads, not taken for a respawn")
{
	Fixture f{ HostedOptions() };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	bob.Kills(RAIDER);
	host.Pump();
	CHECK(host.pending.contains(RAIDER));
	f.Advance(GRACE + 50ms);

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
	f.Advance(GRACE + 50ms);  // long past the host's hello

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
	const auto path = Test::TempPath("f4mp-alive-again", ".session").string();
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
		f.Advance(GRACE + 50ms);
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
		f.Advance(GRACE + 50ms);
		const auto later = Clock::now() + DAYS_LATER;
		host.Unloads(RAIDER2);
		REQUIRE(host.Loads(RAIDER2, true, later));
		REQUIRE(host.Scan(RAIDER2, later));
		CHECK(HasOwner(Owners(bob.Pump()), RAIDER2, host.id));
	}
	std::filesystem::remove(path);
}

// --- essential NPCs taken for dead ------------------------------------------------------------
// Builds before session layout 29 took an essential NPC going down for dead: Dogmeat was saved dead, killed in the
// friends' games, and run by nobody, while the host's game kept him alive (essential). Only such a death, one an older
// build saved, can be undone, and only by the host's game offering to run the NPC it has alive and essential.

namespace
{
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t PIPER = 0x00002F1E;  // another essential companion, elsewhere when the host first plays again

	// A session file as a build before layout 29 wrote it: no list of deaths that may be mistakes after the identities.
	void SaveLegacy(const std::filesystem::path& a_path, std::vector<std::uint32_t> a_dead)
	{
		SessionFile::Snapshot snapshot;
		snapshot.contentHash = 0x1234;  // Fixture::MakeHello's
		snapshot.sessionId = 0x5E55;
		snapshot.identities = { { HOST_IDENTITY, 1 } };
		snapshot.deadActors = std::move(a_dead);
		auto                  bytes = SessionFile::Encode(snapshot);
		constexpr std::size_t HEALABLE_AT = 4 + 2 + 4 + 8 + 4 + 2 + 12;  // after the header and the one identity
		bytes.erase(bytes.begin() + HEALABLE_AT, bytes.begin() + HEALABLE_AT + 4);
		const std::uint16_t layout = 26;  // what rc3 wrote
		std::memcpy(bytes.data() + 4, &layout, sizeof(layout));
		std::ofstream out{ a_path, std::ios::binary | std::ios::trunc };
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	Server::Options SavedOptions(const TempSessionFile& a_file)
	{
		auto options = HostedOptions();
		options.sessionFile = a_file.path.string();
		return options;
	}

	// An essential companion as the host's game has them: alive and following, essential by their companion alias.
	constexpr Npc HOSTS_COMPANION{ .loaded = true, .alive = true, .essential = true, .following = true };
}

TEST("essential: Dogmeat, saved dead by an older build, comes back for everyone once the host's game offers to run him")
{
	const TempSessionFile file;
	SaveLegacy(file.path, { DOGMEAT, PIPER, RAIDER });
	{
		Fixture f{ SavedOptions(file) };
		Game    host{ f, 1, "Host", true };
		Game    bob{ f, 2, "Bob" };
		CHECK(host.deaths.Contains(DOGMEAT));
		CHECK(bob.pending.contains(DOGMEAT));  // not loaded in Bob's game yet: his death waits there

		// The host's game has Dogmeat at its side, alive: his death is held there (the kill would only knock him down),
		// and its game offers to run him, marked essential.
		host.world[DOGMEAT] = HOSTS_COMPANION;
		host.TryKill(DOGMEAT);
		CHECK(host.Alive(DOGMEAT));
		CHECK(host.pending.contains(DOGMEAT));
		REQUIRE(host.Scan(DOGMEAT, Clock::now()));
		CHECK(HasOwner(Owners(host.Pump()), DOGMEAT, host.id));
		CHECK(f.Logged(std::format("{:08X}, dead in the saved session, is alive in the host's world", DOGMEAT)));
		CHECK(!host.pending.contains(DOGMEAT));
		CHECK(!host.deaths.Contains(DOGMEAT));

		// Bob's game hears who runs him: the death it held is forgotten, and Dogmeat walks in alive.
		CHECK(HasOwner(Owners(bob.Pump()), DOGMEAT, host.id));
		CHECK(!bob.deaths.Contains(DOGMEAT));
		CHECK(!bob.Loads(DOGMEAT, true, Clock::now()));
		CHECK(bob.Alive(DOGMEAT));

		// Someone joining now isn't told he is dead. The raider, no companion, stays dead, and so does Piper for now.
		Game carol{ f, 3, "Carol" };
		CHECK(!carol.deaths.Contains(DOGMEAT));
		CHECK(carol.deaths.Contains(RAIDER));
		CHECK(carol.deaths.Contains(PIPER));
	}  // stopping saves, in the new layout

	// Piper's death came from the older build too and may still be a mistake, saved as such; Dogmeat's is gone.
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(!std::ranges::contains(saved->deadActors, DOGMEAT));
	CHECK(std::ranges::contains(saved->healable, PIPER));
	CHECK(std::ranges::contains(saved->healable, RAIDER));
	CHECK(!std::ranges::contains(saved->healable, DOGMEAT));

	// Another evening: the host meets Piper, alive and essential in its game. Undone across the restart too.
	Fixture f{ SavedOptions(file) };
	Game    host{ f, 1, "Host", true };
	CHECK(!host.deaths.Contains(DOGMEAT));
	host.world[PIPER] = HOSTS_COMPANION;
	host.TryKill(PIPER);
	REQUIRE(host.Scan(PIPER, Clock::now()));
	CHECK(HasOwner(Owners(host.Pump()), PIPER, host.id));
}

TEST("essential: a real death of an NPC the host's game still keeps essential stays, through any number of server restarts")
{
	const TempSessionFile file;
	for (int run = 0; run < 3; ++run) {
		Fixture f{ SavedOptions(file) };
		Game    host{ f, 1, "Host", true };
		Game    bob{ f, 2, "Bob" };
		if (run == 0) {
			// A quest's script kills Piper in Bob's game; the host's game, whose quest isn't there yet, keeps her essential.
			bob.Kills(PIPER);
			host.Pump();
		}
		CHECK(host.deaths.Contains(PIPER));
		host.world[PIPER] = HOSTS_COMPANION;
		host.TryKill(PIPER);
		CHECK(host.pending.contains(PIPER));  // held, not undone

		// Its game offers to run her, marked essential, long after the death and the host's hello: nobody gets her.
		f.Advance(GRACE + 50ms);
		REQUIRE(host.Scan(PIPER, Clock::now()));
		CHECK(Owners(host.Pump()).empty());
		CHECK(Owners(bob.Pump()).empty());
		CHECK(!f.Logged("no longer dead"));
		CHECK(bob.deaths.Contains(PIPER));
	}
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(std::ranges::contains(saved->deadActors, PIPER));
	CHECK(saved->healable.empty());
}

TEST("essential: an older build's death that a game reports again is no longer undone")
{
	const TempSessionFile file;
	SaveLegacy(file.path, { PIPER });
	Fixture f{ SavedOptions(file) };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };
	// Bob's game saw her die (it loaded before the session's dead list came, say): no mistake.
	f.fake->Deliver(2, Kill(PIPER));
	bob.Pump();
	host.world[PIPER] = HOSTS_COMPANION;
	host.TryKill(PIPER);
	REQUIRE(host.Scan(PIPER, Clock::now()));
	CHECK(Owners(host.Pump()).empty());
	CHECK(!f.Logged("no longer dead"));
	Game carol{ f, 3, "Carol" };
	CHECK(carol.deaths.Contains(PIPER));
}

TEST("essential: only the host's game, offering to run him, undoes an older build's death; a friend's claim or the host's hit changes nothing")
{
	const TempSessionFile file;
	SaveLegacy(file.path, { DOGMEAT });
	Fixture f{ SavedOptions(file) };
	Game    host{ f, 1, "Host", true };
	Game    bob{ f, 2, "Bob" };

	// Bob's game has Dogmeat alive and essential too (he recruited him in his own save): its world isn't the session's.
	for (const auto reason : { Reason::kUnowned, Reason::kCompanion, Reason::kInteract }) {
		f.fake->Deliver(2, Claim(DOGMEAT, reason, true));
	}
	// The host's hit or press can be on a corpse; and an offer not marked essential is a cell reset's, which this isn't
	// (and a companion's never is).
	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kInteract, true));
	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kCompanion, false));
	CHECK(Owners(host.Pump()).empty());
	CHECK(Owners(bob.Pump()).empty());
	CHECK(host.deaths.Contains(DOGMEAT));
	CHECK(!f.Logged("no longer dead"));

	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kUnowned, true));
	CHECK(HasOwner(Owners(bob.Pump()), DOGMEAT, host.id));
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

TEST("session deaths: one that only went down gets up again and is no death; an essential one a script kills is")
{
	using namespace SessionDeaths;
	for (const auto down : { LIFE_ESSENTIAL_DOWN, LIFE_BLEEDOUT, LIFE_UNCONSCIOUS, LIFE_RESTRAINED }) {
		CHECK(GetsUp(down, true));
		CHECK(GetsUp(down, false));  // a protected one hit by anyone but the player bleeds out too
	}
	CHECK(GetsUp(0, true));  // the game called it dead, but it is standing: essential
	CHECK(!GetsUp(LIFE_DYING, true));
	CHECK(!GetsUp(LIFE_DEAD, true));
	CHECK(!GetsUp(LIFE_DYING, false));
	CHECK(!GetsUp(LIFE_DEAD, false));
	CHECK(!GetsUp(0, false));  // a death event for an ordinary NPC: dead, whatever its state reads yet
}

TEST("session deaths: one the session has dead may be offered only by a game that keeps it essential, once the dead list is in")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.Reset(t0);
	deaths.Add(RAIDER, t0);
	CHECK(!deaths.MayClaim(RAIDER, t0, true));  // nothing before the session's dead list
	deaths.Settle();
	CHECK(!deaths.MayClaim(RAIDER, t0));
	CHECK(deaths.MayClaim(RAIDER, t0, true));
	CHECK(deaths.MayClaim(RAIDER2, t0, true));
}

TEST("session deaths: a death heard from a friend's report takes where its corpse came to rest from that friend alone, soon after")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.Add(RAIDER, t0, 2);  // Bob's report
	deaths.Add(RAIDER2, t0);    // our own death event, or the session's list
	CHECK(deaths.RestsFrom(RAIDER, 2, t0 + 3s));
	CHECK(!deaths.RestsFrom(RAIDER, 3, t0 + 3s));  // Carol's report of it, later: not where Bob's corpse lies
	CHECK(!deaths.RestsFrom(RAIDER, 2, t0 + Protocol::CORPSE_REST_WINDOW));
	CHECK(!deaths.RestsFrom(RAIDER2, 2, t0 + 3s));
	CHECK(!deaths.RestsFrom(RAIDER2, 0, t0 + 3s));
	CHECK(!deaths.RestsFrom(0x00ABCDEF, 2, t0 + 3s));  // news: a death, not a rest
	CHECK(!deaths.Add(RAIDER, t0 + 3s, 3));  // a second report changes nothing
	CHECK(deaths.RestsFrom(RAIDER, 2, t0 + 3s));
}

TEST("session deaths: our report sent as an NPC began to fall is taken back once by its death event, to say where it came to rest")
{
	const auto            t0 = Clock::time_point{} + 1h;
	SessionDeaths::Ledger deaths;
	deaths.FellReported(RAIDER, { 1.0f, 2.0f, 3.0f });  // not known dead: nothing kept
	CHECK(!deaths.TakeFell(RAIDER));
	deaths.Add(RAIDER, t0);
	CHECK(!deaths.TakeFell(RAIDER));  // reported at its death event, not as it fell
	deaths.FellReported(RAIDER, { 1.0f, 2.0f, 3.0f });
	const auto fell = deaths.TakeFell(RAIDER);
	REQUIRE(fell);
	CHECK((*fell)[0] == 1.0f);
	CHECK((*fell)[2] == 3.0f);
	CHECK(!deaths.TakeFell(RAIDER));  // once
	deaths.FellReported(RAIDER, { 1.0f, 2.0f, 3.0f });
	deaths.Reset(t0 + 1s);
	CHECK(!deaths.TakeFell(RAIDER));
}
