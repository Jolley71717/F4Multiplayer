// The session server's shared world: deaths, containers, pickups, doors, quests, NPC ownership,
// hits and the relays (shots, lines, voice, markers, pings).

#include "ServerFixture.h"

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

	std::uint32_t JoinAs(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_identity = 0)
	{
		const auto welcome = a_f.Join(a_peer, Fixture::MakeHello(std::move(a_name), a_identity));
		REQUIRE(welcome);
		return welcome->playerId;
	}

	// Returns once the server has handled everything delivered so far: it handles packets in the
	// order they arrive, and answers a heartbeat right away. (At most 10 a second per player.)
	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	// Everything the server sent a_peer up to now.
	Packets Received(Fixture& a_f, PeerId a_peer)
	{
		Sync(a_f, a_peer);
		return a_f.fake->Take(a_peer);
	}

	void Clear(Fixture& a_f, std::initializer_list<PeerId> a_peers)
	{
		for (const auto peer : a_peers) {
			Received(a_f, peer);
		}
	}

	std::size_t Count(const Packets& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return !a_packet.empty() && a_packet[0] == Type(a_type); }));
	}

	// The packets of a_type, decoded.
	template <class Decode>
	auto Decoded(const Packets& a_packets, MT a_type, Decode a_decode)
	{
		using T = typename std::invoke_result_t<Decode, std::span<const std::uint8_t>>::value_type;
		std::vector<T> out;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(a_type)) {
				auto message = a_decode(std::span<const std::uint8_t>{ packet });
				REQUIRE(message);
				out.push_back(std::move(*message));
			}
		}
		return out;
	}

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

	std::vector<Protocol::WorldState> WorldStates(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kWorldState, [](auto a_data) { return Protocol::DecodeWorldState(a_data); });
	}

	// --- what clients send ---

	auto Death(std::uint32_t a_ref, bool a_killed = true)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_ref, 999, a_killed }, MT::kReportDeath);
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

	f.fake->Deliver(2, Death(NPC));
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
	CHECK((states[0] == RS{ DOOR, RS::kYes, RS::kNo }));
	CHECK(Count(Received(f, 1), MT::kRefStateChanged) == 0);

	// No change: nothing to pass on.
	f.fake->Deliver(1, Door(DOOR, RS::kYes, RS::kNo));
	f.fake->Deliver(1, Door(RUNTIME, RS::kYes, RS::kNo));
	CHECK(Count(Received(f, 2), MT::kRefStateChanged) == 0);

	f.fake->Deliver(2, Door(DOOR, RS::kNo, RS::kYes));
	states = Decoded(Received(f, 1), MT::kRefStateChanged, Protocol::DecodeRefState);
	REQUIRE(states.size() == 1);
	CHECK((states[0] == RS{ DOOR, RS::kNo, RS::kYes }));

	JoinAs(f, 3, "Carol");
	const auto world = WorldStates(Received(f, 3));
	REQUIRE(world.size() == 1);
	REQUIRE(world[0].refStates.size() == 1);
	CHECK((world[0].refStates[0] == RS{ DOOR, RS::kNo, RS::kYes }));
}

// ---- quests -----------------------------------------------------------------------------

TEST("server world: quest stages are relayed once and late joiners get them in order")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Stage(QUEST, 10));
	f.fake->Deliver(2, Stage(QUEST2, 5));
	f.fake->Deliver(1, Stage(QUEST, 20));
	f.fake->Deliver(2, Stage(QUEST, 10));  // already known
	f.fake->Deliver(1, Stage(QUEST, 20));
	f.fake->Deliver(1, Stage(RUNTIME, 1));
	f.fake->Deliver(1, Stage(0, 1));

	const auto toBob = Decoded(Received(f, 2), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((toBob == std::vector<Protocol::QuestStage>{ { QUEST, 10 }, { QUEST, 20 } }));
	const auto toAlice = Decoded(Received(f, 1), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((toAlice == std::vector<Protocol::QuestStage>{ { QUEST2, 5 } }));

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
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Stage(QUEST, 20));
	f.fake->Deliver(1, Stage(QUEST, 10));
	const auto stages = Decoded(Received(f, 2), MT::kQuestStage, Protocol::DecodeQuestStage);
	CHECK((stages == std::vector<Protocol::QuestStage>{ { QUEST, 20 }, { QUEST, 10 } }));
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

TEST("server world: NPC ownership is capped at MAX_WORLD_STATE_ACTORS")
{
	constexpr std::size_t PACKETS = (Protocol::MAX_WORLD_STATE_ACTORS + Protocol::MAX_ACTORS_PER_PACKET - 1) / Protocol::MAX_ACTORS_PER_PACKET + 1;
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
	const auto watched = Burst(f, packets, { 1, 2 }, 1, MT::kActorOwners, Protocol::MAX_WORLD_STATE_ACTORS / Protocol::MAX_ACTORS_PER_PACKET + 1);
	std::this_thread::sleep_for(200ms);
	auto all = watched;
	std::ranges::move(Received(f, 1), std::back_inserter(all));
	Clear(f, { 2 });
	CHECK(Owners(all).size() == Protocol::MAX_WORLD_STATE_ACTORS);

	// Full: a new NPC isn't granted, not even as a companion.
	f.fake->Deliver(1, Claim(0x00F00000, Reason::kCompanion));
	CHECK(Owners(Received(f, 1)).empty());

	// A death makes room.
	f.fake->Deliver(1, Death(0x00100000));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Claim(0x00F00000));
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
