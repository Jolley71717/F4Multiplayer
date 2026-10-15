// NPCs a game makes at runtime (random encounters, ambushes) shared between games under spawn ids: the wire format, the
// server's grants and limits, what reaches whom, and two games' books (SpawnBook.h) played against a real server.

#include "ServerFixture.h"

#include "game/SpawnBook.h"

#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Reason = Protocol::ClaimReason;
	using SpawnBook::Act;
	using SpawnBook::Facts;
	using RemoteEdits::Spot;

	constexpr std::uint32_t WORLD = 0x0000003C;    // the Commonwealth
	constexpr std::uint32_t RAIDER = 0x0001D4D2;   // a plugin's NPC base
	constexpr std::uint32_t SPAWN = 0xFF001234;    // a spawn id
	constexpr std::uint32_t SPAWN2 = 0xFF005678;
	constexpr std::uint32_t NPC = 0x0001F001;      // a plugin's reference

	Protocol::Spawn Raider(std::uint32_t a_id, float a_x = 1000.0f)
	{
		return { 0, a_id, RAIDER, 0, WORLD, a_x, 2000.0f, 100.0f, 0.5f };
	}

	auto Report(const Protocol::Spawn& a_spawn)
	{
		return Protocol::Encode(a_spawn, MT::kReportSpawn);
	}

	std::vector<Protocol::ActorOwner> Owners(const Packets& a_packets)
	{
		std::vector<Protocol::ActorOwner> out;
		for (const auto& batch : Decoded(a_packets, MT::kActorOwners, [](auto a_data) { return Protocol::DecodeOwners(a_data); })) {
			out.insert(out.end(), batch.begin(), batch.end());
		}
		return out;
	}

	bool HasOwner(const std::vector<Protocol::ActorOwner>& a_owners, std::uint32_t a_ref, std::uint32_t a_player)
	{
		return std::ranges::any_of(a_owners, [&](const auto& a_owner) { return a_owner.refId == a_ref && a_owner.playerId == a_player; });
	}

	std::vector<Protocol::Spawn> Spawned(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kSpawned, [](auto a_data) { return Protocol::DecodeSpawn(a_data); });
	}

	auto Death(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 999, true, 1000.0f, 2000.0f, 100.0f }, MT::kReportDeath);
	}

	auto Claim(std::uint32_t a_ref, Reason a_reason)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } });
	}

	auto States(std::uint32_t a_ref)
	{
		Protocol::ActorState state;
		state.refId = a_ref;
		state.worldspace = WORLD;
		state.x = 1100.0f;
		return Protocol::Encode(std::vector<Protocol::ActorState>{ state }, MT::kActorStates);
	}

	const auto T0 = SpawnBook::Clock::time_point{} + 1h;

	Spot At(float a_x, std::uint32_t a_worldspace = WORLD)
	{
		return { 0, a_worldspace, a_x, 2000.0f, 100.0f };
	}

	// Our player near the copy's spot, its owner's stand-in beside it.
	Facts Near()
	{
		return { .player = At(1500.0f), .owner = At(1200.0f) };
	}

	Facts Placed(float a_copyAt, float a_player = 1500.0f)
	{
		return { .player = At(a_player), .owner = At(1200.0f), .copyThere = true, .copyAt = At(a_copyAt) };
	}
}

// ---- wire --------------------------------------------------------------------------------------------------------

