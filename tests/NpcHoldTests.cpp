// When our game keeps running an NPC (NpcHold, as NpcSync::Scan, ShouldHandOff, ApplyOwners and OnLocalInteraction use
// it), and which kills the server believes across a change of hands (Server.cpp, handleDeath: the runner, a fighter, and
// the player whose game ran it moments ago). The tests play the games against a real server: each scan's claim or
// release goes to the server, and what the server answers goes back into that game, as NpcSync does.

#include "ServerFixture.h"

#include "game/NpcClaims.h"
#include "game/NpcHold.h"

#include <optional>
#include <thread>
#include <unordered_map>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Reason = Protocol::ClaimReason;
	using NpcHold::Clock;

	constexpr std::uint32_t GUARD = 0x00095E8A;  // a vault guard, as in the live fight this comes from
	constexpr std::uint32_t RAIDER = 0x0001F301;
	constexpr std::uint32_t NPC_SHOT = 0x0001F302;
	constexpr std::uint64_t HOST_IDENTITY = 0x1111;

	Server::Options HostedOptions()
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		return options;
	}

	enum class Here
	{
		kAlive,  // loaded and alive: a candidate for NpcSync::Scan
		kDying,  // loaded, dead or falling
		kGone,   // unloaded
	};

	// One player's game: who it thinks runs each NPC, its claims and its holds.
	struct Game
	{
		Fixture&                                         f;
		PeerId                                           peer;
		std::uint32_t                                    id = 0;
		NpcClaims::Book                                  claims;
		NpcHold::Book                                    hold;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;
		std::uint32_t                                    releases = 0;

		Game(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_identity = 0) :
			f(a_f),
			peer(a_peer)
		{
			if (a_identity == HOST_IDENTITY) {
				f.fake->SetLocal(a_peer);
			}
			id = JoinAs(f, a_peer, std::move(a_name), a_identity);
		}

		[[nodiscard]] std::uint32_t OwnerOf(std::uint32_t a_ref) const
		{
			const auto it = owners.find(a_ref);
			return it == owners.end() ? 0 : it->second;
		}

		// What the server sent, owners applied as NpcSync::ApplyOwners does; the deaths it relayed are returned.
		std::vector<Protocol::ActorDeath> Pump(Clock::time_point a_now)
		{
			std::vector<Protocol::ActorDeath> deaths;
			for (const auto& packet : Received(f, peer)) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kActorDied)) {
					const auto death = Protocol::DecodeActorDeath(packet);
					REQUIRE(death);
					deaths.push_back(*death);
					continue;
				}
				if (packet[0] != Type(MT::kActorOwners)) {
					continue;
				}
				const auto batch = Protocol::DecodeOwners(packet);
				REQUIRE(batch);
				for (const auto& entry : *batch) {
					claims.Answered(entry.refId, entry.playerId, id, a_now);
					if (entry.playerId != id) {
						hold.Forget(entry.refId);
					}
					if (entry.playerId == 0) {
						owners.erase(entry.refId);
					} else {
						owners[entry.refId] = entry.playerId;
					}
				}
			}
			return deaths;
		}

		// NpcSync::Scan for one NPC at a_now: a living one nobody runs is claimed; one we run that is no longer a
		// candidate is released unless NpcHold keeps it. a_holds false plays a game without the hold, which let one go
		// as it fell.
		void Scan(std::uint32_t a_ref, Here a_here, Clock::time_point a_now, bool a_holds = true)
		{
			bool sent = false;
			if (a_here == Here::kAlive) {
				if (const auto reason = claims.Offer(a_ref, OwnerOf(a_ref), id, false, true, a_now)) {
					f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, *reason } }));
					sent = true;
				}
			} else if (OwnerOf(a_ref) == id && !(a_holds && hold.Keep(a_ref, a_here == Here::kDying, a_now))) {
				hold.Forget(a_ref);
				owners.erase(a_ref);
				f.fake->Deliver(peer, Protocol::EncodeRelease({ a_ref }));
				++releases;
				sent = true;
			}
			claims.Expire(a_now);
			hold.Expire(a_now);
			if (sent) {
				Pump(a_now);  // (a heartbeat each: the server answers ten a second)
			}
		}

		// Our player hits the NPC (WorldSync::Frame): NpcSync::OnLocalInteraction, whose takeover claim goes for one
		// another game runs, then the health report if it lives.
		void Hits(std::uint32_t a_ref, std::optional<float> a_healthAfter, Clock::time_point a_now)
		{
			hold.Fought(a_ref, a_now);
			if (const auto owner = OwnerOf(a_ref); owner != 0 && owner != id) {
				claims.Claimed(a_ref, Reason::kInteract, a_now);
				f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Reason::kInteract } }));
			}
			if (a_healthAfter) {
				f.fake->Deliver(peer, Protocol::Encode(Protocol::ActorHealth{ a_ref, *a_healthAfter }, MT::kReportHealth));
			}
			Pump(a_now);
		}

		// The death event, seconds after the killing blow (WorldSync::ReportDeaths): a death seen happen.
		void SawDie(std::uint32_t a_ref, bool a_killed)
		{
			f.fake->Deliver(peer, Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, a_killed, 100.0f, 0.0f, 0.0f }, MT::kReportDeath));
		}

		// Our kill's death event (WorldSync::ReportDeaths): NpcSync::OnLocalInteraction first, whose takeover claim
		// leaves a frame before the death.
		void Kills(std::uint32_t a_ref, Clock::time_point a_now)
		{
			hold.Fought(a_ref, a_now);
			if (const auto owner = OwnerOf(a_ref); owner != 0 && owner != id) {
				f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Reason::kInteract } }));
			}
			SawDie(a_ref, true);
		}

		// The host's check of an NPC another game now runs that is dead here (WorldSync::CheckFoundDead).
		void FoundDead(std::uint32_t a_ref)
		{
			f.fake->Deliver(peer, Protocol::Encode(Protocol::ActorDeath{ .refId = a_ref, .foundDead = true }, MT::kReportDeath));
		}

		// WorldSync::CheckFoundDead for an NPC dead in our world (a_falling: still falling): true if it was reported
		// found dead, false if left to its death event.
		bool Check(std::uint32_t a_ref, bool a_falling, Clock::time_point a_now)
		{
			if (NpcHold::LeaveToEvent(waits, a_ref, a_falling, hold.FoughtLately(a_ref, a_now), a_now)) {
				return false;
			}
			waits.Forget(a_ref);
			FoundDead(a_ref);
			return true;
		}

		NpcHold::Book waits;  // WorldSync's: checks left to the death event
	};
}

