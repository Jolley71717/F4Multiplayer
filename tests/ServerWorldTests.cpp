// The session server's shared world: deaths, containers, pickups, doors, quests, NPC ownership,
// hits and the relays (shots, lines, voice, markers, pings).

#include "ServerFixture.h"

#include "game/DeathReports.h"
#include "game/RemoteEdits.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <set>
#include <thread>
#include <type_traits>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Reason = Protocol::ClaimReason;

	// References from plugin files (shareable) and one created at runtime (not).
	constexpr std::uint32_t NPC = 0x0001F001;
	constexpr std::uint32_t NPC2 = 0x0001F002;
	constexpr std::uint32_t NPC3 = 0x0001F003;
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t DOOR = 0x0003D001;
	constexpr std::uint32_t ITEM = 0x0004A001;
	constexpr std::uint32_t QUEST = 0x0005B001;
	constexpr std::uint32_t QUEST2 = 0x0005B002;
	constexpr std::uint32_t RUNTIME = 0xFF001234;

	// All ownership changes in the packets, in order.
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

	// --- what clients send ---

	auto Death(std::uint32_t a_ref, bool a_killed = true)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 999, a_killed }, MT::kReportDeath);
	}

	// A death seen happening: the corpse lies at (a_x, 0, 0) in the reporter's game.
	auto DeathAt(std::uint32_t a_ref, bool a_killed, float a_x)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 999, a_killed, a_x, 0.0f, 0.0f }, MT::kReportDeath);
	}

	// Found already dead in the reporter's save, not seen dying (the host's game sends these: WorldSync's
	// CheckFoundDead).
	auto FoundDeadReport(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::ActorDeath{ .refId = a_ref, .playerId = 999, .foundDead = true }, MT::kReportDeath);
	}

	auto Health(std::uint32_t a_ref, float a_health)
	{
		return Protocol::Encode(Protocol::ActorHealth{ a_ref, a_health }, MT::kReportHealth);
	}

	auto Container(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count)
	{
		return Protocol::Encode(Protocol::ContainerChange{ a_container, a_item, a_count }, MT::kReportContainer);
	}

	auto Pickup(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::RefPickedUp{ a_ref }, MT::kReportPickup);
	}

	auto Door(std::uint32_t a_ref, std::uint8_t a_open, std::uint8_t a_locked)
	{
		return Protocol::Encode(Protocol::RefState{ a_ref, a_open, a_locked }, MT::kReportRefState);
	}

	auto Claim(std::vector<Protocol::ActorClaim> a_claims)
	{
		return Protocol::Encode(a_claims);
	}

	auto Claim(std::uint32_t a_ref, Reason a_reason = Reason::kUnowned)
	{
		return Claim(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } });
	}

	auto Release(std::uint32_t a_ref)
	{
		return Protocol::EncodeRelease(std::vector<std::uint32_t>{ a_ref });
	}

	auto Positions(std::vector<std::uint32_t> a_refs)
	{
		std::vector<Protocol::ActorState> states;
		for (const auto ref : a_refs) {
			Protocol::ActorState state;
			state.refId = ref;
			state.x = 1.0f;
			states.push_back(state);
		}
		return Protocol::Encode(states, MT::kActorStates);
	}

	auto Hit(std::uint32_t a_victim, float a_damage, bool a_byPlayer, std::uint32_t a_attacker)
	{
		return Protocol::Encode(Protocol::PlayerHit{ a_victim, a_damage, a_byPlayer, a_attacker }, MT::kPlayerHit);
	}

	auto Stage(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		return Protocol::Encode(Protocol::QuestStage{ a_quest, a_stage }, MT::kReportQuestStage);
	}

	auto Markers(std::vector<Protocol::MapMarker> a_markers)
	{
		return Protocol::Encode(Protocol::MarkersFound{ 999, std::move(a_markers) }, MT::kReportMarkers);
	}

	// Sends a_packets as fast as possible, then waits until a_watcher has been sent a_count
	// packets of a_type, emptying every peer's queue along the way (big bursts would make WaitFor
	// slow). Returns what a_watcher got.
	Packets Burst(Fixture& a_f, const std::vector<std::pair<PeerId, std::vector<std::uint8_t>>>& a_packets, std::initializer_list<PeerId> a_peers,
		PeerId a_watcher, MT a_type, std::size_t a_count)
	{
		for (const auto& [peer, packet] : a_packets) {
			a_f.fake->Deliver(peer, packet);
		}
		Packets watched;
		const auto deadline = std::chrono::steady_clock::now() + 15s;
		while (Count(watched, a_type) < a_count && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(50ms);
			for (const auto peer : a_peers) {
				auto got = a_f.fake->Take(peer);
				if (peer == a_watcher) {
					std::ranges::move(got, std::back_inserter(watched));
				}
			}
		}
		return watched;
	}
}

// ---- deaths and health ------------------------------------------------------------------

TEST("server world: a death is relayed to the others with the reporter's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Death(NPC, true));  // claims to be player 999
	const auto deaths = Decoded(Received(f, 2), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == NPC);
	CHECK(deaths[0].playerId == alice);
	CHECK(deaths[0].killed);
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
}

TEST("server world: the same death isn't relayed twice")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Death(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Death(NPC));
	f.fake->Deliver(1, Death(NPC));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);
}

TEST("server world: deaths of the player or runtime references aren't shared")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Death(0));
	f.fake->Deliver(1, Death(Protocol::PLAYER_REF_ID));
	f.fake->Deliver(1, Death(RUNTIME));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);

	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.empty());
}

TEST("server world: a late joiner gets the dead actors")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Death(NPC));
	f.fake->Deliver(1, Death(NPC2, false));

	JoinAs(f, 2, "Bob");
	const auto world = WorldStates(Received(f, 2));
	REQUIRE(world.size() == 1);
	CHECK((std::set<std::uint32_t>(world[0].deadActors.begin(), world[0].deadActors.end()) == std::set<std::uint32_t>{ NPC, NPC2 }));
}

TEST("server world: a death frees the NPC's owner, and the dead can't be claimed")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, alice));
	Clear(f, { 1 });

	f.fake->Deliver(1, Death(NPC));  // its runner reports it (a report from anyone else is dropped)
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, 0));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, 0));

	f.fake->Deliver(2, Claim(NPC, Reason::kCompanion));
	CHECK(Owners(Received(f, 2)).empty());
	CHECK(Owners(Received(f, 1)).empty());

	JoinAs(f, 3, "Carol");
	CHECK(Owners(Received(f, 3)).empty());
}

TEST("server world: health is relayed to the others, but not for the dead")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Health(NPC, 0.5f));
	const auto health = Decoded(Received(f, 2), MT::kActorHealth, Protocol::DecodeActorHealth);
	REQUIRE(health.size() == 1);
	CHECK(health[0].refId == NPC);
	CHECK(health[0].health == 0.5f);
	CHECK(Count(Received(f, 1), MT::kActorHealth) == 0);

	f.fake->Deliver(1, Death(NPC));
	f.fake->Deliver(1, Health(NPC, 0.0f));
	f.fake->Deliver(1, Health(RUNTIME, 0.2f));
	CHECK(Count(Received(f, 2), MT::kActorHealth) == 0);
}

TEST("server world: damage a player took off their copy of an NPC is relayed to the others as damage, never none, more than all of it, or for the dead")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorHealth{ NPC, 0.25f }, MT::kReportDamage));
	const auto damage = Decoded(Received(f, 2), MT::kActorDamaged, Protocol::DecodeActorHealth);
	REQUIRE(damage.size() == 1);
	CHECK(damage[0].refId == NPC);
	CHECK(damage[0].health == 0.25f);
	CHECK(Count(Received(f, 1), MT::kActorDamaged) == 0);

	for (const float bad : { 0.0f, -0.5f, 1.5f }) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::ActorHealth{ NPC, bad }, MT::kReportDamage));
	}
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorHealth{ RUNTIME, 0.2f }, MT::kReportDamage));
	f.fake->Deliver(1, Death(NPC));
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorHealth{ NPC, 0.2f }, MT::kReportDamage));
	CHECK(Count(Received(f, 2), MT::kActorDamaged) == 0);
}

// ---- containers -------------------------------------------------------------------------

TEST("server world: container changes are numbered and echoed to everyone with the sender's ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Container(CHEST, ITEM, -3));
	f.fake->Deliver(1, Container(CHEST, ITEM + 1, 2));
	f.fake->Deliver(2, Container(CHEST, ITEM, 1));

	for (const PeerId peer : { 1, 2 }) {
		const auto changes = Decoded(Received(f, peer), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
		REQUIRE(changes.size() == 3);
		for (std::uint32_t i = 0; i < 3; ++i) {
			CHECK(changes[i].index == i);
			CHECK(changes[i].change.container == CHEST);
		}
		CHECK(changes[0].playerId == alice);
		CHECK(changes[0].change.item == ITEM);
		CHECK(changes[0].change.count == -3);
		CHECK(changes[1].playerId == alice);
		CHECK(changes[1].change.count == 2);
		CHECK(changes[2].playerId == bob);
	}
}

TEST("server world: invalid container changes are dropped and don't take a number")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });

	f.fake->Deliver(1, Container(CHEST, ITEM, 0));
	f.fake->Deliver(1, Container(CHEST, 0, 1));
	f.fake->Deliver(1, Container(RUNTIME, ITEM, 1));
	f.fake->Deliver(1, Container(0, ITEM, 1));
	f.fake->Deliver(1, Container(CHEST, ITEM, Protocol::MAX_ITEM_COUNT + 1));
	f.fake->Deliver(1, Container(CHEST, ITEM, -Protocol::MAX_ITEM_COUNT - 1));
	f.fake->Deliver(1, Container(CHEST, ITEM, Protocol::MAX_ITEM_COUNT));

	const auto changes = Decoded(Received(f, 1), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].index == 0);
	CHECK(changes[0].change.count == Protocol::MAX_ITEM_COUNT);
}

TEST("server world: a late joiner gets the container changes")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Container(CHEST, ITEM, -1));
	f.fake->Deliver(2, Container(CHEST, ITEM, 5));

	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 0);
	REQUIRE(world[0].containerChanges.size() == 2);
	CHECK(world[0].containerChanges[0].index == 0);
	CHECK(world[0].containerChanges[0].playerId == alice);
	CHECK(world[0].containerChanges[0].change.count == -1);
	CHECK(world[0].containerChanges[1].index == 1);
	CHECK(world[0].containerChanges[1].playerId == bob);
	CHECK(world[0].containerChanges[1].change.count == 5);
}

TEST("server world: a world from this session only gets the container changes it lacks")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	for (int i = 0; i < 5; ++i) {
		f.fake->Deliver(1, Container(CHEST, ITEM + i, 1));
	}

	auto hello = Fixture::MakeHello("Bob");
	hello.world = { welcome->sessionId, 3 };
	REQUIRE(f.Join(2, hello));
	auto world = WorldStates(Received(f, 2));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 3);
	REQUIRE(world[0].containerChanges.size() == 2);
	CHECK(world[0].containerChanges[0].index == 3);
	CHECK(world[0].containerChanges[0].change.item == ITEM + 3);
	CHECK(world[0].containerChanges[1].index == 4);
	CHECK(world[0].containerChanges[1].change.item == ITEM + 4);

	// Past the end (it can't know more than the server, but mustn't break it).
	hello = Fixture::MakeHello("Carol");
	hello.world = { welcome->sessionId, 100 };
	REQUIRE(f.Join(3, hello));
	world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 5);
	CHECK(world[0].containerChanges.empty());
}

TEST("server world: a world from another session gets every container change")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	for (int i = 0; i < 5; ++i) {
		f.fake->Deliver(1, Container(CHEST, ITEM + i, 1));
	}

	auto hello = Fixture::MakeHello("Bob");
	hello.world = { welcome->sessionId + 1, 3 };
	REQUIRE(f.Join(2, hello));
	const auto world = WorldStates(Received(f, 2));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 0);
	REQUIRE(world[0].containerChanges.size() == 5);
	CHECK(world[0].containerChanges[0].change.item == ITEM);
}

TEST("server world: a world request after loading a save follows the same rule")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	for (int i = 0; i < 5; ++i) {
		f.fake->Deliver(1, Container(CHEST, ITEM + i, 1));
	}
	f.fake->Deliver(1, Death(NPC));
	Clear(f, { 1 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 4 }));
	auto world = WorldStates(Received(f, 1));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 4);
	REQUIRE(world[0].containerChanges.size() == 1);
	CHECK(world[0].containerChanges[0].change.item == ITEM + 4);
	CHECK(world[0].deadActors == std::vector<std::uint32_t>{ NPC });

	f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ 12345, 4 }));
	world = WorldStates(Received(f, 1));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 0);
	CHECK(world[0].containerChanges.size() == 5);
}

TEST("server world: a world request past the end of the log answers with containerFirstIndex = log size and no changes")
{
	// A save from before the host's game stopped without writing its last changes asks from further on
	// than the server knows: the answer says where the log ends, which is how the client knows to count
	// from there (ContainerLedger::Ledger::OnChunk). The rest of the world comes as usual.
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	for (int i = 0; i < 3; ++i) {
		f.fake->Deliver(1, Container(CHEST, ITEM + i, 1));
	}
	f.fake->Deliver(1, Death(NPC));
	Clear(f, { 1 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 45 }));
	const auto world = WorldStates(Received(f, 1));
	REQUIRE(world.size() == 1);
	CHECK(world[0].containerFirstIndex == 3);
	CHECK(world[0].containerChanges.empty());
	CHECK(world[0].deadActors == std::vector<std::uint32_t>{ NPC });

	// The next change takes the first number past the end, which is where the client now counts from.
	f.fake->Deliver(1, Container(CHEST, ITEM, -1));
	const auto changes = Decoded(Received(f, 1), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].index == 3);
}