TEST("spawns: a spawn reads back, and one naming what no game can place does not")
{
	using Protocol::DecodeSpawn;
	using Protocol::Encode;
	const auto ok = [](Protocol::Spawn a_spawn) { return DecodeSpawn(Encode(a_spawn, MT::kSpawned)).has_value(); };
	const auto spawn = Raider(SPAWN);
	CHECK(DecodeSpawn(Encode(spawn, MT::kReportSpawn)) == spawn);
	CHECK(Protocol::IsSpawnRef(SPAWN) && !Protocol::IsShareableRef(SPAWN) && Protocol::IsActorRef(SPAWN));
	CHECK(!Protocol::IsSpawnRef(0xFF000000) && !Protocol::IsSpawnRef(NPC) && !Protocol::IsSpawnRef(Protocol::PLAYER_REF_ID));

	auto bad = spawn;
	bad.refId = NPC;  // a plugin's reference: every game has it already
	CHECK(!ok(bad));
	bad.refId = 0xFF000000;
	CHECK(!ok(bad));
	bad = spawn;
	bad.base = 0xFF000801;  // a base our game made: no other game has it
	CHECK(!ok(bad));
	bad.base = 0;
	CHECK(!ok(bad));
	bad.base = Protocol::PLAYER_REF_ID;
	CHECK(!ok(bad));
	bad = spawn;
	bad.cell = 0x0001A001;  // in a cell and a worldspace at once
	CHECK(!ok(bad));
	bad.worldspace = 0;
	CHECK(ok(bad));  // an interior
	bad.cell = 0;
	CHECK(!ok(bad));  // nowhere
	bad = spawn;
	bad.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(!ok(bad));
	bad = spawn;
	bad.z = 1.0e9f;
	CHECK(!ok(bad));
	bad = spawn;
	bad.heading = std::numeric_limits<float>::infinity();
	CHECK(!ok(bad));

	auto packet = Encode(spawn, MT::kSpawned);
	packet.pop_back();
	CHECK(!DecodeSpawn(packet));
	packet = Encode(spawn, MT::kSpawned);
	packet.push_back(0);
	CHECK(!DecodeSpawn(packet));
}

// ---- server ------------------------------------------------------------------------------------------------------

TEST("spawns: the server grants a spawn to the game that made it, and the others place a copy")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(Raider(SPAWN)));
	const auto toAlice = Received(f, 1);
	CHECK(HasOwner(Owners(toAlice), SPAWN, alice));
	CHECK(Spawned(toAlice).empty());  // her own: she has it

	const auto toBob = Received(f, 2);
	const auto spawned = Spawned(toBob);
	REQUIRE(spawned.size() == 1);
	auto expected = Raider(SPAWN);
	expected.playerId = alice;
	CHECK(spawned[0] == expected);
	CHECK(HasOwner(Owners(toBob), SPAWN, alice));
	(void)bob;
}

TEST("spawns: an id another game's spawn has is refused, naming that game")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Report(Raider(SPAWN, 5000.0f)));
	const auto toBob = Received(f, 2);
	CHECK(Owners(toBob).size() == 1);
	CHECK(HasOwner(Owners(toBob), SPAWN, alice));
	const auto toAlice = Received(f, 1);
	CHECK(Spawned(toAlice).empty());
	CHECK(Owners(toAlice).empty());

	// Another id is hers to have.
	f.fake->Deliver(1, Report(Raider(SPAWN2)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN2, alice));
}

TEST("spawns: each game shares so many at once, and the session so many")
{
	Server::Options options;
	options.maxPlayers = 5;  // four full shares, and one more game
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 0; i < Protocol::MAX_SPAWNS_PER_PLAYER; ++i) {
		f.fake->Deliver(1, Report(Raider(0xFF100000 + i)));
	}
	f.fake->Deliver(1, Report(Raider(0xFF1000FF)));
	const auto owners = Owners(Received(f, 1));
	CHECK(owners.size() == Protocol::MAX_SPAWNS_PER_PLAYER + 1);
	CHECK(HasOwner(owners, 0xFF100000, alice));
	CHECK(HasOwner(owners, 0xFF1000FF, 0));  // refused: nobody's
	CHECK(Spawned(Received(f, 2)).size() == Protocol::MAX_SPAWNS_PER_PLAYER);
	CHECK(f.Logged("spawn FF1000FF from player"));

	// One of hers dies: room for another, once her budget has a token again (a burst is her whole share).
	f.fake->Deliver(1, Death(0xFF100000));
	Clear(f, { 1 });
	std::this_thread::sleep_for(600ms);
	f.fake->Deliver(1, Report(Raider(0xFF1000FF)));
	CHECK(HasOwner(Owners(Received(f, 1)), 0xFF1000FF, alice));

	// The session's: four games' full shares.
	for (PeerId peer = 3; peer <= 5; ++peer) {
		JoinAs(f, peer, std::format("Player{}", peer));
	}
	for (PeerId peer = 2; peer <= 4; ++peer) {
		for (std::uint32_t i = 0; i < Protocol::MAX_SPAWNS_PER_PLAYER; ++i) {
			f.fake->Deliver(peer, Report(Raider(0xFF000000 + (peer << 16) + i + 1)));
		}
	}
	Clear(f, { 1, 2, 3, 4, 5 });
	f.fake->Deliver(5, Report(Raider(0xFF500001)));
	CHECK(HasOwner(Owners(Received(f, 5)), 0xFF500001, 0));
	CHECK(Spawned(Received(f, 1)).empty());
}

