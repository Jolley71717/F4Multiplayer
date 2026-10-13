#include "ServerFixture.h"

using namespace TestServer;

// NPC gear: the player who runs an NPC reports what it wears; the server keeps it, relays it to
// the others and tells newcomers.
namespace
{
	constexpr std::uint32_t RAIDER = 0x0008340C;
	constexpr std::uint32_t PIPE_PISTOL = 0x00024F55;
	constexpr std::uint32_t RAIDER_LEATHERS = 0x000B3F4E;

	std::vector<std::uint8_t> Claim(std::uint32_t a_ref)
	{
		return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Protocol::ClaimReason::kUnowned } });
	}

	std::vector<std::uint8_t> Gear(std::uint32_t a_ref, std::vector<std::uint32_t> a_items)
	{
		return Protocol::Encode(Protocol::NpcEquipment{ 0, a_ref, std::move(a_items) }, MT::kReportNpcEquipment);
	}
}

TEST("protocol: NPC equipment round trip")
{
	const Protocol::NpcEquipment msg{ 7, RAIDER, { PIPE_PISTOL, RAIDER_LEATHERS } };
	const auto decoded = Protocol::DecodeNpcEquipment(Protocol::Encode(msg, MT::kNpcEquipment));
	REQUIRE(decoded);
	CHECK(*decoded == msg);
	// A runtime reference (0xFF......) isn't the same NPC in another game.
	CHECK(!Protocol::DecodeNpcEquipment(Protocol::Encode(Protocol::NpcEquipment{ 7, 0xFF000123, {} }, MT::kNpcEquipment)));
}

TEST("npc gear: the owner's report reaches the others with the owner's ID")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(alice);
	f.fake->Deliver(1, Claim(RAIDER));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kActorOwners)));
	f.fake->Take(2);

	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL }));
	const auto packet = f.fake->WaitFor(2, Type(MT::kNpcEquipment));
	REQUIRE(packet);
	const auto gear = Protocol::DecodeNpcEquipment(*packet);
	REQUIRE(gear);
	CHECK(gear->playerId == alice->playerId);
	CHECK(gear->refId == RAIDER);
	CHECK((gear->items == std::vector<std::uint32_t>{ PIPE_PISTOL }));
	// Not echoed to the owner.
	CHECK(!f.fake->WaitFor(1, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
}

TEST("npc gear: only the NPC's owner can report it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Deliver(1, Claim(RAIDER));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kActorOwners)));
	f.fake->Take(3);

	f.fake->Deliver(2, Gear(RAIDER, { PIPE_PISTOL }));  // Bob doesn't run it
	CHECK(!f.fake->WaitFor(3, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
	f.fake->Deliver(2, Gear(0x00012345, { PIPE_PISTOL }));  // nobody runs this one
	CHECK(!f.fake->WaitFor(3, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
}

TEST("npc gear: a newcomer is told what the run NPCs wear")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Deliver(1, Claim(RAIDER));
	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL, RAIDER_LEATHERS }));
	// The last report wins.
	f.fake->Deliver(1, Gear(RAIDER, { RAIDER_LEATHERS }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto packet = f.fake->WaitFor(2, Type(MT::kNpcEquipment));
	REQUIRE(packet);
	const auto gear = Protocol::DecodeNpcEquipment(*packet);
	REQUIRE(gear);
	CHECK(gear->playerId == alice->playerId);
	CHECK((gear->items == std::vector<std::uint32_t>{ RAIDER_LEATHERS }));
}

TEST("npc gear: forgotten when the NPC dies")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Deliver(1, Claim(RAIDER));
	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorDeath{ RAIDER, 0, true }, MT::kReportDeath));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));  // the death releases it

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWorldState)));
	CHECK(!f.fake->WaitFor(2, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
}

// F11: the gear kept for an NPC is its runner's. Once its runner lets it go, a newcomer used to be told the
// old gear under the next runner's name (each game rolls NPC gear on its own, so it was the wrong gear), and
// the table grew with every NPC anyone ever ran.
TEST("npc gear: after a release, a newcomer gets the next runner's gear, never the last one's")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	f.fake->Deliver(1, Claim(RAIDER));
	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL }));
	f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ RAIDER }));  // she walked away
	REQUIRE(f.fake->WaitFor(2, Type(MT::kNpcEquipment)));                               // (her report, while she ran it)
	f.fake->Deliver(2, Claim(RAIDER));                                                  // Bob's game runs it now
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kWorldState)));
	CHECK(!f.fake->WaitFor(3, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));

	f.fake->Deliver(2, Gear(RAIDER, { RAIDER_LEATHERS }));
	const auto packet = f.fake->WaitFor(3, Type(MT::kNpcEquipment));
	REQUIRE(packet);
	const auto gear = Protocol::DecodeNpcEquipment(*packet);
	REQUIRE(gear);
	CHECK(gear->playerId == bob->playerId);
	CHECK((gear->items == std::vector<std::uint32_t>{ RAIDER_LEATHERS }));
	REQUIRE(f.Join(4, Fixture::MakeHello("Dan")));
	const auto told = f.fake->WaitFor(4, Type(MT::kNpcEquipment));
	REQUIRE(told);
	CHECK((Protocol::DecodeNpcEquipment(*told)->items == std::vector<std::uint32_t>{ RAIDER_LEATHERS }));
}

TEST("npc gear: an NPC taken over as a companion isn't shown to newcomers in its last runner's gear")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Claim(RAIDER));
	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL }));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kNpcEquipment)));
	// It follows Bob now: his game takes it over at once.
	f.fake->Deliver(2, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { RAIDER, Protocol::ClaimReason::kCompanion } }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kActorOwners)));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kWorldState)));
	CHECK(!f.fake->WaitFor(3, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
}

TEST("npc gear: forgotten when its runner leaves")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Claim(RAIDER));
	f.fake->Deliver(1, Gear(RAIDER, { PIPE_PISTOL }));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kNpcEquipment)));
	f.fake->Drop(1);
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerLeft)));
	f.fake->Deliver(2, Claim(RAIDER));  // Bob's game picks it up
	REQUIRE(f.fake->WaitFor(2, Type(MT::kActorOwners)));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kWorldState)));
	CHECK(!f.fake->WaitFor(3, Type(MT::kNpcEquipment), std::chrono::milliseconds(300)));
}