TEST("server world: a big world state is sent in chunks")
{
	// 4500 changes: more than one chunk. A player may report 500 events a second, so nine share it.
	constexpr std::size_t PLAYERS = 9;
	constexpr std::size_t TOTAL = Protocol::WORLD_STATE_CHUNK + 500;
	static_assert(TOTAL / PLAYERS <= 500);

	Server::Options options;
	options.maxPlayers = PLAYERS + 1;
	Fixture       f{ options };
	std::uint64_t session = 0;
	for (PeerId peer = 1; peer <= PLAYERS; ++peer) {
		const auto welcome = f.Join(peer, Fixture::MakeHello(std::format("P{}", peer)));
		REQUIRE(welcome);
		session = welcome->sessionId;
	}
	std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> packets;
	for (std::size_t i = 0; i < TOTAL; ++i) {
		packets.emplace_back(static_cast<PeerId>(1 + i % PLAYERS), Container(CHEST, static_cast<std::uint32_t>(ITEM + i), 1));
	}
	const auto watched = Burst(f, packets, { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, 1, MT::kContainerChanged, TOTAL);
	REQUIRE(Count(watched, MT::kContainerChanged) == TOTAL);

	// Every list is chunked alike; check the container changes line up across chunks.
	const auto check = [&](const std::vector<Protocol::WorldState>& a_world, std::size_t a_from) {
		REQUIRE(a_world.size() == 2);
		CHECK(a_world[0].containerFirstIndex == a_from);
		CHECK(a_world[0].containerChanges.size() == Protocol::WORLD_STATE_CHUNK);
		CHECK(a_world[1].containerFirstIndex == a_from + Protocol::WORLD_STATE_CHUNK);
		CHECK(a_world[1].containerChanges.size() == TOTAL - a_from - Protocol::WORLD_STATE_CHUNK);
		for (const auto& chunk : a_world) {
			for (const auto& change : chunk.containerChanges) {
				if (change.change.item != ITEM + change.index) {
					CHECK(change.change.item == ITEM + change.index);
					return;
				}
			}
		}
	};

	REQUIRE(f.Join(10, Fixture::MakeHello("Late")));
	check(WorldStates(Received(f, 10)), 0);

	f.fake->Deliver(10, Protocol::Encode(Protocol::WorldRequest{ session, 300 }));
	check(WorldStates(Received(f, 10)), 300);

	// From well into the list it fits one chunk again.
	f.fake->Deliver(10, Protocol::Encode(Protocol::WorldRequest{ session, 1000 }));
	const auto one = WorldStates(Received(f, 10));
	REQUIRE(one.size() == 1);
	CHECK(one[0].containerFirstIndex == 1000);
	CHECK(one[0].containerChanges.size() == TOTAL - 1000);
}

// ---- pickups and doors ------------------------------------------------------------------

TEST("server world: a pickup is relayed once and late joiners get it")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Pickup(ITEM));
	const auto pickups = Decoded(Received(f, 2), MT::kRefPickedUp, Protocol::DecodeRefPickedUp);
	REQUIRE(pickups.size() == 1);
	CHECK(pickups[0].refId == ITEM);
	CHECK(Count(Received(f, 1), MT::kRefPickedUp) == 0);

	f.fake->Deliver(2, Pickup(ITEM));
	f.fake->Deliver(1, Pickup(RUNTIME));
	CHECK(Count(Received(f, 1), MT::kRefPickedUp) == 0);
	CHECK(Count(Received(f, 2), MT::kRefPickedUp) == 0);

	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(world[0].pickedUp == std::vector<std::uint32_t>{ ITEM });
}

TEST("server world: door states are relayed when they change, and the latest is kept")
{
	using RS = Protocol::RefState;
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Door(DOOR, RS::kYes, RS::kNo));
	auto states = Decoded(Received(f, 2), MT::kRefStateChanged, Protocol::DecodeRefState);
	REQUIRE(states.size() == 1);
	CHECK(states[0].SameState(RS{ DOOR, RS::kYes, RS::kNo }));
	CHECK(Count(Received(f, 1), MT::kRefStateChanged) == 0);

	// No change: nothing to pass on.
	f.fake->Deliver(1, Door(DOOR, RS::kYes, RS::kNo));
	f.fake->Deliver(1, Door(RUNTIME, RS::kYes, RS::kNo));
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);

	f.fake->Deliver(2, Door(DOOR, RS::kNo, RS::kYes));
	states = Decoded(Received(f, 1), MT::kRefStateChanged, Protocol::DecodeRefState);
	REQUIRE(states.size() == 1);
	CHECK(states[0].SameState(RS{ DOOR, RS::kNo, RS::kYes }));

	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	REQUIRE(world[0].refStates.size() == 1);
	CHECK((world[0].refStates[0] == RS{ DOOR, RS::kNo, RS::kYes }));
}

// ---- quests -----------------------------------------------------------------------------

TEST("server world: quest stages are relayed once, stamped with who did them, and late joiners get them in order")
{
	Fixture f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Stage(QUEST, 10));
	f.fake->Deliver(2, Stage(QUEST2, 5));
	f.fake->Deliver(1, Protocol::Encode(Protocol::QuestStage{ QUEST, 20, bob }, MT::kReportQuestStage));  // claims Bob did it
	f.fake->Deliver(2, Stage(QUEST, 10));  // already known
	f.fake->Deliver(1, Stage(QUEST, 20));
	f.fake->Deliver(1, Stage(RUNTIME, 1));
	f.fake->Deliver(1, Stage(0, 1));

	// The sender's id is stamped by the server, whatever the sender said: a stage that moves the
	// player is only followed by a friend standing with the one who did it.
	const auto toBob = Decoded(Received(f, 2), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((toBob == std::vector<Protocol::QuestStage>{ { QUEST, 10, alice }, { QUEST, 20, alice } }));
	const auto toAlice = Decoded(Received(f, 1), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((toAlice == std::vector<Protocol::QuestStage>{ { QUEST2, 5, bob } }));

	// Caught up from the session, nobody is "with" the one who did it: no id.
	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK((world[0].questStages == std::vector<Protocol::QuestStage>{ { QUEST, 10 }, { QUEST2, 5 }, { QUEST, 20 } }));
}

// The server doesn't order stages: it keeps every distinct stage it hears of, in the order it
// heard them, and each game decides (QuestSync only ever moves a quest forward). Quest stages
// needn't be set in numeric order, so an earlier number isn't necessarily stale.
TEST("server world: a lower stage of a quest is still passed on")
{
	Fixture f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Stage(QUEST, 20));
	f.fake->Deliver(1, Stage(QUEST, 10));
	const auto stages = Decoded(Received(f, 2), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((stages == std::vector<Protocol::QuestStage>{ { QUEST, 20, alice }, { QUEST, 10, alice } }));
}

TEST("server world: a completed quest is relayed with the sender's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::QuestDone{ 999, QUEST }, MT::kReportQuestDone));
	f.fake->Deliver(1, Protocol::Encode(Protocol::QuestDone{ 0, RUNTIME }, MT::kReportQuestDone));
	const auto done = Decoded(Received(f, 2), MT::kQuestDone, Protocol::DecodeQuestDone);
	REQUIRE(done.size() == 1);
	CHECK(done[0].playerId == alice);
	CHECK(done[0].quest == QUEST);
	CHECK(Count(Received(f, 1), MT::kQuestDone) == 0);
}

// ---- NPC ownership ----------------------------------------------------------------------

TEST("server world: the first claim on an NPC wins and everyone is told")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Claim(NPC));
	const auto toAlice = Owners(Received(f, 1));
	CHECK(toAlice.size() == 1);
	CHECK(HasOwner(toAlice, NPC, alice));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, alice));

	// Claiming it again changes nothing.
	f.fake->Deliver(1, Claim(NPC));
	CHECK(Owners(Received(f, 1)).empty());
	CHECK(Owners(Received(f, 2)).empty());
}

TEST("server world: a claim on someone else's NPC is answered with its owner, to the claimer only")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Claim(NPC));
	f.fake->Deliver(2, Claim(NPC, Reason::kInteract));  // moments after Alice got it
	const auto toBob = Owners(Received(f, 2));
	REQUIRE(toBob.size() == 2);
	CHECK(HasOwner(toBob, NPC, alice));
	CHECK(std::ranges::all_of(toBob, [&](const auto& a_owner) { return a_owner.playerId == alice; }));
	CHECK(Owners(Received(f, 1)).empty());
}

TEST("server world: fighting another player's NPC takes it over after the cooldown")
{
	Fixture    f;
	JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	std::this_thread::sleep_for(5100ms);  // TAKEOVER_COOLDOWN
	f.fake->Deliver(2, Claim(NPC, Reason::kInteract));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, bob));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, bob));

	// And it doesn't trade straight back.
	f.fake->Deliver(1, Claim(NPC, Reason::kInteract));
	CHECK(Owners(Received(f, 2)).empty());
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, bob));
}

TEST("server world: a companion goes to its player, but not away from another's companion")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	f.fake->Deliver(1, Claim(NPC2));
	Clear(f, { 1, 2 });

	// A companion claim takes it at once, cooldown or not.
	f.fake->Deliver(2, Claim(NPC, Reason::kCompanion));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, bob));
	Clear(f, { 2 });

	// ...but not from someone it's a companion of.
	f.fake->Deliver(1, Claim(NPC, Reason::kCompanion));
	f.fake->Deliver(1, Claim(NPC, Reason::kInteract));
	CHECK(Owners(Received(f, 2)).empty());
	const auto toAlice = Owners(Received(f, 1));
	CHECK(!toAlice.empty() && std::ranges::all_of(toAlice, [&](const auto& a_owner) { return a_owner.playerId == bob; }));

	// The owner's own companion claim upgrades its hold.
	f.fake->Deliver(1, Claim(NPC2, Reason::kCompanion));
	CHECK(Owners(Received(f, 1)).empty());
	f.fake->Deliver(2, Claim(NPC2, Reason::kCompanion));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC2, alice));
	CHECK(Owners(Received(f, 1)).empty());
}

TEST("server world: only the owner can release an NPC")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Release(NPC));
	CHECK(Owners(Received(f, 1)).empty());

	f.fake->Deliver(1, Release(NPC));
	const auto toBob = Owners(Received(f, 2));
	CHECK(toBob.size() == 1);
	CHECK(HasOwner(toBob, NPC, 0));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, 0));

	// Free again: the next claim gets it.
	f.fake->Deliver(2, Claim(NPC));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, bob));
	CHECK(!HasOwner(Owners(Received(f, 2)), NPC, alice));
}

TEST("server world: runtime references and the player can't be claimed")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	f.fake->Deliver(1, Claim(std::vector<Protocol::ActorClaim>{ { RUNTIME, Reason::kUnowned }, { Protocol::PLAYER_REF_ID, Reason::kCompanion }, { 0, Reason::kUnowned } }));
	CHECK(Owners(Received(f, 1)).empty());
}

TEST("server world: a newcomer is told who runs which NPC")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	// More than one packet's worth.
	std::vector<Protocol::ActorClaim> claims;
	for (std::uint32_t i = 0; i < Protocol::MAX_ACTORS_PER_PACKET; ++i) {
		claims.push_back({ 0x00100000 + i, Reason::kUnowned });
	}
	f.fake->Deliver(1, Claim(claims));
	f.fake->Deliver(2, Claim(NPC));
	f.fake->Deliver(2, Claim(NPC2));

	JoinAs(f, 3, "Carol");
	const auto packets = Received(f, 3);
	CHECK(Count(packets, MT::kActorOwners) == 2);
	const auto owners = Owners(packets);
	CHECK(owners.size() == Protocol::MAX_ACTORS_PER_PACKET + 2);
	CHECK(HasOwner(owners, 0x00100000, alice));
	CHECK(HasOwner(owners, 0x00100000 + Protocol::MAX_ACTORS_PER_PACKET - 1, alice));
	CHECK(HasOwner(owners, NPC, bob));
	CHECK(HasOwner(owners, NPC2, bob));
}

TEST("server world: an owner who leaves frees their NPCs for everyone")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	f.fake->Deliver(1, Claim(NPC));
	f.fake->Deliver(1, Claim(NPC2));
	f.fake->Deliver(2, Claim(NPC3));
	Clear(f, { 1, 2, 3 });

	f.fake->Drop(1);
	for (const PeerId peer : { 2, 3 }) {
		const auto packets = Received(f, peer);
		CHECK(Count(packets, MT::kPlayerLeft) == 1);
		const auto owners = Owners(packets);
		CHECK(owners.size() == 2);
		CHECK(HasOwner(owners, NPC, 0));
		CHECK(HasOwner(owners, NPC2, 0));
	}

	JoinAs(f, 4, "Dave");
	const auto owners = Owners(Received(f, 4));
	CHECK(owners.size() == 1);
	CHECK(!HasOwner(owners, NPC, alice));
	CHECK(HasOwner(owners, NPC3, bob));
}

TEST("server world: rejoining frees the old connection's NPCs")
{
	Fixture    f;
	const auto bob = JoinAs(f, 1, "Bob", 0xB0B);
	const auto alice = JoinAs(f, 2, "Alice", 0xA11CE);
	f.fake->Deliver(2, Claim(NPC));
	f.fake->Deliver(1, Claim(NPC2));
	Clear(f, { 1, 2 });

	// Alice's game restarted before the old connection timed out.
	const auto again = JoinAs(f, 3, "Alice", 0xA11CE);
	CHECK(again == alice);
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, 0));
	const auto owners = Owners(Received(f, 3));
	CHECK(!HasOwner(owners, NPC, alice));
	CHECK(HasOwner(owners, NPC2, bob));

	// The new connection can run it again.
	f.fake->Deliver(3, Claim(NPC));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, alice));
}

TEST("server world: hello again on the same connection frees that player's NPCs")
{
	Fixture f;
	JoinAs(f, 1, "Bob", 0xB0B);
	JoinAs(f, 2, "Alice", 0xA11CE);
	f.fake->Deliver(2, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWelcome)));
	CHECK(HasOwner(Owners(Received(f, 1)), NPC, 0));
}