TEST("spawns: a newcomer places the spawns already shared before it hears who runs them")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	Sync(f, 1);

	JoinAs(f, 2, "Bob");
	const auto packets = Received(f, 2);
	const auto spawned = std::ranges::find_if(packets, [](const auto& a_packet) { return a_packet[0] == Type(MT::kSpawned); });
	const auto owners = std::ranges::find_if(packets, [&](const auto& a_packet) {
		const auto decoded = a_packet[0] == Type(MT::kActorOwners) ? Protocol::DecodeOwners(a_packet) : std::nullopt;
		return decoded && HasOwner(*decoded, SPAWN, alice);
	});
	REQUIRE(spawned != packets.end());
	REQUIRE(owners != packets.end());
	CHECK(spawned < owners);
	CHECK(Spawned(packets)[0].playerId == alice);

	// Her own are not sent back to her when she joins again: she leaves, and they are over.
	f.fake->Drop(1);
	const auto ended = Owners(Received(f, 2));
	CHECK(HasOwner(ended, SPAWN, 0));
	JoinAs(f, 3, "Carol");
	CHECK(Spawned(Received(f, 3)).empty());
}

TEST("spawns: another game never takes one over; fighting it makes its kill believed")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	Clear(f, { 1, 2, 3 });

	// Bob hits his copy: told it is Alice's, and noted as fighting it.
	f.fake->Deliver(2, Claim(SPAWN, Reason::kInteract));
	f.fake->Deliver(2, Claim(SPAWN, Reason::kUnowned));
	const auto toBob = Owners(Received(f, 2));
	CHECK(!toBob.empty());
	CHECK(std::ranges::all_of(toBob, [&](const auto& a_owner) { return a_owner.refId == SPAWN && a_owner.playerId == alice; }));
	CHECK(Owners(Received(f, 1)).empty());

	// Carol, who never fought it, says it died: not believed.
	f.fake->Deliver(3, Death(SPAWN));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
	CHECK(f.Logged("death of spawn FF001234 from player"));

	// Bob kills it: Alice's game kills hers, everyone's copy goes, and it is over.
	f.fake->Deliver(2, Death(SPAWN));
	const auto toAlice = Received(f, 1);
	const auto deaths = Decoded(toAlice, MT::kActorDied, [](auto a_data) { return Protocol::DecodeActorDeath(a_data); });
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == SPAWN && deaths[0].playerId == bob && deaths[0].killed);
	CHECK(HasOwner(Owners(toAlice), SPAWN, 0));
	CHECK(HasOwner(Owners(Received(f, 3)), SPAWN, 0));

	// Not with the session's dead: a joiner neither places it nor has it dead, and its id may be granted again.
	JoinAs(f, 4, "Dan");
	const auto toDan = Received(f, 4);
	CHECK(Spawned(toDan).empty());
	const auto world = WorldStates(toDan);
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.empty());
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, alice));
}

