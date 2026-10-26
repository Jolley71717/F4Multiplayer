// Our game stopped by a menu hands the NPCs it runs to friends near them, and offers to run nothing new (PauseHandOff, as
// NpcSync::Scan uses it). The last tests play two games against a real server, as NpcHoldTests does: each scan's claim
// or release goes to the server, and what it answers goes back into that game.

#include "ServerFixture.h"

#include "game/NpcClaims.h"
#include "game/PauseHandOff.h"

#include <unordered_map>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using PauseHandOff::Why;
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t DIAMOND_CITY = 0x0001F4DC;  // an interior, for "another space"
	constexpr std::uint32_t RAIDER = 0x0001F301;
	constexpr std::uint32_t GUARD = 0x00095E8A;
	constexpr std::uint32_t SPAWN = 0xFF000801;  // a spawn id (Protocol::IsSpawnRef)

	PauseHandOff::Spot Outside(float a_x, float a_y = 0.0f)
	{
		return { 0, COMMONWEALTH, a_x, a_y, 0.0f };
	}

	PauseHandOff::Npc Raider()
	{
		return { .id = RAIDER, .at = Outside(1000.0f) };
	}

	const std::vector<PauseHandOff::Friend> BOB_NEAR{ { Outside(3000.0f) } };
}

TEST("pause hand-off: a short stop hands nothing off, and offers to run NPCs as before")
{
	CHECK(PauseHandOff::Offers(0s));
	CHECK(PauseHandOff::Offers(4900ms));
	CHECK(PauseHandOff::Decide(0s, Raider(), BOB_NEAR) == Why::kRunning);
	CHECK(PauseHandOff::Decide(2s, Raider(), BOB_NEAR) == Why::kRunning);  // a look at the Pip-Boy
	CHECK(PauseHandOff::Decide(4900ms, Raider(), BOB_NEAR) == Why::kRunning);
}

TEST("pause hand-off: after 5 s, an NPC a friend in the same space is near goes; a stopped game offers no claims")
{
	CHECK(!PauseHandOff::Offers(PauseHandOff::AFTER));
	CHECK(!PauseHandOff::Offers(60s));
	CHECK(PauseHandOff::Decide(PauseHandOff::AFTER, Raider(), BOB_NEAR) == Why::kRelease);
	// As far as HANDOFF_FROM, and no farther.
	CHECK(PauseHandOff::Decide(6s, Raider(), { { Outside(1000.0f, 5999.0f) } }) == Why::kRelease);
	CHECK(PauseHandOff::Decide(6s, Raider(), { { Outside(1000.0f, 6001.0f) } }) == Why::kNoFriend);
	// Any one friend near will do.
	CHECK(PauseHandOff::Decide(6s, Raider(), { { Outside(90000.0f) }, { Outside(2000.0f) } }) == Why::kRelease);
}

TEST("pause hand-off: an NPC is kept when no friend can take it on: none, one in another space, or one stopped too")
{
	CHECK(PauseHandOff::Decide(6s, Raider(), {}) == Why::kNoFriend);
	// The same coordinates in an interior are nowhere near it.
	CHECK(PauseHandOff::Decide(6s, Raider(), { { { DIAMOND_CITY, 0, 1000.0f, 0.0f, 0.0f } } }) == Why::kNoFriend);
	CHECK(PauseHandOff::Decide(6s, { .id = RAIDER, .at = { DIAMOND_CITY, 0, 0.0f, 0.0f, 0.0f } }, BOB_NEAR) == Why::kNoFriend);
	// A friend in a menu that stops their game would only stand it still too; one talking or building runs on.
	for (const auto menu : { Protocol::MenuKind::kPipboy, Protocol::MenuKind::kPaused, Protocol::MenuKind::kLooting, Protocol::MenuKind::kTerminal }) {
		CHECK(PauseHandOff::Decide(6s, Raider(), { { Outside(3000.0f), menu } }) == Why::kNoFriend);
	}
	for (const auto menu : { Protocol::MenuKind::kTalking, Protocol::MenuKind::kCrafting }) {
		CHECK(PauseHandOff::Decide(6s, Raider(), { { Outside(3000.0f), menu } }) == Why::kRelease);
	}
}

TEST("pause hand-off: our companion, our conversation's partner, a settler we command, a fought NPC and our spawns stay ours")
{
	auto npc = Raider();
	npc.companion = true;
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kOurs);
	npc = Raider();
	npc.talkingToUs = true;
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kOurs);
	npc = Raider();
	npc.holdsUs = true;
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kOurs);
	npc = Raider();
	npc.commanded = true;
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kOurs);
	npc = Raider();
	npc.fought = true;  // our player hit it within NpcHold::FOUGHT: our kill must land where it runs
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kFought);
	npc = Raider();
	npc.id = SPAWN;  // made in our game: no other game has it
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kSpawn);
	npc = Raider();
	npc.handedOffLately = true;  // within HANDOFF_RETRY: not traded back and forth
	CHECK(PauseHandOff::Decide(60s, npc, BOB_NEAR) == Why::kLately);
}

