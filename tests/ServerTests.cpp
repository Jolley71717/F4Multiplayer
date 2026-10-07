#include "FakeTransport.h"
#include "Test.h"

#include "Protocol.h"
#include "Server.h"

#include <atomic>

namespace
{
	using MT = Protocol::MessageType;

	// Each test gets its own UDP port: a stopped server's port can stay busy for a moment.
	std::uint16_t NextPort()
	{
		static std::atomic<std::uint16_t> port{ 47100 };
		return port++;
	}

	// A server with one fake transport, stopped at the end of the test.
	struct Fixture
	{
		Server         server{ [](std::string_view) {} };
		FakeTransport* fake = nullptr;

		explicit Fixture(Server::Options a_options = {})
		{
			auto transport = std::make_unique<FakeTransport>();
			fake = transport.get();
			std::vector<std::unique_ptr<ServerTransport>> extra;
			extra.push_back(std::move(transport));
			a_options.port = NextPort();
			a_options.udpLoopbackOnly = true;
			REQUIRE(server.Start(a_options, std::move(extra)));
		}

		~Fixture() { server.Stop(); }

		static Protocol::Hello MakeHello(std::string a_name, std::uint64_t a_identity = 0)
		{
			Protocol::Hello hello;
			hello.contentHash = 0x1234;
			hello.name = std::move(a_name);
			hello.identity = a_identity;
			return hello;
		}

		// Connects a_peer, says hello and returns the Welcome.
		std::optional<Protocol::Welcome> Join(PeerId a_peer, const Protocol::Hello& a_hello)
		{
			fake->Connect(a_peer);
			fake->Deliver(a_peer, Protocol::Encode(a_hello));
			const auto packet = fake->WaitFor(a_peer, static_cast<std::uint8_t>(MT::kWelcome));
			return packet ? Protocol::DecodeWelcome(*packet) : std::nullopt;
		}
	};
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
	f.fake->Take(1);

	const auto again = f.Join(3, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == alice->playerId);
	CHECK(f.fake->Disconnected(2));
	// Bob saw Alice leave and come back under the same ID.
	const auto left = f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kPlayerLeft));
	REQUIRE(left);
	CHECK(Protocol::DecodePlayerLeft(*left)->playerId == alice->playerId);
	const auto joined = f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kPlayerJoined));
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
	const auto packet = f.fake->WaitFor(2, static_cast<std::uint8_t>(MT::kWelcome));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWelcome(*packet)->playerId == alice->playerId);
	CHECK(!f.fake->Disconnected(2));
	CHECK(f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kPlayerLeft)));
	CHECK(f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kPlayerJoined)));
}