TEST("spawns: what an owner's game says of its spawn reaches the others, and nobody else's word does")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	Clear(f, { 1, 2 });

	// Its states and shots, from Alice's game.
	f.fake->Deliver(1, States(SPAWN));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 0, SPAWN }, MT::kReportShot));
	const auto toBob = Received(f, 2);
	CHECK(Count(toBob, MT::kActorStatesRelay) == 1);
	CHECK(Count(toBob, MT::kShotFired) == 1);
	// From Bob's: dropped.
	f.fake->Deliver(2, States(SPAWN));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Shot{ 0, SPAWN }, MT::kReportShot));
	const auto toAlice = Received(f, 1);
	CHECK(Count(toAlice, MT::kActorStatesRelay) == 0);
	CHECK(Count(toAlice, MT::kShotFired) == 0);

	// Bob hurt his copy: the health reaches Alice's game. A spawn nobody shares: dropped.
	f.fake->Deliver(2, Protocol::Encode(Protocol::ActorHealth{ SPAWN, 0.25f }, MT::kReportHealth));
	f.fake->Deliver(2, Protocol::Encode(Protocol::ActorHealth{ SPAWN2, 0.25f }, MT::kReportHealth));
	const auto health = Decoded(Received(f, 1), MT::kActorHealth, [](auto a_data) { return Protocol::DecodeActorHealth(a_data); });
	REQUIRE(health.size() == 1);
	CHECK(health[0].refId == SPAWN);

	// It hits Bob's stand-in in Alice's game: Bob takes it. Said by Bob's game of Alice's stand-in (there it is his
	// copy, which hits nobody's), or of a spawn nobody shares: dropped.
	f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerHit{ bob, 12.0f, false, SPAWN }, MT::kPlayerHit));
	f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerHit{ bob, 12.0f, false, SPAWN2 }, MT::kPlayerHit));
	const auto damaged = Decoded(Received(f, 2), MT::kPlayerDamaged, [](auto a_data) { return Protocol::DecodePlayerHit(a_data); });
	REQUIRE(damaged.size() == 1);
	CHECK(damaged[0].attacker == SPAWN);
	f.fake->Deliver(2, Protocol::Encode(Protocol::PlayerHit{ alice, 12.0f, false, SPAWN }, MT::kPlayerHit));
	CHECK(Count(Received(f, 1), MT::kPlayerDamaged) == 0);
}

TEST("spawns: one its owner's game lets go of is over everywhere")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Report(Raider(SPAWN)));
	f.fake->Deliver(1, Report(Raider(SPAWN2)));
	Clear(f, { 1, 2 });

	// Bob's game can't let it go: it isn't his.
	f.fake->Deliver(2, Protocol::EncodeRelease(std::vector<std::uint32_t>{ SPAWN }));
	CHECK(Owners(Received(f, 1)).empty());

	f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ SPAWN }));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN, 0));
	JoinAs(f, 3, "Carol");
	const auto spawned = Spawned(Received(f, 3));
	REQUIRE(spawned.size() == 1);
	CHECK(spawned[0].refId == SPAWN2 && spawned[0].playerId == alice);
}

// ---- the books ---------------------------------------------------------------------------------------------------

TEST("spawns: the id asked for is the NPC's own, then others in the runtime range")
{
	CHECK(SpawnBook::CandidateId(SPAWN, 0) == SPAWN);
	std::set<std::uint32_t> seen{ SPAWN };
	for (std::uint32_t attempt = 1; attempt < 16; ++attempt) {
		const auto id = SpawnBook::CandidateId(SPAWN, attempt);
		CHECK(Protocol::IsSpawnRef(id));
		seen.insert(id);
	}
	CHECK(seen.size() == 16);
	CHECK(Protocol::IsSpawnRef(SpawnBook::CandidateId(0xFF000000, 0)));
	CHECK(Protocol::IsSpawnRef(SpawnBook::CandidateId(NPC, 0)));
}