namespace
{
	// One player's game, as far as NpcSync::Scan's claims and releases go.
	struct Game
	{
		Fixture&                                         f;
		PeerId                                           peer;
		std::uint32_t                                    id = 0;
		NpcClaims::Book                                  claims;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;
		std::uint32_t                                    releases = 0;
		std::uint32_t                                    offered = 0;

		Game(Fixture& a_f, PeerId a_peer, std::string a_name) :
			f(a_f),
			peer(a_peer),
			id(JoinAs(a_f, a_peer, std::move(a_name)))
		{}

		[[nodiscard]] std::uint32_t OwnerOf(std::uint32_t a_ref) const
		{
			const auto it = owners.find(a_ref);
			return it == owners.end() ? 0 : it->second;
		}

		// What the server sent, owners applied as NpcSync::ApplyOwners does.
		void Pump(Clock::time_point a_now)
		{
			for (const auto& packet : Received(f, peer)) {
				if (packet.empty() || packet[0] != Type(MT::kActorOwners)) {
					continue;
				}
				const auto batch = Protocol::DecodeOwners(packet);
				REQUIRE(batch);
				for (const auto& entry : *batch) {
					claims.Answered(entry.refId, entry.playerId, id, a_now);
					if (entry.playerId == 0) {
						owners.erase(entry.refId);
					} else {
						owners[entry.refId] = entry.playerId;
					}
				}
			}
		}

		// NpcSync::Scan for one loaded NPC, our game stopped for a_stoppedFor: one we run goes when PauseHandOff says
		// so; one nobody runs is offered only while PauseHandOff::Offers.
		void Scan(std::uint32_t a_ref, Clock::duration a_stoppedFor, const std::vector<PauseHandOff::Friend>& a_friends, Clock::time_point a_now)
		{
			if (OwnerOf(a_ref) == id) {
				if (PauseHandOff::Decide(a_stoppedFor, { .id = a_ref, .at = Outside(1000.0f) }, a_friends) == Why::kRelease) {
					owners.erase(a_ref);
					f.fake->Deliver(peer, Protocol::EncodeRelease({ a_ref }));
					++releases;
					Pump(a_now);
				}
			} else if (PauseHandOff::Offers(a_stoppedFor)) {
				if (const auto reason = claims.Offer(a_ref, OwnerOf(a_ref), id, false, true, a_now)) {
					f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, *reason } }));
					++offered;
					Pump(a_now);
				}
			}
			claims.Expire(a_now);
		}
	};
}

TEST("pause hand-off: Alice's Pip-Boy stays open: after 5 s the raider she ran is Bob's, granted by the server as nobody's")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	Game       bob{ f, 2, "Bob" };
	const auto t0 = Clock::now();
	alice.Scan(RAIDER, 0s, {}, t0);
	bob.Pump(t0);
	REQUIRE(alice.OwnerOf(RAIDER) == alice.id);
	REQUIRE(bob.OwnerOf(RAIDER) == alice.id);

	// Alice opens her Pip-Boy at t0; both games scan every 250 ms (Bob's has nothing to claim: Alice's game runs it).
	// Bob stands near the raider, playing.
	auto now = t0;
	for (; now < t0 + PauseHandOff::AFTER; now += 250ms) {
		alice.Scan(RAIDER, now - t0, BOB_NEAR, now);
		bob.Scan(RAIDER, 0s, {}, now);
	}
	CHECK(bob.offered == 0);
	CHECK(alice.releases == 0);
	CHECK(bob.OwnerOf(RAIDER) == alice.id);

	alice.Scan(RAIDER, now - t0, BOB_NEAR, now);
	CHECK(alice.releases == 1);
	bob.Pump(now);  // told nobody runs it
	CHECK(bob.OwnerOf(RAIDER) == 0);
	bob.Scan(RAIDER, 0s, {}, now);  // claimed as nobody's
	alice.Pump(now);
	CHECK(bob.OwnerOf(RAIDER) == bob.id);
	CHECK(alice.OwnerOf(RAIDER) == bob.id);  // hers copies Bob's once her menu closes
	CHECK(!f.Logged("dropped"));
}

TEST("pause hand-off: a game stopped a while claims nothing, even nobody's; it does again once the menu closes")
{
	Fixture    f;
	Game       alice{ f, 1, "Alice" };
	const auto t0 = Clock::now();
	for (auto now = t0; now < t0 + 10s; now += 250ms) {
		alice.Scan(GUARD, 5s + (now - t0), {}, now);  // stopped since before the guard loaded
	}
	CHECK(alice.offered == 0);
	CHECK(alice.OwnerOf(GUARD) == 0);

	alice.Scan(GUARD, 0s, {}, t0 + 10s);
	CHECK(alice.offered == 1);
	CHECK(alice.OwnerOf(GUARD) == alice.id);
}