TEST("server world: NPC positions are relayed only from their owner")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Positions({ NPC, NPC2 }));  // NPC2: nobody's
	const auto relayed = Decoded(Received(f, 2), MT::kActorStatesRelay, [](auto a_data) { return Protocol::DecodeActorStates(a_data); });
	REQUIRE(relayed.size() == 1);
	REQUIRE(relayed[0].size() == 1);
	CHECK(relayed[0][0].refId == NPC);
	CHECK(relayed[0][0].x == 1.0f);
	CHECK(Count(Received(f, 1), MT::kActorStatesRelay) == 0);

	// Bob's view of Alice's NPC doesn't count; nothing left means nothing sent.
	f.fake->Deliver(2, Positions({ NPC }));
	f.fake->Deliver(1, Positions({ NPC2 }));
	CHECK(Count(Received(f, 1), MT::kActorStatesRelay) == 0);
	CHECK(Count(Received(f, 2), MT::kActorStatesRelay) == 0);
}

TEST("server world: NPC position batches are capped per second")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Claim(NPC));
	for (int i = 0; i < 130; ++i) {
		f.fake->Deliver(1, Positions({ NPC }));
	}
	CHECK(Count(Received(f, 2), MT::kActorStatesRelay) == 120);  // MAX_ACTOR_BATCHES_PER_SECOND
}

TEST("server world: NPC ownership is capped per player")
{
	constexpr std::size_t CAP = 2 * Protocol::MAX_OWNED_PER_PLAYER;  // two players
	constexpr std::size_t PACKETS = (CAP + Protocol::MAX_ACTORS_PER_PACKET - 1) / Protocol::MAX_ACTORS_PER_PACKET + 2;
	Fixture               f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// Two players, so neither goes over 500 events a second.
	std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> packets;
	std::uint32_t                                            ref = 0x00100000;
	for (std::size_t p = 0; p < PACKETS; ++p) {
		std::vector<Protocol::ActorClaim> claims;
		for (std::size_t i = 0; i < Protocol::MAX_ACTORS_PER_PACKET; ++i) {
			claims.push_back({ ref++, Reason::kUnowned });
		}
		packets.emplace_back(static_cast<PeerId>(1 + p % 2), Claim(claims));
	}
	const auto watched = Burst(f, packets, { 1, 2 }, 1, MT::kActorOwners, CAP / Protocol::MAX_ACTORS_PER_PACKET + 1);
	std::this_thread::sleep_for(200ms);
	auto all = watched;
	std::ranges::move(Received(f, 1), std::back_inserter(all));
	Clear(f, { 2 });
	CHECK(Owners(all).size() == CAP);

	// Full: a new NPC isn't granted, not even as a companion.
	f.fake->Deliver(1, Claim(0x00F00000, Reason::kCompanion));
	CHECK(Owners(Received(f, 1)).empty());

	// A death makes room for its runner.
	f.fake->Deliver(1, Death(0x00100000));
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Claim(0x00F00000));
	CHECK(Owners(Received(f, 2)).size() == 1);
}

// ---- hits -------------------------------------------------------------------------------

TEST("server world: an NPC's hit reaches its victim as damage from the reporter")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(1, Hit(bob, 12.5f, false, NPC));
	const auto damage = Decoded(Received(f, 2), MT::kPlayerDamaged, Protocol::DecodePlayerHit);
	REQUIRE(damage.size() == 1);
	CHECK(damage[0].playerId == alice);
	CHECK(damage[0].damage == 12.5f);
	CHECK(!damage[0].byPlayer);
	CHECK(damage[0].attacker == NPC);
	CHECK(Count(Received(f, 1), MT::kPlayerDamaged) == 0);
	CHECK(Count(Received(f, 3), MT::kPlayerDamaged) == 0);
}

TEST("server world: hits by players only count with friendly fire")
{
	{
		Fixture    f;
		JoinAs(f, 1, "Alice");
		const auto bob = JoinAs(f, 2, "Bob");
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Hit(bob, 10.0f, true, 0));
		CHECK(Count(Received(f, 2), MT::kPlayerDamaged) == 0);
	}
	{
		Server::Options options;
		options.friendlyFire = true;
		Fixture    f{ options };
		const auto alice = JoinAs(f, 1, "Alice");
		const auto bob = JoinAs(f, 2, "Bob");
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Hit(bob, 10.0f, true, 0));
		const auto damage = Decoded(Received(f, 2), MT::kPlayerDamaged, Protocol::DecodePlayerHit);
		REQUIRE(damage.size() == 1);
		CHECK(damage[0].playerId == alice);
		CHECK(damage[0].byPlayer);
	}
}

TEST("server world: hits on unknown players or yourself are dropped")
{
	Server::Options options;
	options.friendlyFire = true;
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Hit(alice, 10.0f, false, NPC));
	f.fake->Deliver(1, Hit(alice, 10.0f, true, 0));
	f.fake->Deliver(1, Hit(4242, 10.0f, false, NPC));
	f.fake->Deliver(1, Hit(0, 10.0f, false, NPC));
	CHECK(Count(Received(f, 1), MT::kPlayerDamaged) == 0);
	CHECK(Count(Received(f, 2), MT::kPlayerDamaged) == 0);
}

TEST("server world: a revive reaches the downed player with the helper's ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::Revive{ bob }, MT::kRevive));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Revive{ alice }, MT::kRevive));
	const auto revived = Decoded(Received(f, 2), MT::kRevived, Protocol::DecodeRevive);
	REQUIRE(revived.size() == 1);
	CHECK(revived[0].playerId == alice);
	CHECK(Count(Received(f, 1), MT::kRevived) == 0);
	CHECK(Count(Received(f, 3), MT::kRevived) == 0);
}

// ---- relays -----------------------------------------------------------------------------

TEST("server world: shots are relayed with the shooter's ID, NPC shots only from its owner")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 999, 0 }, MT::kReportShot));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 999, NPC }, MT::kReportShot));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 999, NPC2 }, MT::kReportShot));  // nobody's
	const auto shots = Decoded(Received(f, 2), MT::kShotFired, Protocol::DecodeShot);
	REQUIRE(shots.size() == 2);
	CHECK(shots[0].playerId == alice);
	CHECK(shots[0].refId == 0);
	CHECK(shots[1].playerId == alice);
	CHECK(shots[1].refId == NPC);
	CHECK(Count(Received(f, 1), MT::kShotFired) == 0);

	f.fake->Deliver(2, Protocol::Encode(Protocol::Shot{ 0, NPC }, MT::kReportShot));  // Alice's NPC
	CHECK(Count(Received(f, 1), MT::kShotFired) == 0);
}

TEST("server world: a shot is relayed with the shooter's gun sound")
{
	// Each game's copy of a gun lacks the shooter's mods, so only the shooter's game knows which
	// sound it makes (a semi-automatic pistol's single shot, not an automatic receiver's loop).
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kShot, 0x0012A3B4 }, MT::kReportShot));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 0, NPC, Protocol::ShotAction::kShot, 0x0004F00D }, MT::kReportShot));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 0, 0 }, MT::kReportShot));
	const auto shots = Decoded(Received(f, 2), MT::kShotFired, Protocol::DecodeShot);
	REQUIRE(shots.size() == 3);
	CHECK(shots[0].sound == 0x0012A3B4);
	CHECK(shots[1].refId == NPC);
	CHECK(shots[1].sound == 0x0004F00D);
	CHECK(shots[2].sound == 0);
}

TEST("server world: shots are capped per second")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (int i = 0; i < 70; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::Shot{ 0, 0 }, MT::kReportShot));
	}
	CHECK(Count(Received(f, 2), MT::kShotFired) == 60);  // MAX_SHOTS_PER_SECOND
}

TEST("server world: lines are relayed with the speaker's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::Line{ 999, NPC, "Hello there" }, MT::kReportLine));
	const auto lines = Decoded(Received(f, 2), MT::kLineSpoken, Protocol::DecodeLine);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].playerId == alice);
	CHECK(lines[0].speaker == NPC);
	CHECK(lines[0].text == "Hello there");
	CHECK(Count(Received(f, 1), MT::kLineSpoken) == 0);
}

TEST("server world: voice is relayed with the speaker's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::VoiceData{ 999, { 1, 2, 3 } }, MT::kVoice));
	const auto voice = Decoded(Received(f, 2), MT::kVoiceRelay, Protocol::DecodeVoice);
	REQUIRE(voice.size() == 1);
	CHECK(voice[0].playerId == alice);
	CHECK((voice[0].data == std::vector<std::uint8_t>{ 1, 2, 3 }));
	CHECK(Count(Received(f, 1), MT::kVoiceRelay) == 0);
}

TEST("server world: pings are relayed with the pinger's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::Ping{ 999, 0, 0x3C, 1.0f, 2.0f, 3.0f }, MT::kPing));
	const auto pings = Decoded(Received(f, 2), MT::kPinged, Protocol::DecodePing);
	REQUIRE(pings.size() == 1);
	CHECK(pings[0].playerId == alice);
	CHECK(pings[0].worldspace == 0x3C);
	CHECK(pings[0].y == 2.0f);
	CHECK(Count(Received(f, 1), MT::kPinged) == 0);
}

TEST("server world: only new map markers are relayed, and late joiners get them all")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Markers({ { 0x00A00001, 1 }, { 0x00A00002, 3 } }));
	auto found = Decoded(Received(f, 2), MT::kMarkersFound, Protocol::DecodeMarkers);
	REQUIRE(found.size() == 1);
	CHECK(found[0].playerId == alice);
	CHECK(found[0].markers.size() == 2);
	CHECK(Count(Received(f, 1), MT::kMarkersFound) == 0);

	// Known already: nothing. A new flag: passed on, merged with what's known.
	f.fake->Deliver(2, Markers({ { 0x00A00001, 1 }, { 0x00A00002, 2 } }));
	CHECK(Count(Received(f, 1), MT::kMarkersFound) == 0);
	f.fake->Deliver(2, Markers({ { 0x00A00001, 2 } }));
	found = Decoded(Received(f, 1), MT::kMarkersFound, Protocol::DecodeMarkers);
	REQUIRE(found.size() == 1);
	REQUIRE(found[0].markers.size() == 1);
	CHECK(found[0].markers[0].refId == 0x00A00001);
	CHECK(found[0].markers[0].flags == 3);

	JoinAs(f, 3, "Carol");
	found = Decoded(Received(f, 3), MT::kMarkersFound, Protocol::DecodeMarkers);
	REQUIRE(found.size() == 1);
	CHECK(found[0].playerId == 0);
	REQUIRE(found[0].markers.size() == 2);
	CHECK(found[0].markers[0].flags == 3);
	CHECK(found[0].markers[1].flags == 3);
}

TEST("server world: map markers are capped at MAX_SESSION_MARKERS")
{
	constexpr std::size_t PACKETS = Protocol::MAX_SESSION_MARKERS / Protocol::MAX_MARKERS_PER_PACKET + 2;
	Fixture               f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	std::uint32_t ref = 0x00A00000;
	for (std::size_t p = 0; p < PACKETS; ++p) {
		std::vector<Protocol::MapMarker> markers;
		for (std::size_t i = 0; i < Protocol::MAX_MARKERS_PER_PACKET; ++i) {
			markers.push_back({ ref++, 1 });
		}
		f.fake->Deliver(1, Markers(std::move(markers)));
	}
	std::size_t relayed = 0;
	for (const auto& found : Decoded(Received(f, 2), MT::kMarkersFound, Protocol::DecodeMarkers)) {
		relayed += found.markers.size();
	}
	CHECK(relayed == Protocol::MAX_SESSION_MARKERS);

	// Known ones still gain flags; new ones are ignored.
	f.fake->Deliver(1, Markers({ { 0x00A00000, 2 }, { 0x00F00000, 3 } }));
	const auto found = Decoded(Received(f, 2), MT::kMarkersFound, Protocol::DecodeMarkers);
	REQUIRE(found.size() == 1);
	REQUIRE(found[0].markers.size() == 1);
	CHECK(found[0].markers[0].refId == 0x00A00000);

	JoinAs(f, 3, "Carol");
	const auto caughtUp = Decoded(Received(f, 3), MT::kMarkersFound, Protocol::DecodeMarkers);
	CHECK(caughtUp.size() == (Protocol::MAX_SESSION_MARKERS + Protocol::MAX_MARKERS_PER_PACKET - 1) / Protocol::MAX_MARKERS_PER_PACKET);
	std::size_t total = 0;
	for (const auto& batch : caughtUp) {
		CHECK(batch.playerId == 0);
		total += batch.markers.size();
	}
	CHECK(total == Protocol::MAX_SESSION_MARKERS);
}

TEST("server world: a newcomer sees the others' equipment and status")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Protocol::Encode(Protocol::Equipment{ 999, { 0x1234, 0x5678 } }, MT::kEquipment));
	Protocol::PlayerStatus status;
	status.playerId = 999;
	status.health = 80;
	status.level = 12;
	f.fake->Deliver(1, Protocol::Encode(status, MT::kReportStatus));

	JoinAs(f, 2, "Bob");
	const auto packets = Received(f, 2);
	const auto equipment = Decoded(packets, MT::kPlayerEquipment, Protocol::DecodeEquipment);
	REQUIRE(equipment.size() == 1);
	CHECK(equipment[0].playerId == alice);
	CHECK((equipment[0].items == std::vector<std::uint32_t>{ 0x1234, 0x5678 }));
	const auto statuses = Decoded(packets, MT::kPlayerStatus, Protocol::DecodePlayerStatus);
	REQUIRE(statuses.size() == 1);
	CHECK(statuses[0].playerId == alice);
	CHECK(statuses[0].level == 12);
}

// ---- the session ------------------------------------------------------------------------