TEST("spawns: a copy is placed from the NPC every game puts together the same")
{
	using SpawnBook::Base;
	using SpawnBook::PickBase;
	const Base own{ 0xFF000801, true, false };     // the game's own copy: in no other game
	const Base leveled{ 0x0001A100, true, true };  // a leveled NPC: each game rolls its list on its own
	const Base whole{ 0x0001A200, true, false };   // an NPC of its own
	const Base list{ 0x0001A300, false, false };   // a leveled list
	CHECK(PickBase(own, std::array{ leveled, whole }, leveled) == whole.id);
	CHECK(PickBase(own, std::array{ list, whole }, leveled) == whole.id);
	CHECK(PickBase(own, std::array{ leveled }, leveled) == leveled.id);
	CHECK(PickBase(own, {}, Base{}) == 0);
	CHECK(PickBase(whole, {}, Base{}) == whole.id);  // placed from a plugin's NPC, not leveled
	CHECK(PickBase(own, std::array{ Base{ 0xFF000900, true, false } }, Base{}) == 0);
}

TEST("spawns: our book asks once, again when unanswered, and waits after a refusal")
{
	SpawnBook::Ours ours;
	std::set<std::uint32_t> taken;
	const auto isTaken = [&](std::uint32_t a_id) { return taken.contains(a_id); };
	constexpr std::uint32_t LOCAL = 0xFF000ABC;
	constexpr std::uint32_t US = 1, FRIEND = 2;

	const auto first = ours.Offer(LOCAL, T0, isTaken);
	REQUIRE(first);
	CHECK(*first == LOCAL);
	CHECK(!ours.Offer(LOCAL, T0 + 1s, isTaken));
	CHECK(ours.IdOf(LOCAL) == 0);  // not granted yet
	CHECK(ours.Offer(LOCAL, T0 + SpawnBook::Ours::ANSWER_WAIT, isTaken) == first);  // never answered: asked again

	// Refused for a friend's spawn under that id: another id, after a wait.
	CHECK(ours.Answered(LOCAL, FRIEND, US, T0 + 6s));
	CHECK(!ours.Holds(LOCAL));
	CHECK(!ours.Offer(LOCAL, T0 + 7s, isTaken));
	const auto second = ours.Offer(LOCAL, T0 + 6s + SpawnBook::Ours::REFUSED_WAIT, isTaken);
	REQUIRE(second);
	CHECK(*second != LOCAL);
	CHECK(ours.Answered(*second, US, US, T0 + 20s));
	CHECK(ours.IdOf(LOCAL) == *second);
	CHECK(ours.LocalOf(*second) == LOCAL);
	CHECK(!ours.Offer(LOCAL, T0 + 60s, isTaken));  // granted: nothing more to ask

	// An id a friend's game shares is never asked for.
	constexpr std::uint32_t OTHER = 0xFF000DEF;
	taken.insert(OTHER);
	const auto third = ours.Offer(OTHER, T0, isTaken);
	REQUIRE(third);
	CHECK(*third != OTHER);
	CHECK(!ours.Answered(SPAWN2, US, US, T0));  // never ours

	// No more than our share at once.
	SpawnBook::Ours full;
	for (std::uint32_t i = 0; i < SpawnBook::Ours::MAX; ++i) {
		CHECK(full.Offer(0xFF010000 + i, T0, isTaken));
	}
	CHECK(!full.Offer(0xFF020000, T0, isTaken));
}

