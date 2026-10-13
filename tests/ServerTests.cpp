#include "ServerFixture.h"

using namespace TestServer;

namespace
{
	// Join returns at Alice's Welcome, but the server tells Bob (peer 1) she joined only after it, so a
	// Take right away could miss that and leave it to be mistaken for the PlayerJoined of her rejoin.
	// Waiting for it first, then taking the rest, leaves Bob's queue empty for what comes next.
	void TakeAliceJoined(Fixture& a_f)
	{
		REQUIRE(a_f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
		a_f.fake->Take(1);
	}
}

TEST("server: a player who says hello is welcomed")
{
	Fixture f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(welcome);
	CHECK(welcome->playerId != 0);
}

TEST("server: a returning player gets their old ID back")
{
	Fixture f;
	const auto first = f.Join(1, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(first);
	f.fake->Drop(1);
	const auto again = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == first->playerId);
}

// Review finding 2: their game restarted before the old connection timed out.
TEST("server: rejoining while the old connection is still open replaces it")
{
	Fixture f;
	const auto bob = f.Join(1, Fixture::MakeHello("Bob", 0xB0B));
	const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(bob);
	REQUIRE(alice);
	TakeAliceJoined(f);

	const auto again = f.Join(3, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == alice->playerId);
	CHECK(f.fake->Disconnected(2));
	// Bob saw Alice leave and come back under the same ID.
	const auto left = f.fake->WaitFor(1, Type(MT::kPlayerLeft));
	REQUIRE(left);
	CHECK(Protocol::DecodePlayerLeft(*left)->playerId == alice->playerId);
	const auto joined = f.fake->WaitFor(1, Type(MT::kPlayerJoined));
	REQUIRE(joined);
	CHECK(Protocol::DecodePlayerJoined(*joined)->playerId == alice->playerId);
}

TEST("server: a full server still lets a stale player's replacement in")
{
	Server::Options options;
	options.maxPlayers = 2;
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
	const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(alice);
	const auto again = f.Join(3, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == alice->playerId);
}

// Review finding 1: Steam keeps one connection per friend, so a restarted game says hello again
// on the connection the server already has.
TEST("server: hello again on the same connection welcomes them again")
{
	Fixture f;
	const auto bob = f.Join(1, Fixture::MakeHello("Bob", 0xB0B));
	const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(bob);
	REQUIRE(alice);
	TakeAliceJoined(f);

	f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	const auto packet = f.fake->WaitFor(2, Type(MT::kWelcome));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWelcome(*packet)->playerId == alice->playerId);
	CHECK(!f.fake->Disconnected(2));
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
}

// Security review: a world state can be a megabyte, so a client asking for it over and over must
// not be able to fill the host's outgoing queue. Hello again and kRequestWorldState share a budget.
// Every request is still answered in the end: the ones past the budget are owed, and all of them together get
// one world state when it refills (a test refill of a second, not ten). Hello again and again is the same:
// the latest one is answered then, once.
TEST("server: asking for the world state over and over gets only a few answers, and the rest one when the budget refills")
{
	Server::Options options;
	options.worldStateRefill = std::chrono::milliseconds(1000);
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice", 0xA11CE)));
	// The join's own world state (from the same budget) is sent right after the Welcome, on the server's
	// thread: Take alone could run before it arrives and leave it to be counted below.
	REQUIRE(f.fake->WaitFor(1, Type(MT::kWorldState)));
	f.fake->Take(1);

	for (int i = 0; i < 20; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::WorldRequest{}));
	}
	const auto atOnce = WorldStates(Received(f, 1)).size();
	CHECK(atOnce >= 1);
	CHECK(atOnce <= 3);
	CHECK(f.Logged("'Alice' has asked for the world too often"));
	// The rest: one, owed.
	std::vector<std::vector<std::uint8_t>> later;
	CHECK(Await(f, 1, MT::kWorldState, later, std::chrono::milliseconds(3000)));
	CHECK(WorldStates(later).size() == 1);
	CHECK(WorldStates(During(f, 1, std::chrono::milliseconds(300))).empty());

	for (int i = 0; i < 20; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	}
	std::vector<std::vector<std::uint8_t>> rejoined;
	CHECK(Await(f, 1, MT::kWelcome, rejoined, std::chrono::milliseconds(3000)));
	std::ranges::move(During(f, 1, std::chrono::milliseconds(300)), std::back_inserter(rejoined));
	CHECK(Count(rejoined, MT::kWelcome) >= 1);
	CHECK(Count(rejoined, MT::kWelcome) <= 2);  // not one per hello
	CHECK(!f.fake->Disconnected(1));
}

namespace
{
	std::vector<std::uint8_t> GearReport(std::vector<std::uint32_t> a_items)
	{
		return Protocol::Encode(Protocol::Equipment{ 0, std::move(a_items) }, MT::kEquipment);
	}

	std::optional<Protocol::Equipment> GearPassedOn(Fixture& a_f, PeerId a_to, std::chrono::milliseconds a_wait = std::chrono::milliseconds(2000))
	{
		const auto packet = a_f.fake->WaitFor(a_to, Type(MT::kPlayerEquipment), a_wait);
		return packet ? Protocol::DecodeEquipment(*packet) : std::nullopt;
	}

	constexpr std::uint32_t PISTOL = 0x0000DF42;
	constexpr std::uint32_t VAULT_SUIT = 0x0001EED7;
}

// A game sending the same list over and over (a modified one, or one that resends after loading a save) had every other
// player strip and dress its stand-in again for each copy.
TEST("server: a gear report the same as the player's last is not passed on; a changed one is, and a newcomer gets the latest")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
	const auto alice = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(alice);
	TakeAliceJoined(f);

	f.fake->Deliver(2, GearReport({ PISTOL, VAULT_SUIT }));
	auto gear = GearPassedOn(f, 1);
	REQUIRE(gear);
	CHECK(gear->playerId == alice->playerId);
	CHECK((gear->items == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));

	for (int i = 0; i < 5; ++i) {
		f.fake->Deliver(2, GearReport({ PISTOL, VAULT_SUIT }));
	}
	CHECK(!GearPassedOn(f, 1, std::chrono::milliseconds(300)));

	f.fake->Deliver(2, GearReport({ VAULT_SUIT }));  // gun put away for good
	gear = GearPassedOn(f, 1);
	REQUIRE(gear);
	CHECK((gear->items == std::vector<std::uint32_t>{ VAULT_SUIT }));
	// Back to what she had before the last report: a change from the last, so passed on.
	f.fake->Deliver(2, GearReport({ PISTOL, VAULT_SUIT }));
	gear = GearPassedOn(f, 1);
	REQUIRE(gear);
	CHECK((gear->items == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));

	const auto carol = f.Join(3, Fixture::MakeHello("Carol", 0xCA201));
	REQUIRE(carol);
	gear = GearPassedOn(f, 3);
	REQUIRE(gear);
	CHECK(gear->playerId == alice->playerId);
	CHECK((gear->items == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));
}

TEST("server: a player's first gear report is passed on even when they wear nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Alice", 0xA11CE)));
	TakeAliceJoined(f);

	f.fake->Deliver(2, GearReport({}));
	const auto gear = GearPassedOn(f, 1);
	REQUIRE(gear);
	CHECK(gear->items.empty());
	f.fake->Deliver(2, GearReport({}));
	CHECK(!GearPassedOn(f, 1, std::chrono::milliseconds(300)));
}