TEST("server world: the world outlives its players")
{
	Fixture    f;
	const auto first = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(first);
	f.fake->Deliver(1, Death(NPC));
	f.fake->Deliver(1, Stage(QUEST, 10));
	f.fake->Deliver(1, Claim(NPC2));
	Sync(f, 1);
	f.fake->Drop(1);

	const auto second = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(second);
	CHECK(second->sessionId == first->sessionId);
	const auto packets = Received(f, 2);
	const auto world = WorldStates(packets);
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors == std::vector<std::uint32_t>{ NPC });
	CHECK(world[0].questStages.size() == 1);
	CHECK(Owners(packets).empty());  // Alice's NPC went with her
}

TEST("server world: a different load order starts a fresh world")
{
	Fixture    f;
	const auto first = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(first);
	f.fake->Deliver(1, Death(NPC));
	f.fake->Deliver(1, Container(CHEST, ITEM, 1));
	f.fake->Deliver(1, Pickup(ITEM));
	f.fake->Deliver(1, Door(DOOR, 1, 1));
	f.fake->Deliver(1, Stage(QUEST, 10));
	f.fake->Deliver(1, Markers({ { 0x00A00001, 1 } }));
	Sync(f, 1);
	f.fake->Drop(1);

	auto hello = Fixture::MakeHello("Bob");
	hello.contentHash = 0x5678;
	hello.world = { first->sessionId, 1 };
	const auto second = f.Join(2, hello);
	REQUIRE(second);
	CHECK(second->sessionId != first->sessionId);
	const auto packets = Received(f, 2);
	CHECK(Count(packets, MT::kMarkersFound) == 0);
	const auto world = WorldStates(packets);
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.empty());
	CHECK(world[0].containerFirstIndex == 0);
	CHECK(world[0].containerChanges.empty());
	CHECK(world[0].pickedUp.empty());
	CHECK(world[0].refStates.empty());
	CHECK(world[0].questStages.empty());

	// The numbering starts over.
	f.fake->Deliver(2, Container(CHEST, ITEM, 1));
	const auto changes = Decoded(Received(f, 2), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].index == 0);
}

// ---- workshop building --------------------------------------------------------------------

namespace
{
	Protocol::WorkshopItem Wall(std::uint32_t a_ref, float a_x = 10.0f)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.base = 0x0001F2A1;
		item.workshop = 0x000250FE;
		item.position[0] = a_x;
		item.rotation[2] = 1.5f;
		return item;
	}

	std::vector<std::uint8_t> Built(const Protocol::WorkshopItem& a_item)
	{
		return Protocol::Encode(a_item, MT::kReportWorkshopItem);
	}
}

TEST("server workshop: a placed object is relayed to the others with the builder's player ID")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	auto wall = Wall(0xFF001234);
	wall.playerId = 999;  // ignored
	f.fake->Deliver(1, Built(wall));
	const auto items = Decoded(Received(f, 2), MT::kWorkshopItem, Protocol::DecodeWorkshopItem);
	REQUIRE(items.size() == 1);
	CHECK(items[0].playerId == alice);
	CHECK(items[0].refId == 0xFF001234);
	CHECK(items[0].base == 0x0001F2A1);
	CHECK(items[0].position[0] == 10.0f);
	CHECK(Count(Received(f, 1), MT::kWorkshopItem) == 0);  // not echoed to the builder
}

TEST("server workshop: a late joiner gets everything built so far, moved objects once, scrapped ones not at all")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Built(Wall(0xFF000001, 10.0f)));
	f.fake->Deliver(1, Built(Wall(0xFF000001, 20.0f)));  // moved
	f.fake->Deliver(1, Built(Wall(0xFF000002)));
	auto scrapped = Wall(0xFF000002);
	scrapped.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(scrapped));
	Clear(f, { 1 });

	JoinAs(f, 2, "Bob");
	const auto batches = Decoded(Received(f, 2), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(batches.size() == 1);
	const auto& items = batches[0];
	REQUIRE(items.size() == 1);
	CHECK(items[0].refId == 0xFF000001);
	CHECK(items[0].position[0] == 20.0f);
}

TEST("server workshop: scrapping a piece of the world itself is relayed, and kept for late joiners")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	auto junk = Wall(0x0001A2B3);
	junk.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(junk));
	const auto items = Decoded(Received(f, 2), MT::kWorkshopItem, Protocol::DecodeWorkshopItem);
	REQUIRE(items.size() == 1);
	CHECK(items[0].refId == 0x0001A2B3);
	CHECK(items[0].op == Protocol::WorkshopOp::kScrapped);
	Clear(f, { 1, 2 });

	JoinAs(f, 3, "Carol");
	const auto batches = Decoded(Received(f, 3), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0].size() == 1);
	CHECK(batches[0][0].refId == 0x0001A2B3);
	CHECK(batches[0][0].op == Protocol::WorkshopOp::kScrapped);
}

TEST("server workshop: a piece of the world reported scrapped again stays scrapped, and isn't relayed twice")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// Alice's game reports the junk her save had scrapped, then again (it hosts again, or reconnects).
	auto junk = Wall(0x0001A2B3);
	junk.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(junk));
	f.fake->Deliver(1, Built(junk));
	CHECK(Decoded(Received(f, 2), MT::kWorkshopItem, Protocol::DecodeWorkshopItem).size() == 1);
	Clear(f, { 1, 2 });

	JoinAs(f, 3, "Carol");
	const auto batches = Decoded(Received(f, 3), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0].size() == 1);
	CHECK(batches[0][0].refId == 0x0001A2B3);
	CHECK(batches[0][0].op == Protocol::WorkshopOp::kScrapped);
}

TEST("server workshop: scrapping something nobody knew about is not relayed")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	auto scrapped = Wall(0xFF000009);
	scrapped.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(scrapped));
	CHECK(Count(Received(f, 2), MT::kWorkshopItem) == 0);
}

TEST("server workshop: a piece of the world moved and then scrapped reaches a late joiner scrapped")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Built(Wall(0x0001A2B3, 20.0f)));  // moved: Bob gets a copy where it went
	auto junk = Wall(0x0001A2B3, 20.0f);
	junk.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(junk));
	const auto items = Decoded(Received(f, 2), MT::kWorkshopItem, Protocol::DecodeWorkshopItem);
	REQUIRE(items.size() == 2);
	CHECK(items[1].op == Protocol::WorkshopOp::kScrapped);
	Clear(f, { 1, 2 });

	// Carol gets neither the copy nor the original.
	JoinAs(f, 3, "Carol");
	const auto batches = Decoded(Received(f, 3), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0].size() == 1);
	CHECK(batches[0][0].refId == 0x0001A2B3);
	CHECK(batches[0][0].op == Protocol::WorkshopOp::kScrapped);
}

// A friend who loads a save lost the copies their game had made since (they went with the world before
// the load), so the world request a load sends brings the building back too; but it shares the world
// state's budget, so a client can't make the host send its settlements over and over.
TEST("server workshop: a world request after a load sends everything the others built, but only a few times in a row")
{
	Server::Options options;
	options.worldStateRefill = std::chrono::milliseconds{ 500 };
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	f.fake->Deliver(1, Built(Wall(0xFF000001)));
	f.fake->Deliver(2, Built(Wall(0xFF00B0B1, 50.0f)));
	Clear(f, { 1, 2 });

	const auto batchesFor = [&](PeerId a_peer) {
		f.fake->Deliver(a_peer, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
		return Decoded(Received(f, a_peer), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	};
	auto batches = batchesFor(2);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0].size() == 1);  // not Bob's own
	CHECK(batches[0][0].refId == 0xFF000001);
	CHECK(batches[0][0].playerId == alice);

	batches = batchesFor(1);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0].size() == 1);
	CHECK(batches[0][0].refId == 0xFF00B0B1);

	CHECK(batchesFor(2).size() == 1);  // with his join, the third world state his budget holds
	// The next waits for his budget with the world state it goes with, and asking again meanwhile adds nothing.
	CHECK(batchesFor(2).empty());
	CHECK(batchesFor(2).empty());
	std::vector<std::vector<std::uint8_t>> later;
	REQUIRE(Await(f, 2, MT::kWorkshopItems, later));
	std::ranges::move(Received(f, 2), std::back_inserter(later));
	const auto owed = Decoded(later, MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(owed.size() == 1);  // once, for both requests
	REQUIRE(owed[0].size() == 1);
	CHECK(owed[0][0].refId == 0xFF000001);
	CHECK(Count(later, MT::kWorldState) == 1);
}

TEST("server workshop: a friend who loads a save from another session, or from before they ever joined, still gets the session's building sent")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	f.fake->Deliver(1, Built(Wall(0xFF000001)));
	Clear(f, { 1, 2 });

	// The building went with the world his game had before the load, whatever session that save was
	// made in: the copies it holds are only found again when the building is sent.
	for (const auto session : { welcome->sessionId ^ std::uint64_t{ 0x5A5A5A5A }, std::uint64_t{ 0 } }) {
		f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ session, 0 }));
		const auto batches = Decoded(Received(f, 2), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
		REQUIRE(batches.size() == 1);
		REQUIRE(batches[0].size() == 1);
		CHECK(batches[0][0].refId == 0xFF000001);
		CHECK(batches[0][0].playerId == alice);
	}
}

// A game that loads a save knows none of the others' radiant runs then: the others are told (kPlayerReloaded), so their
// games send their runs going again. Not over and over, for a game asking without pause.
TEST("server: a friend who loads a save has the others told, once in a while, so their games send their radiant runs again")
{
	Fixture    f;
	JoinAs(f, 1, "Alice");
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(welcome);
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
	CHECK(Count(Received(f, 2), MT::kPlayerReloaded) == 0);  // not Bob himself
	const auto told = Decoded(Received(f, 1), MT::kPlayerReloaded, Protocol::DecodePlayerReloaded);
	REQUIRE(told.size() == 1);
	CHECK(told[0].playerId == welcome->playerId);

	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ welcome->sessionId, 0 }));
	Received(f, 2);
	CHECK(Count(Received(f, 1), MT::kPlayerReloaded) == 0);  // asked again at once
}

// ---- the saved session --------------------------------------------------------------------

TEST("server session file: the world survives a server restart with the same load order")
{
	const TempSessionFile file;
	const auto            path = file.path.string();
	Server::Options options;
	options.sessionFile = path;
	std::uint64_t firstSession = 0;
	{
		Fixture    f{ options };
		const auto alice = f.Join(1, Fixture::MakeHello("Alice", 0xA11CE));
		REQUIRE(alice);
		CHECK(alice->playerId == 1);
		firstSession = alice->sessionId;
		f.fake->Deliver(1, Death(NPC));
		f.fake->Deliver(1, Built(Wall(0xFF000001)));
		Clear(f, { 1 });
	}  // stopping saves

	{
		Fixture    f{ options };
		const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
		REQUIRE(bob);
		CHECK(bob->sessionId == firstSession);  // the same session carries on
		CHECK(bob->playerId != 1);              // Alice's ID stays hers
		CHECK(f.Logged("loaded the saved session"));
		const auto received = Received(f, 2);
		const auto world = WorldStates(received);
		REQUIRE(world.size() == 1);
		CHECK((std::set<std::uint32_t>(world[0].deadActors.begin(), world[0].deadActors.end()) == std::set<std::uint32_t>{ NPC }));
		const auto batches = Decoded(received, MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
		REQUIRE(batches.size() == 1);
		CHECK(batches[0].size() == 1);
		// Alice comes back as player 1, and is not sent her own wall.
		const auto again = f.Join(3, Fixture::MakeHello("Alice", 0xA11CE));
		REQUIRE(again);
		CHECK(again->playerId == 1);
		CHECK(Decoded(Received(f, 3), MT::kWorkshopItems, Protocol::DecodeWorkshopItems).empty());
	}

	{
		// A different load order starts fresh, as it would have mid-run.
		Fixture f{ options };
		auto    carol = Fixture::MakeHello("Carol");
		carol.contentHash = 0x9999;
		const auto welcome = f.Join(3, carol);
		REQUIRE(welcome);
		CHECK(welcome->sessionId != firstSession);
		CHECK(f.Logged("load order changed"));
		CHECK(WorldStates(Received(f, 3))[0].deadActors.empty());
	}
}

TEST("server session file: junk the host's save had scrapped, reported again after a restart, stays scrapped")
{
	const TempSessionFile file;
	const auto            path = file.path.string();
	Server::Options options;
	options.sessionFile = path;
	auto junk = Wall(0x0001A2B3);
	junk.op = Protocol::WorkshopOp::kScrapped;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Alice", 0xA11CE)));
		f.fake->Deliver(1, Built(junk));
	}  // stopping saves

	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Alice", 0xA11CE)));
		f.fake->Deliver(1, Built(junk));  // her game walks its cells again in the new run
		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		const auto batches = Decoded(Received(f, 2), MT::kWorkshopItems, Protocol::DecodeWorkshopItems);
		REQUIRE(batches.size() == 1);
		REQUIRE(batches[0].size() == 1);
		CHECK(batches[0][0].refId == 0x0001A2B3);
		CHECK(batches[0][0].op == Protocol::WorkshopOp::kScrapped);
	}
}

// --- What a client is believed about (docs/AUDIT-LIFETIME.md, network trust) ---------------------

namespace
{
	std::vector<std::uint8_t> Throw()
	{
		return Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kThrow }, MT::kReportShot);
	}

	std::vector<std::uint8_t> Explosion(float a_x, float a_y)
	{
		return Protocol::Encode(Protocol::Explosion{ 0, 0x0001D2EE, a_x, a_y, 0.0f, 0, 0x0000003C }, MT::kReportExplosion);
	}

	std::vector<std::uint8_t> SomeFace(float a_weight)
	{
		Protocol::Face face;
		face.female = true;
		face.race = 0x00013746;
		face.headParts = { 0x0004DA7A };
		face.hairColor = 0x0005D7A1;
		face.weight[0] = a_weight, face.weight[1] = 0.5f, face.weight[2] = 0.3f;
		return Protocol::Encode(face, MT::kReportFace);
	}
}

TEST("server trust: a death reported by someone who doesn't run the actor is dropped")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC, Reason::kInteract));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Death(NPC, true));  // Bob doesn't run it
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);

	f.fake->Deliver(1, Death(NPC, true));  // Alice does
	const auto deaths = Decoded(Received(f, 2), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == alice);
}

