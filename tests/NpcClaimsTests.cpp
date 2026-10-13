// Our game's offers to run NPCs, and when it makes them again (NpcClaims, as NpcSync::Scan, ApplyOwners and
// OnLocalInteraction use it: Scan's decision is Book::Offer itself). The tests play the games against a real server:
// each scan's claim goes to the server, and what the server answers goes back into that game's book, as NpcSync does.

#include "ServerFixture.h"

#include "game/NpcClaims.h"

#include <unordered_map>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Reason = Protocol::ClaimReason;
	using NpcClaims::Clock;

	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t RAIDER = 0x0001F301;
	constexpr auto          SCAN = 250ms;  // NpcSync's scan interval

	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	// One player's game: its peer, its id, who it thinks runs each NPC, and its claims.
	struct Game
	{
		Fixture&                                         f;
		PeerId                                           peer;
		std::uint32_t                                    id = 0;
		NpcClaims::Book                                  book;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;
		std::uint32_t                                    claimsSent = 0;

		Game(Fixture& a_f, PeerId a_peer, std::string a_name) :
			f(a_f),
			peer(a_peer)
		{
			const auto welcome = f.Join(a_peer, Fixture::MakeHello(std::move(a_name)));
			REQUIRE(welcome);
			id = welcome->playerId;
			Pump(Clock::now());
		}

		[[nodiscard]] std::uint32_t OwnerOf(std::uint32_t a_ref) const
		{
			const auto it = owners.find(a_ref);
			return it == owners.end() ? 0 : it->second;
		}

		// The owners the server sent, applied at a_now as NpcSync::ApplyOwners does.
		void Pump(Clock::time_point a_now)
		{
			Sync(f, peer);
			for (const auto& packet : f.fake->Take(peer)) {
				if (packet.empty() || packet[0] != Type(MT::kActorOwners)) {
					continue;
				}
				const auto batch = Protocol::DecodeOwners(packet);
				REQUIRE(batch);
				for (const auto& entry : *batch) {
					book.Answered(entry.refId, entry.playerId, id, a_now);
					if (entry.playerId == 0) {
						owners.erase(entry.refId);
					} else {
						owners[entry.refId] = entry.playerId;
					}
				}
			}
		}

		// NpcSync::Scan for one loaded NPC at a_now (a_companion: it is this player's companion; a_mayClaim: what
		// WorldSync::MayClaim says). True if it claimed; the answer is then pumped in.
		bool Scan(std::uint32_t a_ref, bool a_companion, Clock::time_point a_now, bool a_mayClaim = true)
		{
			const auto reason = book.Offer(a_ref, OwnerOf(a_ref), id, a_companion, a_mayClaim, a_now);
			if (reason) {
				f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, *reason } }));
				++claimsSent;
				Pump(a_now);
			}
			book.Expire(a_now);
			return reason.has_value();
		}

		// Its player hits or talks to the NPC (NpcSync::OnLocalInteraction): a takeover claim.
		void Interacts(std::uint32_t a_ref, Clock::time_point a_now)
		{
			f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Reason::kInteract } }));
			book.Claimed(a_ref, Reason::kInteract, a_now);
			Pump(a_now);
		}

		// Its game unloads the NPC, or the companion is dismissed and walks off: released.
		void Releases(std::uint32_t a_ref)
		{
			f.fake->Deliver(peer, Protocol::EncodeRelease({ a_ref }));
			owners.erase(a_ref);
		}

		// Scans every SCAN from a_from for a_span; returns the claims sent.
		std::uint32_t ScanFor(std::uint32_t a_ref, bool a_companion, Clock::time_point a_from, Clock::duration a_span)
		{
			std::uint32_t sent = 0;
			for (auto now = a_from; now < a_from + a_span; now += SCAN) {
				sent += Scan(a_ref, a_companion, now) ? 1 : 0;
			}
			return sent;
		}
	};
}

