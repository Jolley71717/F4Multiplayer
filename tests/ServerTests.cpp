#include "ServerFixture.h"

using namespace TestServer;

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
	f.fake->Take(1);

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
	f.fake->Take(1);

	f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	const auto packet = f.fake->WaitFor(2, Type(MT::kWelcome));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWelcome(*packet)->playerId == alice->playerId);
	CHECK(!f.fake->Disconnected(2));
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
}