TEST("server trust: two players fighting one raider: the one whose takeover was refused still gets the kill")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));  // Alice's game runs the raider
	Clear(f, { 1, 2 });

	// Bob shoots it: his game reports its health and asks to take it over, which is refused (it changed hands
	// moments ago), so it stays Alice's. Then he kills it.
	f.fake->Deliver(2, Health(NPC, 0.3f));
	f.fake->Deliver(2, Claim(NPC, Reason::kInteract));
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, alice));
	f.fake->Deliver(2, DeathAt(NPC, true, 250.0f));

	const auto toAlice = Received(f, 1);
	const auto deaths = Decoded(toAlice, MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == NPC);
	CHECK(deaths[0].playerId == bob);  // Alice's kill feed names Bob, and her copy is pushed by his stand-in
	CHECK(deaths[0].killed);
	CHECK(deaths[0].x == 250.0f);      // and lies where it fell for him
	CHECK(HasOwner(Owners(toAlice), NPC, 0));  // her game stops running a corpse
	CHECK(HasOwner(Owners(Received(f, 2)), NPC, 0));

	// Her own copy falls as the news arrives and her game reports it too: nobody hears of it twice.
	f.fake->Deliver(1, Death(NPC));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);
	// Nobody can run it again, and a newcomer finds it dead where Bob dropped it.
	f.fake->Deliver(1, Claim(NPC));
	CHECK(Owners(Received(f, 1)).empty());
	JoinAs(f, 3, "Carol");
	const auto toCarol = Received(f, 3);
	const auto world = WorldStates(toCarol);
	REQUIRE(world.size() == 1);
	CHECK(std::ranges::contains(world[0].deadActors, NPC));
	const auto placed = Decoded(toCarol, MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(placed.size() == 1);
	CHECK(placed[0].x == 250.0f);
}

TEST("server trust: a one-shot kill counts: the takeover claim from the hit, then the death, back to back")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim({ { NPC, Reason::kUnowned }, { NPC2, Reason::kUnowned } }));
	Clear(f, { 1, 2 });

	// A sneak headshot: no health report (it died of the hit), just the claim the hit made and the death a
	// frame later, as Session::Frame sends them.
	f.fake->Deliver(2, Claim(NPC, Reason::kInteract));
	f.fake->Deliver(2, Death(NPC, true));
	const auto deaths = Decoded(Received(f, 1), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == bob);

	// The other order is no kill of Bob's: a death first, then a claim, says nothing about a fight.
	f.fake->Deliver(2, Death(NPC2, true));
	f.fake->Deliver(2, Claim(NPC2, Reason::kInteract));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(!std::ranges::contains(world[0].deadActors, NPC2));
}

TEST("server trust: in a three-player fight, only those who fought the raider can kill it, and a dropped kill is logged")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	const auto carol = JoinAs(f, 3, "Carol");
	f.fake->Deliver(1, Claim(NPC));
	f.fake->Deliver(2, Health(NPC, 0.6f));  // Bob is fighting it; Carol is not
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(3, Death(NPC, true));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);
	CHECK(f.Logged(std::format("death of {:08X} from player {} dropped: player {} runs it", NPC, carol, alice)));
	// Nor does a fight make Carol's say-so a found-dead report: only the host's game is believed about those.
	f.fake->Deliver(3, Health(NPC, 0.5f));
	f.fake->Deliver(3, FoundDeadReport(NPC));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);

	// Carol's fight now counts: her kill is everyone's.
	f.fake->Deliver(3, Death(NPC, true));
	const auto deaths = Decoded(Received(f, 1), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == carol);
	CHECK(Count(Received(f, 2), MT::kActorDied) == 1);
}

TEST("server trust: a fight long over is no licence to kill the NPC")
{
	Server::Options options;
	options.deathGrace = 200ms;
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));
	f.fake->Deliver(2, Health(NPC, 0.9f));
	f.fake->Deliver(2, Claim(NPC, Reason::kInteract));
	Clear(f, { 1, 2 });

	std::this_thread::sleep_for(300ms);
	f.fake->Deliver(2, Death(NPC, true));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 0);
	CHECK(f.Logged("dropped"));
	// Alice's game still runs it, and her report of its death is believed as always.
	f.fake->Deliver(1, Death(NPC, true));
	const auto deaths = Decoded(Received(f, 2), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].playerId == alice);
}

TEST("server trust: a death of an actor nobody runs is taken from anyone")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Death(NPC2));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 1);
}

namespace
{
	constexpr std::uint64_t HOST_IDENTITY = 0x1111;

	Server::Options HostedOptions()
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		return options;
	}

	// The host's own game: on the server's machine, with the identity the server was started for.
	std::uint32_t JoinAsHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		return JoinAs(a_f, a_peer, "Host", HOST_IDENTITY);
	}
}

TEST("server trust: the host's game is believed about an NPC dead in its save that another player runs")
{
	Fixture    f{ HostedOptions() };
	const auto host = JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	f.fake->SetLocal(4);
	JoinAs(f, 4, "Bot", 0x2222);  // on the server's machine, but not the host's game
	f.fake->Deliver(2, Claim({ { NPC, Reason::kUnowned }, { NPC2, Reason::kUnowned } }));  // alive in Bob's game
	Clear(f, { 1, 2, 3, 4 });

	f.fake->Deliver(3, FoundDeadReport(NPC2));  // Carol isn't the host
	f.fake->Deliver(4, FoundDeadReport(NPC2));  // nor is the bot
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);

	f.fake->Deliver(1, FoundDeadReport(NPC));  // found dead in the host's save
	const auto packets = Received(f, 2);
	const auto deaths = Decoded(packets, MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == NPC);
	CHECK(deaths[0].playerId == host);
	CHECK(deaths[0].foundDead);
	CHECK(!deaths[0].killed);
	CHECK(deaths[0].x == 0.0f && deaths[0].y == 0.0f && deaths[0].z == 0.0f);  // Bob's copy dies where it stands
	CHECK(HasOwner(Owners(packets), NPC, 0));  // Bob's game gives it up
	CHECK(Count(Received(f, 3), MT::kActorDied) == 1);

	f.fake->Deliver(2, Claim(NPC));
	CHECK(Owners(Received(f, 2)).empty());
}

TEST("server trust: the host's game isn't believed about an NPC another player runs dying in front of it")
{
	Fixture f{ HostedOptions() };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(2, Claim({ { NPC, Reason::kUnowned }, { NPC2, Reason::kUnowned } }));
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, DeathAt(NPC, false, 100.0f));  // the host's copy fell over: Bob's game runs it, alive
	f.fake->Deliver(1, DeathAt(NPC2, true, 100.0f));  // or the host shot it before its game took it over
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);

	// The same death found again after a reconnect or a load: still Bob's game's to judge.
	f.fake->Deliver(1, FoundDeadReport(NPC));
	f.fake->Deliver(1, FoundDeadReport(NPC2));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);

	f.fake->Deliver(2, DeathAt(NPC, true, 100.0f));  // Bob's game, which runs it, is believed
	CHECK(Count(Received(f, 1), MT::kActorDied) == 1);
}

TEST("server trust: a death the host's game saw happen without a corpse position isn't a found-dead report")
{
	Fixture f{ HostedOptions() };  // everyone on their own story
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(2, Claim({ { NPC, Reason::kUnowned }, { NPC2, Reason::kUnowned } }));  // alive in Bob's game
	Clear(f, { 1, 2 });

	// A quest's script in the host's game kills a character with no 3D there: nobody killed it and no place
	// is known, the same as a found-dead report but for the flag. The host's story doesn't decide Bob's.
	f.fake->Deliver(1, Death(NPC, false));
	f.fake->Deliver(1, Death(NPC2, true));  // or the host's own kill of one it can't see
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);
	CHECK(Owners(Received(f, 2)).empty());  // Bob's game keeps running them

	// Found dead after a load, it is still the same death the host's game saw happen.
	f.fake->Deliver(1, FoundDeadReport(NPC));
	f.fake->Deliver(1, FoundDeadReport(NPC2));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);

	f.fake->Deliver(2, Death(NPC, false));  // Bob's game, which runs it, is believed
	CHECK(Count(Received(f, 1), MT::kActorDied) == 1);
}

TEST("server trust: nor about one it has had alive this session")
{
	Fixture    f{ HostedOptions() };
	JoinAsHost(f, 1);
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Claim(NPC));  // the host's game ran it: alive there
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Release(NPC));
	f.fake->Deliver(2, Claim({ { NPC, Reason::kUnowned }, { NPC2, Reason::kUnowned }, { NPC3, Reason::kUnowned } }));
	f.fake->Deliver(1, Health(NPC2, 0.5f));  // the host hit it: alive there
	f.fake->Deliver(1, Claim(NPC3, Reason::kInteract));  // the host hit its corpse: that says nothing (and Bob's game keeps it)
	const auto owners = Owners(Received(f, 1));
	REQUIRE(HasOwner(owners, NPC, bob));
	REQUIRE(HasOwner(owners, NPC3, bob));
	Clear(f, { 2 });

	f.fake->Deliver(1, FoundDeadReport(NPC));
	f.fake->Deliver(1, FoundDeadReport(NPC2));
	f.fake->Deliver(1, FoundDeadReport(NPC3));
	const auto deaths = Decoded(Received(f, 2), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == NPC3);
}

TEST("server trust: an NPC the session knows is dead hurts nobody")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, Death(NPC, false));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Hit(alice, 10.0f, false, NPC));  // still alive in Bob's game for a moment
	CHECK(Count(Received(f, 1), MT::kPlayerDamaged) == 0);
	f.fake->Deliver(2, Hit(alice, 10.0f, false, NPC2));
	CHECK(Count(Received(f, 1), MT::kPlayerDamaged) == 1);
}

TEST("server trust: a hit is capped per hit and per second")
{
	Fixture    f;
	JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Hit(bob, 5000.0f, false, NPC));
	auto damage = Decoded(Received(f, 2), MT::kPlayerDamaged, Protocol::DecodePlayerHit);
	REQUIRE(damage.size() == 1);
	CHECK(damage[0].damage == 500.0f);

	for (int i = 0; i < 10; ++i) {
		f.fake->Deliver(1, Hit(bob, 500.0f, false, NPC));
	}
	damage = Decoded(Received(f, 2), MT::kPlayerDamaged, Protocol::DecodePlayerHit);
	float total = 0.0f;
	for (const auto& d : damage) {
		total += d.damage;
	}
	CHECK(total <= 1000.0f);  // 1500 a second, 500 already spent
}

TEST("server trust: a second face within five seconds isn't passed on straight away")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, SomeFace(0.2f));
	f.fake->Deliver(1, SomeFace(0.9f));
	CHECK(Count(Received(f, 2), MT::kPlayerFace) == 1);
}

TEST("server trust: an explosion is relayed only after a throw and near the thrower")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Explosion(100.0f, 100.0f));  // no throw yet
	CHECK(Count(Received(f, 2), MT::kExplosion) == 0);

	f.fake->Deliver(1, Throw());
	Protocol::PlayerState state;
	state.sequence = 1;
	state.worldspace = 0x0000003C;
	state.x = 100.0f, state.y = 100.0f;
	f.fake->Deliver(1, Protocol::Encode(state));
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Explosion(100000.0f, 100000.0f));  // far from Alice
	CHECK(Count(Received(f, 2), MT::kExplosion) == 0);
	f.fake->Deliver(1, Explosion(600.0f, 300.0f));  // near her, after her throw
	CHECK(Count(Received(f, 2), MT::kExplosion) == 1);
}

namespace
{
	// Where the player says they are (outdoors in the Commonwealth unless a_cell names an interior).
	std::vector<std::uint8_t> StateAt(std::uint32_t a_sequence, float a_x, float a_y, float a_z = 0.0f, std::uint32_t a_cell = 0)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.cell = a_cell;
		state.worldspace = a_cell ? 0 : 0x0000003C;
		state.x = a_x, state.y = a_y, state.z = a_z;
		return Protocol::Encode(state);
	}

	std::vector<std::uint8_t> ExplosionAt(float a_x, float a_y, float a_z, std::uint32_t a_base = 0x0001D2EE)
	{
		return Protocol::Encode(Protocol::Explosion{ 0, a_base, a_x, a_y, a_z, 0, 0x0000003C }, MT::kReportExplosion);
	}
}

TEST("server trust: an explosion from a player who hasn't said where they are is dropped")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Throw());
	f.fake->Deliver(1, Explosion(100.0f, 100.0f));  // a throw, but nowhere known
	CHECK(Count(Received(f, 2), MT::kExplosion) == 0);

	f.fake->Deliver(1, StateAt(1, 100.0f, 100.0f));
	f.fake->Deliver(1, Explosion(100.0f, 100.0f));
	CHECK(Count(Received(f, 2), MT::kExplosion) == 1);
}

TEST("server trust: an explosion is measured from the thrower in 3D, not just on the map")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, StateAt(1, 100.0f, 100.0f, 0.0f));
	f.fake->Deliver(1, Throw());
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, ExplosionAt(100.0f, 100.0f, 9000.0f));  // straight above her, far up
	CHECK(Count(Received(f, 2), MT::kExplosion) == 0);
	f.fake->Deliver(1, ExplosionAt(100.0f, 100.0f, 300.0f));
	CHECK(Count(Received(f, 2), MT::kExplosion) == 1);
}

TEST("server trust: a joiner's explosions stop at their budget however many throws they report")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, StateAt(1, 100.0f, 100.0f));
	for (int i = 0; i < 20; ++i) {
		f.fake->Deliver(1, Throw());  // throws cost nothing to report
	}
	Clear(f, { 1, 2 });
	for (int i = 0; i < 17; ++i) {
		f.fake->Deliver(1, Explosion(100.0f + static_cast<float>(i), 100.0f));
	}
	CHECK(Count(Received(f, 2), MT::kExplosion) == 16);
	CHECK(f.Logged("reports explosions faster than anyone throws"));
}