TEST("npc hold: our player's fight keeps an NPC from being handed to a nearer friend, for 10 s after the last hit")
{
	NpcHold::Book book;
	const auto    t0 = Clock::now();
	CHECK(!book.FoughtLately(GUARD, t0));
	book.Fought(GUARD, t0);
	CHECK(book.FoughtLately(GUARD, t0 + 9s));
	CHECK(!book.FoughtLately(GUARD, t0 + NpcHold::FOUGHT));
	book.Fought(GUARD, t0 + 8s);  // each hit counts afresh
	CHECK(book.FoughtLately(GUARD, t0 + 17s));
	book.Expire(t0 + 30s);
	CHECK(!book.FoughtLately(GUARD, t0 + 17s));  // forgotten

	// Never more than MAX_FOUGHT remembered: a full book makes room from fights long over, else takes no more.
	for (std::uint32_t i = 0; i < NpcHold::MAX_FOUGHT; ++i) {
		book.Fought(0x00100000 + i, t0);
	}
	book.Fought(GUARD, t0 + 1s);
	CHECK(!book.FoughtLately(GUARD, t0 + 1s));
	book.Fought(GUARD, t0 + 11s);
	CHECK(book.FoughtLately(GUARD, t0 + 11s));
	CHECK(!book.FoughtLately(0x00100000, t0 + 11s));
}