TEST("npc claims: two players with Dogmeat: the one who didn't get him asks again every 15 s, not four times a second")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();

	REQUIRE(alice.Scan(DOGMEAT, true, t0));
	CHECK(alice.OwnerOf(DOGMEAT) == alice.id);
	bob.Pump(t0);
	CHECK(bob.OwnerOf(DOGMEAT) == alice.id);

	// A minute of Bob's scans with Dogmeat following him too: refused each time, and he asks four times, not
	// every 3 s (twenty).
	CHECK(bob.ScanFor(DOGMEAT, true, t0, 60s) == 4);
	CHECK(bob.book.Refusals() == 4);
	CHECK(bob.OwnerOf(DOGMEAT) == alice.id);
	alice.Pump(t0 + 60s);
	CHECK(alice.OwnerOf(DOGMEAT) == alice.id);
}

TEST("npc claims: when the friend who has Dogmeat lets him go, the other player's game takes him at once")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	REQUIRE(alice.Scan(DOGMEAT, true, t0));
	bob.Pump(t0);
	REQUIRE(bob.Scan(DOGMEAT, true, t0));
	CHECK(bob.OwnerOf(DOGMEAT) == alice.id);
	CHECK(!bob.Scan(DOGMEAT, true, t0 + 5s));  // backing off

	// Alice's game unloads him (she fast-travelled away): the server tells everyone nobody runs him, and Bob's
	// next scan claims him, well inside the backoff.
	alice.Releases(DOGMEAT);
	bob.Pump(t0 + 6s);
	CHECK(bob.OwnerOf(DOGMEAT) == 0);
	REQUIRE(bob.Scan(DOGMEAT, true, t0 + 6s + SCAN));
	CHECK(bob.OwnerOf(DOGMEAT) == bob.id);
	alice.Pump(t0 + 7s);
	CHECK(alice.OwnerOf(DOGMEAT) == bob.id);
}

TEST("npc claims: a companion just recruited from a friend whose game merely runs it is taken at once")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();

	// Alice's game runs the settler because it was near her; Bob recruits it.
	REQUIRE(alice.Scan(DOGMEAT, false, t0));
	bob.Pump(t0);
	CHECK(!bob.Scan(DOGMEAT, false, t0));  // not his companion yet: Alice runs it
	REQUIRE(bob.Scan(DOGMEAT, true, t0 + 1s));
	CHECK(bob.OwnerOf(DOGMEAT) == bob.id);
	CHECK(bob.book.Refusals() == 0);
}

TEST("npc claims: Dogmeat's backoff holds back nothing else: a raider nobody runs is claimed on the same scan")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	REQUIRE(alice.Scan(DOGMEAT, true, t0));
	bob.Pump(t0);
	REQUIRE(bob.Scan(DOGMEAT, true, t0));
	CHECK(bob.OwnerOf(DOGMEAT) == alice.id);

	CHECK(!bob.Scan(DOGMEAT, true, t0 + SCAN));
	REQUIRE(bob.Scan(RAIDER, false, t0 + SCAN));
	CHECK(bob.OwnerOf(RAIDER) == bob.id);
}

TEST("npc claims: a takeover refused because the raider just changed hands is no reason to hold back later claims")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	REQUIRE(alice.Scan(RAIDER, false, t0));
	bob.Pump(t0);

	// Bob shoots it inside the takeover cooldown: refused, and the server says Alice still runs it.
	bob.Interacts(RAIDER, t0 + 1s);
	CHECK(bob.OwnerOf(RAIDER) == alice.id);
	CHECK(bob.book.Refusals() == 0);

	// Alice's game drops it; Bob's next scan claims it.
	alice.Releases(RAIDER);
	bob.Pump(t0 + 2s);
	REQUIRE(bob.Scan(RAIDER, false, t0 + 2s));
	CHECK(bob.OwnerOf(RAIDER) == bob.id);
}