TEST("server trust: the host's own game has no explosion budget")
{
	Fixture f{ HostedOptions() };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	f.fake->Deliver(1, StateAt(1, 100.0f, 100.0f));
	f.fake->Deliver(1, Throw());
	Clear(f, { 1, 2 });
	for (int i = 0; i < 24; ++i) {
		f.fake->Deliver(1, Explosion(100.0f + static_cast<float>(i), 100.0f));  // a Fat Man into a ghoul pack: a chain of explosions
	}
	CHECK(Count(Received(f, 2), MT::kExplosion) == 24);
}

namespace
{
	// Deaths of NPCs nobody runs, from a_first on (each a different NPC).
	std::vector<std::vector<std::uint8_t>> Deaths(std::uint32_t a_first, int a_count)
	{
		std::vector<std::vector<std::uint8_t>> out;
		for (int i = 0; i < a_count; ++i) {
			out.push_back(Death(a_first + static_cast<std::uint32_t>(i)));
		}
		return out;
	}

	void DeliverAll(Fixture& a_f, PeerId a_peer, const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		for (const auto& packet : a_packets) {
			a_f.fake->Deliver(a_peer, packet);
		}
	}
}

TEST("server trust: a joiner's deaths stop at their budget and come back with time")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DeliverAll(f, 1, Deaths(0x00100000, 61));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 60);
	CHECK(f.Logged("reports deaths faster than any fight"));

	std::this_thread::sleep_for(1100ms);  // two a second come back
	DeliverAll(f, 1, Deaths(0x00100100, 2));
	CHECK(Count(Received(f, 2), MT::kActorDied) == 2);

	// The one dropped is dropped for good: it isn't in the session's dead either.
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.size() == 62);
	CHECK(std::ranges::find(world[0].deadActors, 0x00100000u + 60) == world[0].deadActors.end());
}

TEST("server trust: a flood that goes on says once per 10 seconds that the budget ran out, and how many were dropped")
{
	// The budget refills two a second and a flooder spends them at once: the log used to say so each time.
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DeliverAll(f, 1, Deaths(0x00200000, 62));  // 60 pass, 2 dropped
	REQUIRE(Count(Received(f, 2), MT::kActorDied) == 60);
	std::this_thread::sleep_for(1100ms);
	DeliverAll(f, 1, Deaths(0x00200100, 5));  // 2 pass, 3 dropped
	REQUIRE(Count(Received(f, 2), MT::kActorDied) == 2);
	CHECK(f.LoggedTimes("reports deaths faster than any fight") == 1);
	CHECK(f.Logged("(1 dropped since the last such line)"));
}

TEST("server trust: the host's own game reports any number of deaths")
{
	Fixture f{ HostedOptions() };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	DeliverAll(f, 1, Deaths(0x00100000, 90));  // a big fight, or found dead across a whole settlement
	CHECK(Count(Received(f, 2), MT::kActorDied) == 90);
}

TEST("server trust: a joiner who leaves and comes back as themselves gets no new budget of deaths")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	const auto start = std::chrono::steady_clock::now();
	JoinAs(f, 2, "Mallory", 0x0BAD);
	DeliverAll(f, 2, Deaths(0x00100000, 60));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 60);
	f.fake->Drop(2);
	Sync(f, 1);

	JoinAs(f, 3, "Mallory", 0x0BAD);  // the same game, a new connection
	DeliverAll(f, 3, Deaths(0x00200000, 60));
	const auto again = Count(Received(f, 1), MT::kActorDied);
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	CHECK(again <= static_cast<std::size_t>(2.0 * seconds) + 1);  // only what came back with time

	// Saying hello again on the same connection starts her over as a new arrival, with the same budget.
	f.fake->Deliver(3, Protocol::Encode(Fixture::MakeHello("Mallory", 0x0BAD)));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kWelcome)));
	DeliverAll(f, 3, Deaths(0x00300000, 60));
	const auto seconds2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	CHECK(again + Count(Received(f, 1), MT::kActorDied) <= static_cast<std::size_t>(2.0 * seconds2) + 1);
}

TEST("server trust: a joiner who comes back as someone new each time gets no more deaths than all joiners' budget")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	const auto  start = std::chrono::steady_clock::now();
	std::size_t relayed = 0;
	for (PeerId mallory = 2; mallory < 6; ++mallory) {
		JoinAs(f, mallory, "Mallory", 0x0BAD0000 + mallory);  // a made-up identity each time
		DeliverAll(f, mallory, Deaths(0x00100000 + 0x1000 * mallory, 60));
		relayed += Count(Received(f, 1), MT::kActorDied);
		f.fake->Drop(mallory);
		Sync(f, 1);
	}
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	CHECK(relayed >= 120);  // all joiners' 120 at once
	CHECK(relayed <= 120 + static_cast<std::size_t>(4.0 * seconds) + 1);  // not 60 for every rejoin
}

TEST("server trust: two honest joiners in a big fight each get their own budget of deaths")
{
	Fixture f;
	JoinAs(f, 1, "Host");
	JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	Clear(f, { 1, 2, 3 });
	DeliverAll(f, 2, Deaths(0x00100000, 60));
	DeliverAll(f, 3, Deaths(0x00200000, 60));
	CHECK(Count(Received(f, 1), MT::kActorDied) == 120);
}

TEST("server trust: a death refused, or known already, costs nothing from the budget")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	std::vector<Protocol::ActorClaim> bobs;
	for (std::uint32_t i = 0; i < 40; ++i) {
		bobs.push_back({ 0x00200000 + i, Reason::kUnowned });
	}
	f.fake->Deliver(2, Claim(bobs));  // Bob's game runs these
	f.fake->Deliver(1, Death(NPC));
	Clear(f, { 1, 2 });

	for (int round = 0; round < 2; ++round) {
		DeliverAll(f, 1, Deaths(0x00200000, 40));  // not Alice's to report
		for (int i = 0; i < 20; ++i) {
			f.fake->Deliver(1, Death(NPC));  // dead already
		}
	}
	CHECK(Count(Received(f, 2), MT::kActorDied) == 0);
	DeliverAll(f, 1, Deaths(0x00100000, 59));  // one of her 60 went on NPC
	CHECK(Count(Received(f, 2), MT::kActorDied) == 59);
}

TEST("server trust: a joiner's deaths over the budget are not in the session the host restarts from")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-server-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Mallory", 0xBAD)));
		DeliverAll(f, 1, Deaths(0x00100000, 200));  // every NPC around, as fast as it can
		Sync(f, 1);
	}  // the host quits: stopping saves

	Fixture f{ options };
	REQUIRE(f.Join(2, Fixture::MakeHello("Host")));
	const auto world = WorldStates(Received(f, 2));
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.size() == 60);
	std::filesystem::remove(path);
}

TEST("server trust: one player's companion claims are capped")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	std::vector<Protocol::ActorClaim> claims;
	for (std::uint32_t i = 0; i < 6; ++i) {
		claims.push_back({ 0x00020000 + i, Reason::kCompanion });
	}
	f.fake->Deliver(1, Claim(claims));
	const auto owners = Decoded(Received(f, 2), MT::kActorOwners, Protocol::DecodeOwners);
	std::size_t granted = 0;
	for (const auto& batch : owners) {
		granted += batch.size();
	}
	CHECK(granted == 4);
}

TEST("server trust: one player's container changes stop at their share of the table")
{
	Server::Options options;
	options.maxContainerChangesPerPlayer = 5;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (int i = 0; i < 8; ++i) {
		f.fake->Deliver(1, Container(CHEST, ITEM + i, 1));
	}
	CHECK(Count(Received(f, 2), MT::kContainerChanged) == 5);
	Clear(f, { 1 });
	f.fake->Deliver(2, Container(CHEST, ITEM, 1));  // Bob's share is his own
	CHECK(Count(Received(f, 1), MT::kContainerChanged) == 1);
}

TEST("server trust: a joiner is told where each dead NPC fell before the dead list")
{
	Fixture    f;
	JoinAs(f, 1, "Alice");
	Clear(f, { 1 });
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorDeath{ NPC, 0, true, 100.0f, 200.0f, 300.0f }, MT::kReportDeath));
	f.fake->Deliver(1, Death(NPC2));  // no place known
	Clear(f, { 1 });

	JoinAs(f, 2, "Bob");
	const auto packets = Received(f, 2);
	const auto deaths = Decoded(packets, MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(deaths.size() == 1);
	CHECK(deaths[0].refId == NPC);
	CHECK(deaths[0].x == 100.0f);
	CHECK(deaths[0].z == 300.0f);
	std::size_t deathAt = 0, worldAt = 0;
	for (std::size_t i = 0; i < packets.size(); ++i) {
		if (packets[i][0] == Type(MT::kActorDied)) deathAt = i;
		if (packets[i][0] == Type(MT::kWorldState) && !worldAt) worldAt = i;
	}
	CHECK(deathAt < worldAt);
	const auto world = WorldStates(packets);
	REQUIRE(world.size() == 1);
	CHECK(world[0].deadActors.size() == 2);
}

// --- Host authority, end to end: what one player's game sends, through the real server, judged by the --
// --- rules the receiving game applies (src/game/RemoteEdits.h) -------------------------------------------

namespace
{
	// The receiving game's copies of a friend's builds, as WorkshopSync places and moves them: a new copy
	// starts at the game's own size, and each item the server passes on sets the size RemoteEdits::ScaleToApply
	// gives (nothing: left as it is).
	struct BuildsHere
	{
		std::map<std::uint32_t, float> sizes;  // ref -> our copy's size
		std::size_t                    rescaled = 0;

		void Apply(const Protocol::WorkshopItem& a_item)
		{
			const auto copy = sizes.find(a_item.refId);
			const float current = copy == sizes.end() ? 1.0f : copy->second;  // placed: the game's own size
			const auto  scale = RemoteEdits::ScaleToApply(a_item, current);
			rescaled += scale.has_value();
			sizes[a_item.refId] = scale.value_or(current);
		}

		void Handle(const Packets& a_packets)
		{
			for (const auto& item : Decoded(a_packets, MT::kWorkshopItem, Protocol::DecodeWorkshopItem)) {
				Apply(item);
			}
			for (const auto& batch : Decoded(a_packets, MT::kWorkshopItems, Protocol::DecodeWorkshopItems)) {
				for (const auto& item : batch) {
					Apply(item);
				}
			}
		}
	};

	Protocol::WorkshopItem SizedWall(std::uint32_t a_ref, float a_scale)
	{
		auto item = Wall(a_ref);
		item.scale = a_scale;
		return item;
	}
}

TEST("host authority: a friend's builds come back after a host restart at their own size, but none bigger than ten times or smaller than a tenth")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-server-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
		f.fake->Deliver(1, Built(SizedWall(0xFF000071, 100.0f)));    // the most the protocol lets through: covers a town
		f.fake->Deliver(1, Built(SizedWall(0xFF000072, 0.0001f)));   // too small to see, or to shoot past
		f.fake->Deliver(1, Built(SizedWall(0xFF000073, 1.5f)));      // the workshop's own scaling
		f.fake->Deliver(1, Built(SizedWall(0xFF000074, 1.0f)));
		Sync(f, 1);
	}  // the host quits: stopping saves

	Fixture    f{ options };
	BuildsHere hostGame;
	REQUIRE(f.Join(2, Fixture::MakeHello("Host")));
	JoinAs(f, 1, "Bob", 0xB0B);
	hostGame.Handle(Received(f, 2));
	REQUIRE(hostGame.sizes.size() == 4);
	CHECK(hostGame.sizes.at(0xFF000071) == RemoteEdits::MAX_SCALE);
	CHECK(hostGame.sizes.at(0xFF000072) == RemoteEdits::MIN_SCALE);
	CHECK(hostGame.sizes.at(0xFF000073) == 1.5f);
	CHECK(hostGame.sizes.at(0xFF000074) == 1.0f);
	CHECK(hostGame.rescaled == 3);  // a copy at the game's own size is left alone

	// Bob moves them about: the honest one keeps its size with no rescale, the huge one stays capped.
	const auto before = hostGame.rescaled;
	f.fake->Deliver(1, Built(SizedWall(0xFF000073, 1.5f)));
	f.fake->Deliver(1, Built(SizedWall(0xFF000071, 100.0f)));
	f.fake->Deliver(1, Built(SizedWall(0xFF000074, 50.0f)));  // the plain one, blown up
	hostGame.Handle(Received(f, 2));
	CHECK(hostGame.sizes.at(0xFF000073) == 1.5f);
	CHECK(hostGame.sizes.at(0xFF000071) == RemoteEdits::MAX_SCALE);
	CHECK(hostGame.sizes.at(0xFF000074) == RemoteEdits::MAX_SCALE);
	CHECK(hostGame.rescaled == before + 1);
	std::filesystem::remove(path);
}

namespace
{
	// Outer radii of explosion records, as the receiving game reads them.
	constexpr float FRAG_RADIUS = 500.0f;
	constexpr float FAT_MAN_RADIUS = 850.0f;
	constexpr float NUKE_RADIUS = 3000.0f;  // nothing thrown is this big

	// What the receiving game standing at a_us does with each explosion the server passed on to it: the rule
	// WeaponFire's ApplyExplosion applies, with the radius of the explosion's record in that game.
	std::vector<RemoteEdits::Blast> BlastsAt(const Packets& a_packets, const RemoteEdits::Spot& a_us, float a_radius)
	{
		std::vector<RemoteEdits::Blast> out;
		for (const auto& explosion : Decoded(a_packets, MT::kExplosion, Protocol::DecodeExplosion)) {
			out.push_back(RemoteEdits::SetOff(RemoteEdits::SpotOf(explosion), a_us, a_radius));
		}
		return out;
	}
}