TEST("npc hold: an NPC we run that falls is kept up to 15 s for its death report; one unloaded goes at once")
{
	NpcHold::Book book;
	const auto    t0 = Clock::now();
	CHECK(!book.Keep(GUARD, false, t0));  // unloaded, disabled or deleted
	CHECK(book.Keep(GUARD, true, t0));
	CHECK(book.Keep(GUARD, true, t0 + 14s));
	CHECK(book.Dying() == 1);
	CHECK(!book.Keep(GUARD, true, t0 + NpcHold::DYING));  // its death never got out: let go
	book.Forget(GUARD);
	CHECK(book.Dying() == 0);

	// Its time starts when it falls, and only while it is still there.
	CHECK(book.Keep(GUARD, true, t0 + 20s));
	CHECK(!book.Keep(GUARD, false, t0 + 21s));  // unloaded as it fell
	CHECK(book.Keep(GUARD, true, t0 + 40s));    // a new fall: a new 15 s
	CHECK(book.Keep(GUARD, true, t0 + 54s));
	book.Clear();
	CHECK(book.Dying() == 0);
}

TEST("npc hold: the host kills a guard its game runs while the friend fights it too: the guard dies in both games")
{
	// Live: the host's game ran the guard and the friend's copy took the host's hits down to 134 health. The host's
	// killing blow made its game let the guard go as it fell; the friend's game, where it still stood, took it over;
	// and the host's game, finding it dead in a guard the friend now ran, was not believed. Dead for the host, alive
	// with 134 health for the friend. Now the host's game keeps it while it falls, and its report ends its run.
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", HOST_IDENTITY };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	host.Scan(GUARD, Here::kAlive, t0);
	bob.Pump(t0);
	REQUIRE(host.OwnerOf(GUARD) == host.id);
	REQUIRE(bob.OwnerOf(GUARD) == host.id);

	host.Hits(GUARD, 0.9f, t0 + 1s);
	host.Hits(GUARD, 0.6f, t0 + 2s);
	host.Hits(GUARD, 0.36f, t0 + 3s);
	host.Hits(GUARD, std::nullopt, t0 + 4s);  // the killing blow: no health to report

	// It falls for a few seconds. The host's scans keep it; Bob's, where it stands, have nothing to claim.
	for (auto now = t0 + 4s; now < t0 + 7s; now += 250ms) {
		host.Scan(GUARD, Here::kDying, now);
		bob.Scan(GUARD, Here::kAlive, now);
	}
	CHECK(host.releases == 0);
	CHECK(bob.OwnerOf(GUARD) == host.id);
	CHECK(host.hold.Dying() == 1);

	host.SawDie(GUARD, true);
	const auto deaths = bob.Pump(t0 + 7s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == GUARD);
	CHECK(deaths[0].playerId == host.id);
	CHECK(deaths[0].killed);
	CHECK(bob.OwnerOf(GUARD) == 0);  // nobody runs a corpse
	host.Pump(t0 + 7s);
	CHECK(host.OwnerOf(GUARD) == 0);
	CHECK(host.hold.Dying() == 0);
	CHECK(!f.Logged("dropped"));
}

TEST("npc hold: a guard let go as it fell is still the host's kill once its death event comes, not its found-dead check")
{
	// A game without the hold, or one whose hold ran out: the guard changes hands mid-fall.
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", HOST_IDENTITY };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	host.Scan(GUARD, Here::kAlive, t0);
	bob.Pump(t0);
	host.Hits(GUARD, 0.36f, t0 + 1s);
	host.Hits(GUARD, std::nullopt, t0 + 2s);
	host.Scan(GUARD, Here::kDying, t0 + 2250ms, false);
	CHECK(host.releases == 1);
	bob.Pump(t0 + 2250ms);  // told nobody runs it
	bob.Scan(GUARD, Here::kAlive, t0 + 2250ms);
	host.Pump(t0 + 2250ms);
	REQUIRE(bob.OwnerOf(GUARD) == bob.id);
	REQUIRE(host.OwnerOf(GUARD) == bob.id);

	// What the live host's game sent: found dead in a guard Bob's game runs. Not believed, as before: the host's
	// game had it alive. WorldSync's check now leaves a guard still falling to its death event.
	host.FoundDead(GUARD);
	CHECK(bob.Pump(t0 + 2500ms).empty());
	CHECK(f.Logged(std::format("death of {:08X} from player {} dropped: player {} runs it and the reporter wasn't fighting it", GUARD, host.id, bob.id)));

	host.SawDie(GUARD, true);
	const auto deaths = bob.Pump(t0 + 5s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == host.id);
	CHECK(bob.OwnerOf(GUARD) == 0);
}

