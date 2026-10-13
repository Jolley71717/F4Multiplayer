// What the server writes to its log while real players come and go. Players attach that log (the
// host's game log holds it) to public bug reports, so it must never name a player, hold a password,
// a whole IP address or a whole SteamID64, while still telling a maintainer who did what by player
// ID. The players themselves still see each other's names: only the log is redacted.

#include "ServerFixture.h"

#include "Redact.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	constexpr std::uint64_t STEAM_ID = 76561198012345678ull;

	// Every PlayerJoined a_peer was sent so far, as id -> name.
	std::map<std::uint32_t, std::string> NamesHeard(Fixture& a_f, PeerId a_peer)
	{
		std::map<std::uint32_t, std::string> names;
		for (const auto& packet : a_f.fake->Take(a_peer)) {
			if (!packet.empty() && packet[0] == Type(MT::kPlayerJoined)) {
				const auto joined = Protocol::DecodePlayerJoined(packet);
				REQUIRE(joined);
				names[joined->playerId] = joined->name;
			}
		}
		return names;
	}

	// a_text is in no line of the log.
	bool NowhereInLog(Fixture& a_f, std::string_view a_text)
	{
		return std::ranges::none_of(a_f.LogLines(), [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
	}
}

TEST("log privacy: a whole evening (wrong password, joins over UDP and Steam, a crash and rejoin, a flood, leaving) logs players by ID only")
{
	Server::Options options;
	options.password = "hunter2";
	Fixture f{ options };

	// Someone guesses the password from the internet.
	f.fake->SetAddress(9, "udp 203.0.113.9:1111");
	auto guess = Fixture::MakeHello("Mallory");
	guess.password = "letmein";
	f.fake->Connect(9);
	f.fake->Deliver(9, Protocol::Encode(guess));
	REQUIRE(f.fake->WaitFor(9, Type(MT::kReject)));

	// Alice joins over UDP from her home address, Bob over Steam (a transport that names the account).
	f.fake->SetAddress(1, "udp 198.51.100.7:4321");
	f.fake->SetAddress(2, std::format("steam {}", STEAM_ID));
	auto aliceHello = Fixture::MakeHello("Alice", 0xA11CE);
	aliceHello.password = "hunter2";
	auto bobHello = Fixture::MakeHello("Bob", 0xB0B);
	bobHello.password = "hunter2";
	const auto alice = f.Join(1, aliceHello);
	const auto bob = f.Join(2, bobHello);
	REQUIRE(alice);
	REQUIRE(bob);

	// Bob's game crashes and he is back (same identity) before the server noticed.
	f.fake->SetAddress(3, std::format("steam {}", STEAM_ID));
	const auto bobAgain = f.Join(3, bobHello);
	REQUIRE(bobAgain);
	CHECK(bobAgain->playerId == bob->playerId);

	// Alice's game floods the server with events.
	for (int i = 0; i < 700; ++i) {
		Protocol::PlayerStatus status;
		status.level = static_cast<std::uint16_t>(i);
		f.fake->Deliver(1, Protocol::Encode(status, MT::kReportStatus));
	}
	std::this_thread::sleep_for(300ms);

	// Alice quits.
	f.fake->Take(3);
	f.fake->Drop(1);
	REQUIRE(f.fake->WaitFor(3, Type(MT::kPlayerLeft)));
	std::this_thread::sleep_for(100ms);

	const auto bobAlias = Redact::Alias(STEAM_ID);
	CHECK(f.Logged("wrong password from udp 203.0.113.x:1111"));
	CHECK(f.Logged(std::format("player {} joined from udp 198.51.100.x:4321", alice->playerId)));
	CHECK(f.Logged(std::format("player {} joined from steam {}", bob->playerId, bobAlias)));
	CHECK(f.Logged(std::format("dropping the old connection of player {} (steam {})", bob->playerId, bobAlias)));
	CHECK(f.Logged(std::format("player {} came back from steam {}", bob->playerId, bobAlias)));
	CHECK(f.Logged(std::format("player {} sends too many events", alice->playerId)));
	CHECK(f.Logged(std::format("player {} left", alice->playerId)));

	for (const std::string_view secret : { "Alice", "Bob", "Mallory", "hunter2", "letmein", "198.51.100.7", "203.0.113.9", "76561198012345678", "8012345678" }) {
		CHECK(NowhereInLog(f, secret));
	}
}

TEST("log privacy: a joiner whose name is a forged log line or a Steam id gets in, and the log holds neither")
{
	Fixture f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	f.fake->Take(1);

	// A name with a line break and a fake server line in it, then one that is someone's SteamID64.
	const auto forger = f.Join(2, Fixture::MakeHello("x\nserver: player 1 left"));
	const auto squatter = f.Join(3, Fixture::MakeHello("76561198087654321"));
	REQUIRE(forger);
	REQUIRE(squatter);
	std::this_thread::sleep_for(100ms);

	// The host hears them (their names lose the control characters, that is all).
	const auto heard = NamesHeard(f, 1);
	REQUIRE(heard.contains(forger->playerId));
	REQUIRE(heard.contains(squatter->playerId));
	CHECK(heard.at(forger->playerId).find('\n') == std::string::npos);
	CHECK(heard.at(squatter->playerId) == "76561198087654321");

	// The log: one line per event, none of them forged, no id that isn't a player number.
	const auto lines = f.LogLines();
	CHECK(std::ranges::none_of(lines, [](const std::string& a_line) { return a_line.find('\n') != std::string::npos; }));
	CHECK(!f.Logged(std::format("player {} left", host->playerId)));
	CHECK(NowhereInLog(f, "76561198087654321"));
	CHECK(f.Logged(std::format("player {} joined from fake 2", forger->playerId)));
	CHECK(f.Logged(std::format("player {} joined from fake 3", squatter->playerId)));
}

TEST("log privacy: a second joiner hears the first by name while the log keeps to player numbers")
{
	Fixture f;
	f.fake->SetAddress(1, "udp 192.0.2.44:50000");
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->SetAddress(2, "udp 127.0.0.1:50001");
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	std::this_thread::sleep_for(100ms);

	const auto bobHeard = NamesHeard(f, 2);
	REQUIRE(bobHeard.contains(alice->playerId));
	CHECK(bobHeard.at(alice->playerId) == "Alice");
	const auto aliceHeard = NamesHeard(f, 1);
	REQUIRE(aliceHeard.contains(bob->playerId));
	CHECK(aliceHeard.at(bob->playerId) == "Bob");

	CHECK(f.Logged(std::format("player {} joined from udp 192.0.2.x:50000", alice->playerId)));
	// This machine's own address is no secret and tells a maintainer it was the host's game or a bot.
	CHECK(f.Logged(std::format("player {} joined from udp 127.0.0.1:50001", bob->playerId)));
	CHECK(NowhereInLog(f, "Alice"));
	CHECK(NowhereInLog(f, "Bob"));
}