TEST("npc claims: our game offers nothing while the session's deaths say not to, and never what it already runs")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();

	// Alice just loaded a save: the session's dead list isn't in, so her game offers nothing, not even Dogmeat at
	// her side (WorldSync::MayClaim).
	CHECK(!alice.Scan(DOGMEAT, true, t0, false));
	CHECK(!alice.Scan(RAIDER, false, t0, false));
	bob.Pump(t0);
	CHECK(bob.OwnerOf(DOGMEAT) == 0);
	CHECK(bob.OwnerOf(RAIDER) == 0);

	// Once it is in, both go on the next scan; from then on her game doesn't offer what it runs, a minute on.
	REQUIRE(alice.Scan(DOGMEAT, true, t0 + SCAN));
	REQUIRE(alice.Scan(RAIDER, false, t0 + SCAN));
	CHECK(alice.ScanFor(DOGMEAT, true, t0 + 2 * SCAN, 60s) == 0);
	CHECK(alice.ScanFor(RAIDER, false, t0 + 2 * SCAN, 60s) == 0);
	CHECK(alice.claimsSent == 2);
	bob.Pump(t0 + 1s);
	CHECK(bob.OwnerOf(DOGMEAT) == alice.id);
	CHECK(bob.OwnerOf(RAIDER) == alice.id);
}

TEST("npc claims: a claim the server hasn't answered isn't repeated for 3 s, then goes again")
{
	NpcClaims::Book book;
	const auto      t0 = Clock::now();
	CHECK(book.Due(RAIDER, Reason::kUnowned, t0));
	book.Claimed(RAIDER, Reason::kUnowned, t0);
	CHECK(!book.Due(RAIDER, Reason::kUnowned, t0 + 2s));
	CHECK(!book.Due(RAIDER, Reason::kCompanion, t0 + 2s));
	CHECK(book.Due(RAIDER, Reason::kUnowned, t0 + NpcClaims::RETRY));
	CHECK(book.Due(DOGMEAT, Reason::kUnowned, t0));
	book.Expire(t0 + NpcClaims::RETRY);
	CHECK(book.Due(RAIDER, Reason::kUnowned, t0 + 1s));  // expired: nothing held
}

TEST("npc claims: a refused companion claim waits 15 s, an unowned claim for it doesn't, and a release or our win ends the wait")
{
	NpcClaims::Book     book;
	const auto          t0 = Clock::now();
	constexpr std::uint32_t US = 2;
	constexpr std::uint32_t FRIEND = 1;

	book.Claimed(DOGMEAT, Reason::kCompanion, t0);
	book.Answered(DOGMEAT, FRIEND, US, t0);
	CHECK(book.Refusals() == 1);
	CHECK(!book.Due(DOGMEAT, Reason::kCompanion, t0 + 14s));
	CHECK(book.Due(DOGMEAT, Reason::kUnowned, t0 + 1s));
	CHECK(book.Due(DOGMEAT, Reason::kCompanion, t0 + NpcClaims::COMPANION_BACKOFF));

	book.Answered(DOGMEAT, 0, US, t0 + 2s);  // released
	CHECK(book.Due(DOGMEAT, Reason::kCompanion, t0 + 2s));

	book.Claimed(DOGMEAT, Reason::kCompanion, t0 + 3s);
	book.Answered(DOGMEAT, FRIEND, US, t0 + 3s);
	book.Answered(DOGMEAT, US, US, t0 + 4s);  // a later grant
	CHECK(book.Due(DOGMEAT, Reason::kCompanion, t0 + 4s));

	// An owner broadcast nobody asked for (someone else took it) starts no wait.
	book.Answered(RAIDER, FRIEND, US, t0);
	CHECK(book.Due(RAIDER, Reason::kCompanion, t0));

	book.Claimed(DOGMEAT, Reason::kCompanion, t0 + 5s);
	book.Answered(DOGMEAT, FRIEND, US, t0 + 5s);
	book.Clear();
	CHECK(book.Due(DOGMEAT, Reason::kCompanion, t0 + 5s));
	CHECK(book.Refusals() == 3);
}