TEST("npc hold: a one-shot kill by the game that ran the NPC is believed after it changed hands, for deathGrace")
{
	Server::Options options;
	options.deathGrace = 1000ms;
	Fixture    f{ options };
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	Game       carol{ f, 3, "Carol" };
	const auto t0 = Clock::now();
	alice.Scan(GUARD, Here::kAlive, t0);
	alice.Scan(RAIDER, Here::kAlive, t0);
	bob.Pump(t0);
	carol.Pump(t0);

	// Sneak headshots from Alice: no health report, no takeover claim (her game runs them). Her game, without the
	// hold, lets both go as they fall, and Bob's takes them over.
	alice.Hits(GUARD, std::nullopt, t0 + 1s);
	alice.Hits(RAIDER, std::nullopt, t0 + 1s);
	alice.Scan(GUARD, Here::kDying, t0 + 1250ms, false);
	alice.Scan(RAIDER, Here::kDying, t0 + 1250ms, false);
	bob.Pump(t0 + 1250ms);
	bob.Scan(GUARD, Here::kAlive, t0 + 1250ms);
	bob.Scan(RAIDER, Here::kAlive, t0 + 1250ms);
	REQUIRE(bob.OwnerOf(GUARD) == bob.id);
	REQUIRE(bob.OwnerOf(RAIDER) == bob.id);
	carol.Pump(t0 + 1250ms);

	alice.SawDie(GUARD, true);
	auto deaths = carol.Pump(t0 + 1500ms);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == GUARD);
	CHECK(deaths[0].playerId == alice.id);
	bob.Pump(t0 + 1500ms);

	// Carol never ran nor fought the raider: hers is dropped. Nor is Alice's believed once the grace is over.
	carol.SawDie(RAIDER, true);
	CHECK(bob.Pump(t0 + 1500ms).empty());
	std::this_thread::sleep_for(1100ms);
	alice.SawDie(RAIDER, true);
	CHECK(carol.Pump(t0 + 2s).empty());
	CHECK(f.Logged(std::format("death of {:08X} from player {} dropped", RAIDER, carol.id)));
	bob.SawDie(RAIDER, true);  // Bob's game, which runs it, is believed as always
	deaths = carol.Pump(t0 + 2s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == bob.id);
}

TEST("npc hold: a friend takes over a raider in the middle of a fight: the old runner's killing blow still counts")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	alice.Scan(RAIDER, Here::kAlive, t0);
	bob.Pump(t0);
	REQUIRE(bob.OwnerOf(RAIDER) == alice.id);

	std::this_thread::sleep_for(5100ms);  // the server's TAKEOVER_COOLDOWN: a takeover is granted now
	const auto t1 = Clock::now();
	// Alice one-shots it as Bob's shot lands: Bob's takeover reaches the server first, and Alice's game, told Bob runs
	// it now, lets it fall as a copy.
	bob.Hits(RAIDER, 0.5f, t1);
	alice.Hits(RAIDER, std::nullopt, t1);
	REQUIRE(bob.OwnerOf(RAIDER) == bob.id);
	REQUIRE(alice.OwnerOf(RAIDER) == bob.id);
	CHECK(alice.hold.Dying() == 0);  // not hers to keep

	alice.SawDie(RAIDER, true);
	const auto deaths = bob.Pump(t1 + 3s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == alice.id);
	CHECK(bob.OwnerOf(RAIDER) == 0);
	CHECK(!f.Logged("dropped"));
}