TEST("host authority: a friend's grenade into the fight beside you goes off in your game with friendly fire on or off, so the enemies there die for you too")
{
	// The host's game runs the raiders, so the joiner's game can't report their deaths: the relayed
	// explosion is the only thing that kills them in the host's game.
	for (const bool friendlyFire : { false, true }) {
		Server::Options options;
		options.friendlyFire = friendlyFire;
		Fixture    f{ options };
		const auto host = f.Join(1, Fixture::MakeHello("Host"));
		REQUIRE(host);
		CHECK(host->friendlyFire == friendlyFire);
		JoinAs(f, 2, "Bob");
		const RemoteEdits::Spot hostAt{ 0, 0x0000003C, 1000.0f, 1000.0f, 0.0f };
		f.fake->Deliver(1, StateAt(1, hostAt.x, hostAt.y));
		f.fake->Deliver(2, StateAt(1, 1500.0f, 1000.0f));  // Bob stands 500 away
		f.fake->Deliver(2, Throw());
		Clear(f, { 1, 2 });

		f.fake->Deliver(2, ExplosionAt(1050.0f, 1000.0f, 0.0f));  // into the raiders at the host's feet
		f.fake->Deliver(2, ExplosionAt(1000.0f, 1300.0f, 0.0f));  // into the one biting the host's dog, 300 from the host
		f.fake->Deliver(2, ExplosionAt(2400.0f, 1000.0f, 0.0f));  // past the host, 1400 away
		const auto blasts = BlastsAt(Received(f, 1), hostAt, FRAG_RADIUS);
		REQUIRE(blasts.size() == 3);
		for (const auto blast : blasts) {
			CHECK(blast == RemoteEdits::Blast::kSetOff);
		}
		// A Fat Man's shell is thrown too.
		f.fake->Deliver(2, ExplosionAt(1200.0f, 1000.0f, 0.0f, 0x000E98F0));
		const auto shell = BlastsAt(Received(f, 1), hostAt, FAT_MAN_RADIUS);
		REQUIRE(shell.size() == 1);
		CHECK(shell[0] == RemoteEdits::Blast::kSetOff);
	}
}

TEST("host authority: a friend's game reporting a nuke-sized blast, or one somewhere you are not, sets nothing off in yours")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	JoinAs(f, 2, "Bob");
	const RemoteEdits::Spot hostAt{ 0, 0x0000003C, 0.0f, 3000.0f, 0.0f };
	f.fake->Deliver(1, StateAt(1, hostAt.x, hostAt.y));
	f.fake->Deliver(2, StateAt(1, 0.0f, 0.0f));
	f.fake->Deliver(2, Throw());
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, ExplosionAt(0.0f, 2000.0f, 0.0f, 0x000E98F0));  // after a throw and near Bob: the server passes it on
	auto blasts = BlastsAt(Received(f, 1), hostAt, NUKE_RADIUS);
	REQUIRE(blasts.size() == 1);
	CHECK(blasts[0] == RemoteEdits::Blast::kNotThrown);

	// The same numbers inside a building are nowhere near the host, who is outdoors.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Explosion{ 0, 0x0001D2EE, 0.0f, 3000.0f, 0.0f, 0x0001F00D, 0 }, MT::kReportExplosion));
	blasts = BlastsAt(Received(f, 1), hostAt, FRAG_RADIUS);
	REQUIRE(blasts.size() == 1);
	CHECK(blasts[0] == RemoteEdits::Blast::kAway);
}

TEST("host authority: a friend's game making up grenades at your feet sets off no more than the joiners' budget, however often it rejoins")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const RemoteEdits::Spot hostAt{ 0, 0x0000003C, 3000.0f, 0.0f, 0.0f };
	f.fake->Deliver(1, StateAt(1, hostAt.x, hostAt.y));
	const auto  start = std::chrono::steady_clock::now();
	std::size_t setOff = 0;
	for (PeerId mallory = 2; mallory < 6; ++mallory) {  // she leaves and comes back, as someone new each time
		REQUIRE(f.Join(mallory, Fixture::MakeHello("Mallory", 0x0BAD0000 + mallory)));
		f.fake->Deliver(mallory, StateAt(1, 0.0f, 0.0f));
		for (int i = 0; i < 20; ++i) {
			f.fake->Deliver(mallory, Throw());
			f.fake->Deliver(mallory, ExplosionAt(hostAt.x, hostAt.y, 0.0f));
		}
		for (const auto blast : BlastsAt(Received(f, 1), hostAt, FRAG_RADIUS)) {
			setOff += blast == RemoteEdits::Blast::kSetOff;
		}
		f.fake->Drop(mallory);
		Sync(f, 1);
	}
	// Each comes with 16 of its own, but all joiners share 32 at once: not 16 for every rejoin.
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	CHECK(setOff >= 32);
	CHECK(setOff <= 32 + static_cast<std::size_t>(8.0 * seconds) + 1);
}

namespace
{
	constexpr std::uint32_t CORVEGA_INSIDE = 0x0001F00D;  // an interior cell
	constexpr std::uint32_t BOSS = 0x0001F0B5;            // a unique character of the main story, inside it
	constexpr std::uint32_t RAIDER = 0x0001F0A1;          // one of his raiders
	const RemoteEdits::Spot BOSS_HERE{ CORVEGA_INSIDE, 0, 1200.0f, -300.0f, 0.0f };  // where our copy of him stands
	const RemoteEdits::Spot RAIDER_HERE{ CORVEGA_INSIDE, 0, 900.0f, 100.0f, 0.0f };
	const RemoteEdits::Spot BESIDE_BOSS{ CORVEGA_INSIDE, 0, 1100.0f, -200.0f, 0.0f };
	const RemoteEdits::Spot OUT_FRONT{ 0, 0x0000003C, 5000.0f, 5000.0f, 0.0f };
	const RemoteEdits::Spot ACROSS_THE_MAP{ 0, 0x0000003C, -60000.0f, 80000.0f, 0.0f };
	constexpr FoundDead::Npc UNIQUE{ .unique = true };
	constexpr FoundDead::Npc GENERIC{};

	// What a game has heard of where each other player is: the states the server passed on to it.
	using Heard = std::map<std::uint32_t, RemoteEdits::Spot>;

	void Note(Heard& a_heard, const std::vector<std::uint8_t>& a_packet)
	{
		const auto batch = Protocol::DecodePlayerStates(a_packet);
		REQUIRE(batch);
		for (const auto& entry : batch->players) {
			a_heard[entry.playerId] = RemoteEdits::SpotOf(entry.state);
		}
	}

	std::vector<std::uint8_t> StateOf(const RemoteEdits::Spot& a_at)
	{
		static std::uint32_t sequence = 0;
		Protocol::PlayerState state;
		state.sequence = ++sequence;
		state.cell = a_at.cell;
		state.worldspace = a_at.worldspace;
		state.x = a_at.x, state.y = a_at.y, state.z = a_at.z;
		return Protocol::Encode(state);
	}

	// Hands what the server sends a_peer to a_handle, in order, until a_done() or three seconds pass.
	template <class Handle, class Done>
	void Pump(Fixture& a_f, PeerId a_peer, Handle&& a_handle, Done&& a_done)
	{
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while (!a_done() && std::chrono::steady_clock::now() < deadline) {
			a_handle(a_f.fake->Take(a_peer));
			std::this_thread::sleep_for(10ms);
		}
		REQUIRE(a_done());
	}

	// Player a_id (on a_peer) says where they stand; returns once the server has passed it on to a_watcher.
	void Stand(Fixture& a_f, PeerId a_peer, std::uint32_t a_id, const RemoteEdits::Spot& a_at, PeerId a_watcher, Heard& a_heard)
	{
		a_f.fake->Deliver(a_peer, StateOf(a_at));
		Pump(
			a_f, a_watcher,
			[&](const Packets& a_packets) {
				for (const auto& packet : a_packets) {
					if (packet[0] == Type(MT::kPlayerStates)) {
						Note(a_heard, packet);
					}
				}
			},
			[&] { return a_heard.contains(a_id) && a_heard[a_id].x == a_at.x && a_heard[a_id].cell == a_at.cell; });
	}

	// The receiving game, as far as other players' deaths go: what it has heard of where each player is (the
	// states the server passes on), its loaded copies, and the real DeathReports judging each death as it
	// comes and again on every frame after, as WorldSync does. The test moves its clock.
	struct DeathsHere
	{
		bool                                           sharedStory = false;
		Heard                                          heard;
		std::map<std::uint32_t, DeathReports::Copy>    loaded;  // our copies, by ref
		std::set<std::uint32_t>                        dead, killed, keptAlive;
		std::map<std::uint32_t, DeathReports::Verdict> firstVerdict;  // when each death was first judged
		DeathReports::Clock::time_point                now{ std::chrono::hours(1) };
		DeathReports::Book                             book{ [this](std::uint32_t a_player) -> std::optional<RemoteEdits::Spot> {
			const auto it = heard.find(a_player);
			return it == heard.end() ? std::nullopt : std::optional{ it->second };
		} };

		// WorldSync's TryKill past the game's own checks: our copy must be loaded, then the judge decides.
		bool TryKill(std::uint32_t a_ref, const DeathReports::Report& a_report)
		{
			const auto copy = loaded.find(a_ref);
			if (copy == loaded.end()) {
				return false;
			}
			const auto verdict = book.Judge(sharedStory, a_report, copy->second, now);
			firstVerdict.try_emplace(a_ref, verdict);
			switch (verdict) {
			case DeathReports::Verdict::kKill:
				killed.insert(a_ref);
				return true;
			case DeathReports::Verdict::kKeepAlive:
				keptAlive.insert(a_ref);
				dead.erase(a_ref);
				return true;
			default:
				return false;
			}
		}

		void Take(std::uint32_t a_ref, const DeathReports::Report& a_report)
		{
			if (dead.insert(a_ref).second && !TryKill(a_ref, a_report)) {
				book.Hold(a_ref, a_report);
			}
		}

		// What the server sent, in the order it sent it (as Session hands it on); then a frame.
		void Handle(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet[0] == Type(MT::kPlayerStates)) {
					Note(heard, packet);
				} else if (packet[0] == Type(MT::kActorDied)) {
					const auto death = Protocol::DecodeActorDeath(packet);
					REQUIRE(death);
					Take(death->refId, book.Received(*death, now));
				} else if (packet[0] == Type(MT::kWorldState)) {
					const auto world = Protocol::DecodeWorldState(packet);
					REQUIRE(world);
					for (const auto ref : world->deadActors) {
						Take(ref, DeathReports::Book::Replayed(now));
					}
				}
			}
			Frame();
		}

		void Frame()
		{
			book.Retry([this](std::uint32_t a_ref, const DeathReports::Report& a_report) { return TryKill(a_ref, a_report); });
		}

		bool Judged(std::uint32_t a_ref) const { return killed.contains(a_ref) || keptAlive.contains(a_ref); }
	};

	auto Handler(DeathsHere& a_game)
	{
		return [&a_game](const Packets& a_packets) { a_game.Handle(a_packets); };
	}
}

TEST("host authority: a boss killed by a friend beside him dies in your game too, even while you wait outside")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(2, Claim({ { BOSS, Reason::kUnowned }, { RAIDER, Reason::kUnowned } }));  // Bob's game runs the fight inside
	Clear(f, { 1, 2 });
	DeathsHere hostGame;
	hostGame.sharedStory = host->sharedStory;
	f.fake->Deliver(1, StateOf(OUT_FRONT));  // the host waits out front: the boss isn't loaded in their game
	f.fake->Deliver(2, StateOf(BESIDE_BOSS));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard.contains(bob); });
	f.fake->Deliver(2, DeathAt(BOSS, true, 1150.0f));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.book.Size() == 1; });  // held until the boss loads

	// Bob walks off; much later the host goes in, and the boss loads.
	f.fake->Deliver(2, StateOf(ACROSS_THE_MAP));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard[bob].x == ACROSS_THE_MAP.x; });
	hostGame.now += 30s;
	hostGame.loaded[BOSS] = { UNIQUE, BOSS_HERE };
	hostGame.Frame();
	CHECK(hostGame.killed.contains(BOSS));  // where Bob was when he reported it counts
	CHECK(hostGame.book.Empty());
}

TEST("host authority: a boss a friend kills just after walking in dies in your game, though the death overtakes their steps")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(2, Claim(BOSS));
	Clear(f, { 1, 2 });
	DeathsHere hostGame;
	hostGame.loaded[BOSS] = { UNIQUE, BOSS_HERE };  // the host is inside already
	f.fake->Deliver(2, StateOf(OUT_FRONT));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard.contains(bob); });

	// Bob runs in and kills him at once. The death goes out reliably and at once, his position with the next
	// batch of states: the death reaches the host first.
	f.fake->Deliver(2, DeathAt(BOSS, true, 1150.0f));
	f.fake->Deliver(2, StateOf(BESIDE_BOSS));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.firstVerdict.contains(BOSS); });
	CHECK(hostGame.firstVerdict.at(BOSS) == DeathReports::Verdict::kWait);
	CHECK(!hostGame.keptAlive.contains(BOSS));

	hostGame.now += 500ms;
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.Judged(BOSS); });
	CHECK(hostGame.killed.contains(BOSS));
}

TEST("host authority: a boss a friend kills beside him after their steps arrived dies in your game at once")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	f.fake->Deliver(2, Claim(BOSS));
	Clear(f, { 1, 2 });
	DeathsHere hostGame;
	hostGame.loaded[BOSS] = { UNIQUE, BOSS_HERE };
	f.fake->Deliver(2, StateOf(BESIDE_BOSS));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard.contains(bob); });
	f.fake->Deliver(2, DeathAt(BOSS, true, 1150.0f));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.firstVerdict.contains(BOSS); });
	CHECK(hostGame.firstVerdict.at(BOSS) == DeathReports::Verdict::kKill);
}