TEST("spawns: a copy is placed near us beside its owner, and goes when we leave it or its owner lets it go")
{
	SpawnBook::Theirs theirs;
	constexpr std::uint32_t US = 2;
	auto spawn = Raider(SPAWN);
	spawn.playerId = 1;
	CHECK(!theirs.Add(Protocol::Spawn{ US, SPAWN, RAIDER, 0, WORLD, 0, 0, 0, 0 }, US));  // our own
	REQUIRE(theirs.Add(spawn, US));
	CHECK(theirs.Knows(SPAWN));

	// Near us and beside its owner's stand-in: placed. Far from us, its owner far from it, in another space, or not
	// heard from: not.
	CHECK(theirs.Decide(SPAWN, Near(), T0) == Act::kPlace);
	CHECK(theirs.Decide(SPAWN, { .player = At(1000.0f + SpawnBook::PLACE_RANGE + 500.0f), .owner = At(1200.0f) }, T0) == Act::kNone);
	CHECK(theirs.Decide(SPAWN, { .player = At(1500.0f), .owner = At(1000.0f + SpawnBook::OWNER_RANGE + 500.0f) }, T0) == Act::kNone);
	CHECK(theirs.Decide(SPAWN, { .player = At(1500.0f, 0x0000AAAA), .owner = At(1200.0f) }, T0) == Act::kNone);
	CHECK(theirs.Decide(SPAWN, { .player = At(1500.0f) }, T0) == Act::kNone);
	auto unique = Near();
	unique.baseOk = false;
	CHECK(theirs.Decide(SPAWN, unique, T0) == Act::kNone);

	// Placing failed: tried again a little later, a few times.
	theirs.Placed(SPAWN, 0, T0);
	CHECK(theirs.Decide(SPAWN, Near(), T0 + 1s) == Act::kNone);
	CHECK(theirs.Decide(SPAWN, Near(), T0 + SpawnBook::PLACE_RETRY) == Act::kPlace);

	constexpr std::uint32_t COPY = 0xFF000777;
	theirs.Placed(SPAWN, COPY, T0 + 6s);
	CHECK(theirs.LocalOf(SPAWN) == COPY);
	CHECK(theirs.IdOf(COPY) == SPAWN);
	CHECK(theirs.Decide(SPAWN, Placed(1100.0f), T0) == Act::kNone);
	// Its 3D is not in yet, as after every placing: kept while the engine loads it, then taken for one that unloaded.
	auto loading = Placed(1100.0f);
	loading.copyLoaded = false;
	CHECK(theirs.Decide(SPAWN, loading, T0 + 7s) == Act::kNone);
	CHECK(theirs.Decide(SPAWN, loading, T0 + 6s + SpawnBook::LOAD_WAIT) == Act::kDiscard);
	// Its owner's game moves it: kept while we stand near it.
	theirs.Moved(SPAWN, At(4000.0f), 1.0f);
	CHECK(theirs.Find(SPAWN)->at.x == 4000.0f);
	CHECK(theirs.Decide(SPAWN, Placed(4000.0f), T0) == Act::kNone);
	// We left it behind, or its cell went: deleted, placed again on coming back.
	CHECK(theirs.Decide(SPAWN, Placed(4000.0f, 4000.0f + SpawnBook::KEEP_RANGE + 100.0f), T0) == Act::kDiscard);
	CHECK(theirs.Decide(SPAWN, { .player = At(1500.0f), .copyThere = true }, T0) == Act::kDiscard);
	// The engine took it (a load): forgotten, unplaced.
	CHECK(theirs.Decide(SPAWN, { .player = At(1500.0f) }, T0) == Act::kLost);
	theirs.Unplaced(SPAWN);
	CHECK(theirs.Knows(SPAWN));
	CHECK(theirs.IdOf(COPY) == 0);

	// Its owner's game let it go alive: the copy goes, and the record with it.
	theirs.Moved(SPAWN, At(1000.0f), 0.0f);
	theirs.Placed(SPAWN, COPY, T0);
	theirs.Gone(SPAWN);
	CHECK(theirs.Decide(SPAWN, Placed(1100.0f), T0) == Act::kDiscard);
	theirs.Unplaced(SPAWN);
	CHECK(!theirs.Knows(SPAWN));

	// One over before we placed it is forgotten.
	REQUIRE(theirs.Add(spawn, US));
	theirs.Gone(SPAWN);
	CHECK(theirs.Decide(SPAWN, Near(), T0) == Act::kForget);
}