TEST("npc hold: our own hits keep a hand-off from moving a long shot's target to a nearer friend mid-fight")
{
	// NpcSync::ShouldHandOff: a friend far nearer to it than we are gets it, unless our player hit it lately.
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	alice.Scan(GUARD, Here::kAlive, t0);
	bob.Pump(t0);
	alice.Hits(GUARD, 0.5f, t0 + 1s);

	const auto handOff = [&](Clock::time_point a_now) {
		if (alice.OwnerOf(GUARD) == alice.id && !alice.hold.FoughtLately(GUARD, a_now)) {
			alice.Scan(GUARD, Here::kGone, a_now);  // released, as a hand-off is
		}
	};
	for (auto now = t0 + 1s; now < t0 + 11s; now += 250ms) {
		handOff(now);
		bob.Scan(GUARD, Here::kAlive, now);
	}
	CHECK(alice.releases == 0);
	CHECK(bob.OwnerOf(GUARD) == alice.id);

	// The fight over, it goes to Bob, where it stands.
	handOff(t0 + 11s);
	bob.Pump(t0 + 11s);
	bob.Scan(GUARD, Here::kAlive, t0 + 11s);
	CHECK(alice.releases == 1);
	CHECK(bob.OwnerOf(GUARD) == bob.id);
}

TEST("npc hold: the host shoots a friend's raider dead: its found-dead check leaves the fall to the death event, its kill")
{
	// Live, every kill of the host's on an NPC the friend's game ran: its hit asked to take the raider over, the server's
	// answer (still the friend's) put it up for the found-dead check the frame it fell, and the found-dead report was
	// dropped; the death event that followed was taken for the same death. The raider fought on in the friend's game.
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", HOST_IDENTITY };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	bob.Scan(RAIDER, Here::kAlive, t0);
	host.Pump(t0);
	REQUIRE(host.OwnerOf(RAIDER) == bob.id);

	host.Hits(RAIDER, 0.4f, t0 + 1s);
	host.Hits(RAIDER, std::nullopt, t0 + 2s);  // the killing blow; the takeover is refused (Bob's claim was moments ago)
	REQUIRE(host.OwnerOf(RAIDER) == bob.id);
	for (auto now = t0 + 2s; now < t0 + 5s; now += 250ms) {
		CHECK(!host.Check(RAIDER, true, now));
	}
	CHECK(bob.Pump(t0 + 5s).empty());

	host.Kills(RAIDER, t0 + 5s);
	const auto deaths = bob.Pump(t0 + 5s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == RAIDER);
	CHECK(deaths[0].playerId == host.id);
	CHECK(deaths[0].killed);
	CHECK(!deaths[0].foundDead);
	CHECK(!f.Logged("dropped"));
}

TEST("npc hold: a grenade kill of a friend's NPC is believed: the kill asks to take it over before its death goes")
{
	// A grenade's, a mine's or a fire's blows name no attacker: no hit of ours, no health report, no claim.
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", HOST_IDENTITY };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	bob.Scan(RAIDER, Here::kAlive, t0);
	bob.Scan(GUARD, Here::kAlive, t0);
	host.Pump(t0);
	REQUIRE(host.OwnerOf(RAIDER) == bob.id);

	// Falling: left to the death event, which comes with our player as the killer.
	CHECK(!host.Check(RAIDER, true, t0 + 1s));
	host.Kills(RAIDER, t0 + 3s);
	const auto deaths = bob.Pump(t0 + 3s);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == host.id);
	CHECK(deaths[0].killed);

	// Without the claim, the same kill was dropped.
	host.SawDie(GUARD, true);
	CHECK(bob.Pump(t0 + 3s).empty());
	CHECK(f.Logged(std::format("death of {:08X} from player {} dropped", GUARD, host.id)));
}

TEST("npc hold: a fall whose death event never comes is found dead after 15 s, and one long dead at once")
{
	Fixture    f{ HostedOptions() };
	Game       host{ f, 1, "Host", HOST_IDENTITY };
	const auto t0 = Clock::now();
	CHECK(!host.Check(GUARD, true, t0));
	CHECK(!host.Check(GUARD, true, t0 + 14s));
	CHECK(host.Check(GUARD, true, t0 + NpcHold::DYING));
	CHECK(host.waits.Dying() == 0);
	CHECK(host.Check(RAIDER, false, t0));  // a corpse from the save, nobody hit lately
	// Hit by our player lately but already lying dead: still left to the death event for a while.
	host.hold.Fought(NPC_SHOT, t0);
	CHECK(!host.Check(NPC_SHOT, false, t0 + 1s));
	CHECK(host.Check(NPC_SHOT, false, t0 + 1s + NpcHold::DYING));
}