TEST("host authority: a friend's game reporting a story character dead from nowhere near leaves it alive in yours, but not a raider")
{
	for (const bool shared : { false, true }) {
		Server::Options options;
		options.sharedStory = shared;
		Fixture    f{ options };
		const auto host = f.Join(1, Fixture::MakeHello("Host"));
		REQUIRE(host);
		const auto mallory = JoinAs(f, 2, "Mallory");
		const auto bob = JoinAs(f, 3, "Bob");
		Clear(f, { 1, 2, 3 });
		DeathsHere hostGame;
		hostGame.sharedStory = host->sharedStory;
		hostGame.loaded[BOSS] = { UNIQUE, BOSS_HERE };
		hostGame.loaded[RAIDER] = { GENERIC, RAIDER_HERE };
		f.fake->Deliver(2, StateOf(ACROSS_THE_MAP));
		f.fake->Deliver(3, StateOf(BESIDE_BOSS));
		Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard.contains(mallory) && hostGame.heard.contains(bob); });

		// Nobody runs either, so the server takes the report from anyone. She names Bob, who stands beside
		// the boss, as the reporter: the server says who it really came from.
		f.fake->Deliver(2, Protocol::Encode(Protocol::ActorDeath{ BOSS, bob, true, 1200.0f, 0.0f, 0.0f }, MT::kReportDeath));
		f.fake->Deliver(2, DeathAt(RAIDER, true, 900.0f));
		Pump(f, 1, Handler(hostGame), [&] { return hostGame.firstVerdict.size() == 2; });
		CHECK(hostGame.killed.contains(RAIDER));
		CHECK(hostGame.firstVerdict.at(BOSS) == (shared ? DeathReports::Verdict::kKill : DeathReports::Verdict::kWait));

		// Her position doesn't catch up with her claim: in the host's own story the boss lives on, for good.
		hostGame.now += DeathReports::CATCH_UP;
		hostGame.Frame();
		CHECK(hostGame.killed.contains(BOSS) == shared);
		CHECK(hostGame.keptAlive.contains(BOSS) == !shared);
		CHECK(hostGame.book.Empty());
		Stand(f, 2, mallory, BESIDE_BOSS, 1, hostGame.heard);  // walking over now changes nothing
		hostGame.Frame();
		CHECK(hostGame.killed.contains(BOSS) == shared);
	}
}

TEST("host authority: after loading a save from before the fight, a boss you both watched die is alive again in your own story, and dead in a shared one")
{
	// The session keeps who died, not who saw it: a replay names no reporter, so in your own story its
	// characters are your save's to decide. Generic enemies stay dead either way.
	for (const bool shared : { false, true }) {
		Server::Options options;
		options.sharedStory = shared;
		Fixture    f{ options };
		const auto host = f.Join(1, Fixture::MakeHello("Host"));
		REQUIRE(host);
		const auto bob = JoinAs(f, 2, "Bob");
		f.fake->Deliver(2, Claim({ { BOSS, Reason::kUnowned }, { RAIDER, Reason::kUnowned } }));
		Clear(f, { 1, 2 });
		DeathsHere hostGame;
		hostGame.sharedStory = host->sharedStory;
		hostGame.loaded[BOSS] = { UNIQUE, BOSS_HERE };
		hostGame.loaded[RAIDER] = { GENERIC, RAIDER_HERE };
		f.fake->Deliver(1, StateOf(BESIDE_BOSS));
		f.fake->Deliver(2, StateOf(BESIDE_BOSS));
		Pump(f, 1, Handler(hostGame), [&] { return hostGame.heard.contains(bob); });
		f.fake->Deliver(2, DeathAt(BOSS, true, 1200.0f));
		f.fake->Deliver(2, DeathAt(RAIDER, true, 900.0f));
		Pump(f, 1, Handler(hostGame), [&] { return hostGame.killed.size() == 2; });  // both die as they watch

		// The host loads a save from before the fight: a fresh game with both alive, and the session's dead
		// replayed to it.
		DeathsHere reloaded;
		reloaded.sharedStory = host->sharedStory;
		reloaded.loaded = hostGame.loaded;
		f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 0 }));
		Pump(f, 1, Handler(reloaded), [&] { return reloaded.Judged(BOSS) && reloaded.Judged(RAIDER); });
		CHECK(reloaded.killed.contains(RAIDER));
		CHECK(reloaded.killed.contains(BOSS) == shared);
		CHECK(reloaded.keptAlive.contains(BOSS) == !shared);
	}
}

namespace
{
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t SETTLER = 0x0001D1A0;  // a generic settler at a settlement

	std::vector<std::uint8_t> NpcAt(std::uint32_t a_ref, const RemoteEdits::Spot& a_at)
	{
		Protocol::ActorState state;
		state.refId = a_ref;
		state.cell = a_at.cell;
		state.worldspace = a_at.worldspace;
		state.x = a_at.x, state.y = a_at.y, state.z = a_at.z;
		return Protocol::Encode(std::vector<Protocol::ActorState>{ state }, MT::kActorStates);
	}

	// The receiving game's NpcSync, as far as moving a friend's NPC to where their game has it goes: who runs
	// each NPC (the owners the server sends), the last state each player sent, and for each NPC position the
	// server passes on, whether ours is brought there (RemoteEdits::OwnerBeside, as BringIn and the bring-over
	// of a far mirror ask it).
	struct MovesHere
	{
		std::map<std::uint32_t, std::uint32_t>         owners;
		std::map<std::uint32_t, Protocol::PlayerState> lastStates;
		std::vector<std::pair<std::uint32_t, bool>>    brought;  // (NPC, brought to where its owner's game has it), in order

		void Handle(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet[0] == Type(MT::kActorOwners)) {
					const auto batch = Protocol::DecodeOwners(packet);
					REQUIRE(batch);
					for (const auto& entry : *batch) {
						if (entry.playerId == 0) {
							owners.erase(entry.refId);
						} else {
							owners[entry.refId] = entry.playerId;
						}
					}
				} else if (packet[0] == Type(MT::kPlayerStates)) {
					const auto batch = Protocol::DecodePlayerStates(packet);
					REQUIRE(batch);
					for (const auto& entry : batch->players) {
						lastStates[entry.playerId] = entry.state;
					}
				} else if (packet[0] == Type(MT::kActorStatesRelay)) {
					const auto states = Protocol::DecodeActorStates(packet);
					REQUIRE(states);
					for (const auto& state : *states) {
						const auto owner = owners.find(state.refId);
						const auto lastState = [this](std::uint32_t a_player) -> std::optional<Protocol::PlayerState> {
							const auto it = lastStates.find(a_player);
							return it == lastStates.end() ? std::nullopt : std::optional{ it->second };
						};
						brought.emplace_back(state.refId, RemoteEdits::OwnerBeside(lastState, owner == owners.end() ? 0 : owner->second, state));
					}
				}
			}
		}

		// The verdicts since the last call.
		std::vector<std::pair<std::uint32_t, bool>> Take() { return std::exchange(brought, {}); }
	};

	auto Handler(MovesHere& a_game)
	{
		return [&a_game](const Packets& a_packets) { a_game.Handle(a_packets); };
	}

	const RemoteEdits::Spot HOST_AT{ 0, 0x0000003C, 5000.0f, 5000.0f, 0.0f };
	const RemoteEdits::Spot BESIDE_HOST{ 0, 0x0000003C, 5100.0f, 5000.0f, 0.0f };
}

TEST("host authority: a friend's companion walks in beside them, even when its steps arrive before theirs, but is never pulled to you on their game's word alone")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	MovesHere hostGame;
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.owners.contains(DOGMEAT); });
	CHECK(hostGame.owners.at(DOGMEAT) == bob);

	// Bob walks up to the host with his dog at heel; the dog's position comes before his (positions of NPCs
	// go out as they come, players' with the next batch). Not moved yet; the next one, once his has come, is.
	f.fake->Deliver(2, NpcAt(DOGMEAT, BESIDE_HOST));
	f.fake->Deliver(2, StateOf({ 0, 0x0000003C, 5300.0f, 5100.0f, 0.0f }));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1 && hostGame.lastStates.contains(bob); });
	auto brought = hostGame.Take();
	CHECK(!brought[0].second);
	f.fake->Deliver(2, NpcAt(DOGMEAT, BESIDE_HOST));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	CHECK(hostGame.Take()[0].second);

	// Bob is across the map, and his game says the dog stands next to the host.
	f.fake->Deliver(2, StateOf(ACROSS_THE_MAP));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.lastStates[bob].x == ACROSS_THE_MAP.x; });
	f.fake->Deliver(2, NpcAt(DOGMEAT, BESIDE_HOST));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	CHECK(!hostGame.Take()[0].second);

	// Carol doesn't run the dog: her positions for it never reach the host.
	f.fake->Deliver(3, NpcAt(DOGMEAT, BESIDE_HOST));
	hostGame.Handle(Received(f, 1));
	CHECK(hostGame.brought.empty());
}

TEST("host authority: when a friend's companion changes hands, it follows its new owner's stand-in, not the old one's")
{
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	const auto carol = JoinAs(f, 3, "Carol");
	MovesHere hostGame;
	f.fake->Deliver(2, StateOf(BESIDE_HOST));
	f.fake->Deliver(3, StateOf(ACROSS_THE_MAP));
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.owners.contains(DOGMEAT) && hostGame.lastStates.contains(carol); });

	// Bob lets the dog go and Carol recruits it, far away: positions beside Bob and the host now move nothing.
	f.fake->Deliver(2, Release(DOGMEAT));
	f.fake->Deliver(3, Claim(DOGMEAT, Reason::kCompanion));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.owners.contains(DOGMEAT) && hostGame.owners.at(DOGMEAT) == carol; });
	f.fake->Deliver(3, NpcAt(DOGMEAT, BESIDE_HOST));
	f.fake->Deliver(2, NpcAt(DOGMEAT, BESIDE_HOST));  // no longer Bob's to send
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	auto brought = hostGame.Take();
	CHECK(!brought[0].second);
	CHECK(hostGame.lastStates.at(bob).x == BESIDE_HOST.x);

	// Beside Carol, it is.
	f.fake->Deliver(3, NpcAt(DOGMEAT, { 0, 0x0000003C, ACROSS_THE_MAP.x + 200.0f, ACROSS_THE_MAP.y, 0.0f }));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	CHECK(hostGame.Take()[0].second);
}

TEST("host authority: a settler a friend's game moved far from them is not pulled across your settlement; it runs its own AI in yours until they come")
{
	// The bring-over of a far mirror asks the same question of every NPC a friend's game runs, not only
	// companions: their game moving one somewhere they are not is only their word for it.
	Fixture    f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	const auto bob = JoinAs(f, 2, "Bob");
	MovesHere hostGame;
	f.fake->Deliver(2, StateOf({ 0, 0x0000003C, 20000.0f, 5000.0f, 0.0f }));  // Bob at the far end of the settlement
	f.fake->Deliver(2, Claim(SETTLER));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.owners.contains(SETTLER) && hostGame.lastStates.contains(bob); });
	f.fake->Deliver(2, NpcAt(SETTLER, BESIDE_HOST));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	CHECK(!hostGame.Take()[0].second);

	f.fake->Deliver(2, StateOf(HOST_AT));  // Bob walks over: now it is brought over beside him
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.lastStates[bob].x == HOST_AT.x; });
	f.fake->Deliver(2, NpcAt(SETTLER, BESIDE_HOST));
	Pump(f, 1, Handler(hostGame), [&] { return hostGame.brought.size() == 1; });
	CHECK(hostGame.Take()[0].second);
}

TEST("host authority: a friend who asks to look like any NPC but a settler is shown as your own default, even after rejoining with that look")
{
	constexpr std::uint32_t SOME_NPC = 0x0024A381;  // an ordinary, non-unique NPC with scripts of its own
	Fixture f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));  // no look picked: everyone's own default
	REQUIRE(host);
	auto bobHello = Fixture::MakeHello("Bob", 0xB0B);
	bobHello.appearance = SOME_NPC;
	const auto bob = f.Join(2, bobHello);
	REQUIRE(bob);
	auto carolHello = Fixture::MakeHello("Carol");
	carolHello.appearance = RemoteEdits::MALE_SETTLER;
	const auto carol = f.Join(3, carolHello);
	REQUIRE(carol);

	// Each game places a stand-in from the PlayerJoined the server sent it, by its own ini: the host's default
	// is the female settler, Bob's too, Carol's is that same NPC Bob asked for. (0: the game's own default.)
	const std::map<PeerId, std::uint32_t> defaults{ { 1, RemoteEdits::FEMALE_SETTLER }, { 2, RemoteEdits::FEMALE_SETTLER }, { 3, SOME_NPC } };
	std::map<std::pair<PeerId, std::uint32_t>, std::uint32_t> placed;  // (watcher, player) -> the look placed
	const auto watch = [&](PeerId a_peer) {
		for (const auto& joined : Decoded(Received(f, a_peer), MT::kPlayerJoined, Protocol::DecodePlayerJoined)) {
			placed[{ a_peer, joined.playerId }] = RemoteEdits::LookToPlace(joined.appearance, defaults.at(a_peer));
		}
	};
	for (const auto& [peer, localDefault] : defaults) {
		watch(peer);
	}
	CHECK(placed.size() == 6);
	CHECK(placed.at({ 1, bob->playerId }) == 0);  // the host sees Bob as its default, the female settler
	CHECK(placed.at({ 1, carol->playerId }) == RemoteEdits::MALE_SETTLER);
	CHECK(placed.at({ 2, carol->playerId }) == RemoteEdits::MALE_SETTLER);
	CHECK(placed.at({ 3, bob->playerId }) == SOME_NPC);  // Carol's own stand-ins are that NPC anyway
	CHECK(placed.at({ 2, host->playerId }) == 0);        // the host picked nothing
	CHECK(placed.at({ 3, host->playerId }) == 0);

	// Bob leaves and comes back asking for a unique character this time: still the host's default.
	f.fake->Drop(2);
	Sync(f, 1);
	bobHello.appearance = 0x0001A4D7;
	REQUIRE(f.Join(4, bobHello));
	watch(1);
	CHECK(placed.at({ 1, bob->playerId }) == 0);
}