TEST("spawns: two games share an ambush through the server, and the joiner's kill lands in the owner's game")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	SpawnBook::Ours   aliceOurs, bobOurs;
	SpawnBook::Theirs aliceTheirs, bobTheirs;
	constexpr std::uint32_t RAIDER_HERE = 0xFF0013ED;  // the raider in Alice's game
	constexpr std::uint32_t COPY = 0xFF002001;         // Bob's copy of it
	constexpr std::uint32_t BOB_RAIDER = 0xFF0013ED;   // a raider Bob's game made on its own: the number Alice's has there
	const auto deliver = [&](PeerId a_peer, SpawnBook::Ours& a_ours, SpawnBook::Theirs& a_theirs, std::uint32_t a_us) {
		for (const auto& packet : Received(f, a_peer)) {
			if (packet[0] == Type(MT::kSpawned)) {
				a_theirs.Add(*Protocol::DecodeSpawn(packet), a_us);
			} else if (packet[0] == Type(MT::kActorOwners)) {
				for (const auto& owner : *Protocol::DecodeOwners(packet)) {
					if (!a_ours.Answered(owner.refId, owner.playerId, a_us, T0) && owner.playerId == 0) {
						a_theirs.Gone(owner.refId);
					}
				}
			} else if (packet[0] == Type(MT::kActorDied)) {
				const auto death = *Protocol::DecodeActorDeath(packet);
				if (a_theirs.Knows(death.refId)) {
					a_theirs.Died(death.refId);
				} else if (const auto local = a_ours.LocalOf(death.refId)) {
					CHECK(local == RAIDER_HERE);  // Alice's game kills her raider
				}
			}
		}
	};

	// Alice's game made a raider: shared under its own id.
	const auto id = aliceOurs.Offer(RAIDER_HERE, T0, [&](std::uint32_t a_id) { return aliceTheirs.Knows(a_id); });
	REQUIRE(id);
	auto raider = Raider(*id);
	f.fake->Deliver(1, Report(raider));
	deliver(1, aliceOurs, aliceTheirs, alice);
	CHECK(aliceOurs.IdOf(RAIDER_HERE) == *id);

	// Bob's game is told, places its copy near him, and finds it under the spawn id.
	deliver(2, bobOurs, bobTheirs, bob);
	REQUIRE(bobTheirs.Knows(*id));
	CHECK(bobTheirs.Decide(*id, Near(), T0) == Act::kPlace);
	bobTheirs.Placed(*id, COPY, T0);
	CHECK(bobTheirs.IdOf(COPY) == *id);

	// Bob's game made one of its own, with the number Alice's spawn has: it asks for another id.
	const auto bobId = bobOurs.Offer(BOB_RAIDER, T0, [&](std::uint32_t a_id) { return bobTheirs.Knows(a_id); });
	REQUIRE(bobId);
	CHECK(*bobId != *id);
	f.fake->Deliver(2, Report(Raider(*bobId)));
	deliver(2, bobOurs, bobTheirs, bob);
	CHECK(bobOurs.IdOf(BOB_RAIDER) == *bobId);
	deliver(1, aliceOurs, aliceTheirs, alice);
	CHECK(aliceTheirs.Knows(*bobId));

	// Bob shoots his copy (a claim that notes him fighting it) and kills it: Alice's game kills hers.
	f.fake->Deliver(2, Claim(*id, Reason::kInteract));
	CHECK(bobTheirs.Died(*id) == COPY);
	f.fake->Deliver(2, Death(*id));
	deliver(1, aliceOurs, aliceTheirs, alice);
	CHECK(aliceOurs.LocalOf(*id) == 0);  // over: the server said nobody runs it now
	CHECK(!aliceOurs.Holds(*id));
	deliver(2, bobOurs, bobTheirs, bob);
	// Bob's corpse stays while he is near it, and goes, with the record, once he leaves.
	CHECK(bobTheirs.Decide(*id, Placed(1100.0f), T0) == Act::kNone);
	CHECK(bobTheirs.Decide(*id, Placed(1100.0f, 1100.0f + SpawnBook::KEEP_RANGE + 1.0f), T0) == Act::kDiscard);
	bobTheirs.Unplaced(*id);
	CHECK(!bobTheirs.Knows(*id));

	// Bob leaves: his spawn is over in Alice's game.
	f.fake->Drop(2);
	deliver(1, aliceOurs, aliceTheirs, alice);
	CHECK(aliceTheirs.Decide(*bobId, Near(), T0) == Act::kForget);
}
