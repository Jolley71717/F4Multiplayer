// The session server's lobby and session logic: who gets in, what they are told about each
// other, and the per-player relays (state, status, clock, XP, pings, heartbeats, rate limits).

#include "ServerFixture.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	// Gives the server a moment to act on what was delivered, then takes everything sent to a_peer.
	Packets Settle(Fixture& a_f, PeerId a_peer, std::chrono::milliseconds a_wait = 200ms)
	{
		std::this_thread::sleep_for(a_wait);
		return a_f.fake->Take(a_peer);
	}

	Packets OfType(const Packets& a_packets, MT a_type)
	{
		Packets out;
		for (const auto& packet : a_packets) {
			if (!packet.empty() && packet[0] == Type(a_type)) {
				out.push_back(packet);
			}
		}
		return out;
	}

	std::optional<Protocol::Reject> WaitReject(Fixture& a_f, PeerId a_peer)
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kReject));
		return packet ? Protocol::DecodeReject(*packet) : std::nullopt;
	}

	// Connects a_peer and says hello, expecting a rejection.
	std::optional<Protocol::Reject> JoinRejected(Fixture& a_f, PeerId a_peer, const std::vector<std::uint8_t>& a_hello)
	{
		a_f.fake->Connect(a_peer);
		a_f.fake->Deliver(a_peer, a_hello);
		return WaitReject(a_f, a_peer);
	}

	bool Contains(std::string_view a_text, std::string_view a_part) { return a_text.find(a_part) != std::string_view::npos; }

	Protocol::PlayerState State(std::uint32_t a_sequence, float a_x = 0.0f)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.x = a_x;
		return state;
	}

	// The states of player a_id in every PlayerStates batch in a_packets.
	std::vector<Protocol::PlayerState> StatesOf(const Packets& a_packets, std::uint32_t a_id)
	{
		std::vector<Protocol::PlayerState> out;
		for (const auto& packet : OfType(a_packets, MT::kPlayerStates)) {
			const auto batch = Protocol::DecodePlayerStates(packet);
			REQUIRE(batch);
			for (const auto& entry : batch->players) {
				if (entry.playerId == a_id) {
					out.push_back(entry.state);
				}
			}
		}
		return out;
	}

	Protocol::PlayerStatus Status(std::uint16_t a_level, std::uint8_t a_health = 100)
	{
		Protocol::PlayerStatus status;
		status.location = 0x1000;
		status.level = a_level;
		status.health = a_health;
		return status;
	}

	std::vector<std::uint8_t> TimeReport(float a_hour)
	{
		Protocol::WorldTime time;
		time.gameHour = a_hour;
		time.daysPassed = 3.0f;
		time.worldspace = 0x3C;
		time.weather = 0x1234;
		return Protocol::Encode(time, MT::kReportTime);
	}

	std::vector<std::uint8_t> XpReport(float a_xp)
	{
		return Protocol::Encode(Protocol::XpGain{ 0, a_xp }, MT::kReportXp);
	}

	std::vector<float> XpOf(const Packets& a_packets)
	{
		std::vector<float> out;
		for (const auto& packet : OfType(a_packets, MT::kPartyXp)) {
			const auto gain = Protocol::DecodeXpGain(packet);
			REQUIRE(gain);
			out.push_back(gain->xp);
		}
		return out;
	}
}

// ---- rejections --------------------------------------------------------------------------

TEST("server session: a hello with the wrong magic is rejected")
{
	Fixture f;
	auto hello = Fixture::MakeHello("Alice");
	hello.magic = 0xDEADBEEF;
	const auto reject = JoinRejected(f, 1, Protocol::Encode(hello));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "Not an F4Multiplayer client"));
	CHECK(f.fake->Disconnected(1));
	CHECK(OfType(f.fake->Take(1), MT::kWelcome).empty());
}

TEST("server session: a truncated hello is rejected")
{
	Fixture f;
	const auto full = Protocol::Encode(Fixture::MakeHello("Alice"));
	const auto reject = JoinRejected(f, 1, std::vector<std::uint8_t>(full.begin(), full.begin() + 5));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "Not an F4Multiplayer client"));
	CHECK(f.fake->Disconnected(1));
}

TEST("server session: a different protocol version is rejected, naming both versions")
{
	Fixture f;
	auto hello = Fixture::MakeHello("Alice");
	hello.version = Protocol::VERSION + 7;
	const auto reject = JoinRejected(f, 1, Protocol::Encode(hello));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "Version mismatch"));
	CHECK(Contains(reject->reason, std::format("protocol {}", Protocol::VERSION)));
	CHECK(Contains(reject->reason, std::format("you have {}", Protocol::VERSION + 7)));
	CHECK(f.fake->Disconnected(1));
}

TEST("server session: the wrong password is rejected, the right one gets in")
{
	Server::Options options;
	options.password = "hunter2";
	Fixture f{ options };

	auto wrong = Fixture::MakeHello("Mallory");
	wrong.password = "hunter3";
	const auto reject = JoinRejected(f, 1, Protocol::Encode(wrong));
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password");
	CHECK(f.fake->Disconnected(1));
	CHECK(f.Logged("wrong password from fake 1"));

	const auto none = JoinRejected(f, 2, Protocol::Encode(Fixture::MakeHello("Nobody")));
	REQUIRE(none);
	CHECK(none->reason == "Wrong password");

	auto right = Fixture::MakeHello("Alice");
	right.password = "hunter2";
	CHECK(f.Join(3, right));
}

TEST("server session: without a password, any password gets in")
{
	Fixture f;
	auto hello = Fixture::MakeHello("Alice");
	hello.password = "whatever";
	CHECK(f.Join(1, hello));
}

TEST("server session: a full server rejects the next player and has room again after a leave")
{
	Server::Options options;
	options.maxPlayers = 2;
	Fixture f{ options };
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));  // Bob (sent just after his welcome)

	const auto reject = JoinRejected(f, 3, Protocol::Encode(Fixture::MakeHello("Carol", 0xCA201)));
	REQUIRE(reject);
	CHECK(reject->reason == "Server is full");
	CHECK(f.fake->Disconnected(3));
	// Nobody heard about Carol, and nobody was dropped for her.
	CHECK(OfType(Settle(f, 1), MT::kPlayerJoined).empty());
	CHECK(!f.fake->Disconnected(1));
	CHECK(!f.fake->Disconnected(2));

	f.fake->Drop(2);
	CHECK(f.Join(4, Fixture::MakeHello("Carol", 0xCA201)));
}

TEST("server session: a different load order is rejected while others are in")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Take(1);

	auto hello = Fixture::MakeHello("Bob");
	hello.contentHash = 0x9999;
	const auto reject = JoinRejected(f, 2, Protocol::Encode(hello));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "load order"));
	CHECK(f.fake->Disconnected(2));
	CHECK(OfType(Settle(f, 1), MT::kPlayerJoined).empty());
}

TEST("server session: a new load order after everyone left starts a new session")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Drop(1);

	auto hello = Fixture::MakeHello("Bob");
	hello.contentHash = 0x9999;
	const auto bob = f.Join(2, hello);
	REQUIRE(bob);
	CHECK(bob->sessionId != alice->sessionId);
	CHECK(f.Logged("load order changed"));

	// The new load order is now the session's: the old one is turned away.
	const auto reject = JoinRejected(f, 3, Protocol::Encode(Fixture::MakeHello("Carol")));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "load order"));
}

TEST("server session: the same load order after everyone left keeps the session")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Drop(1);
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	CHECK(bob->sessionId == alice->sessionId);
	CHECK(!f.Logged("load order changed"));
}

TEST("server session: a rejected first player doesn't start a session")
{
	Server::Options options;
	options.password = "pw";
	Fixture f{ options };
	auto wrong = Fixture::MakeHello("Mallory");
	wrong.contentHash = 0x9999;
	REQUIRE(JoinRejected(f, 1, Protocol::Encode(wrong)));

	// The rejected hello's load order was not taken as the session's.
	auto hello = Fixture::MakeHello("Alice");
	hello.password = "pw";
	REQUIRE(f.Join(2, hello));
	auto bob = Fixture::MakeHello("Bob");
	bob.password = "pw";
	CHECK(f.Join(3, bob));
}

// ---- welcome -----------------------------------------------------------------------------

TEST("server session: every player gets their own ID and the same session")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(alice);
	REQUIRE(bob);
	REQUIRE(carol);
	CHECK(alice->playerId != 0);
	CHECK(alice->playerId != bob->playerId);
	CHECK(alice->playerId != carol->playerId);
	CHECK(bob->playerId != carol->playerId);
	CHECK(alice->sessionId != 0);
	CHECK(alice->sessionId == bob->sessionId);
	CHECK(alice->sessionId == carol->sessionId);
}

TEST("server session: the welcome carries the host's options")
{
	{
		Fixture f;
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(!welcome->friendlyFire);
		CHECK(!welcome->sharedStory);
	}
	{
		Server::Options options;
		options.friendlyFire = true;
		Fixture f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(welcome->friendlyFire);
		CHECK(!welcome->sharedStory);
	}
	{
		Server::Options options;
		options.sharedStory = true;
		Fixture f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(!welcome->friendlyFire);
		CHECK(welcome->sharedStory);
	}
}

TEST("server session: a welcomed player gets the world state")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	// Join consumed the welcome; the world state follows it.
	CHECK(f.fake->WaitFor(1, Type(MT::kWorldState)));
}

TEST("server session: a connection that never says hello is dropped")
{
	Fixture f;
	f.fake->Connect(1);
	const auto deadline = std::chrono::steady_clock::now() + 8s;
	while (!f.fake->Disconnected(1) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(50ms);
	}
	CHECK(f.fake->Disconnected(1));
	CHECK(f.Logged("dropping fake 1 (no hello)"));
}

TEST("server session: nothing from a connection is relayed before its hello")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Take(1);

	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(Status(5), MT::kReportStatus));
	f.fake->Deliver(2, Protocol::Encode(State(1)));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Heartbeat{ 42 }, MT::kHeartbeat));
	CHECK(Settle(f, 1).empty());
	CHECK(f.fake->Take(2).empty());
}

// ---- joining and leaving -----------------------------------------------------------------

TEST("server session: players are told about each other when they join")
{
	Fixture f;
	auto aliceHello = Fixture::MakeHello("Alice");
	aliceHello.appearance = 0xA1;
	auto bobHello = Fixture::MakeHello("Bob");
	bobHello.appearance = 0xB0;
	const auto alice = f.Join(1, aliceHello);
	REQUIRE(alice);
	// Alone: nobody to hear about.
	CHECK(OfType(f.fake->Take(1), MT::kPlayerJoined).empty());

	const auto bob = f.Join(2, bobHello);
	REQUIRE(bob);

	const auto toAlice = f.fake->WaitFor(1, Type(MT::kPlayerJoined));
	REQUIRE(toAlice);
	const auto joinedBob = Protocol::DecodePlayerJoined(*toAlice);
	REQUIRE(joinedBob);
	CHECK(joinedBob->playerId == bob->playerId);
	CHECK(joinedBob->name == "Bob");
	CHECK(joinedBob->appearance == 0xB0);

	const auto toBob = f.fake->WaitFor(2, Type(MT::kPlayerJoined));
	REQUIRE(toBob);
	const auto joinedAlice = Protocol::DecodePlayerJoined(*toBob);
	REQUIRE(joinedAlice);
	CHECK(joinedAlice->playerId == alice->playerId);
	CHECK(joinedAlice->name == "Alice");
	CHECK(joinedAlice->appearance == 0xA1);

	// Exactly one each, and nobody hears about themselves.
	CHECK(OfType(Settle(f, 1), MT::kPlayerJoined).empty());
	CHECK(OfType(f.fake->Take(2), MT::kPlayerJoined).empty());
}

TEST("server session: a newcomer hears about every player already in")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice);
	REQUIRE(bob);
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(carol);

	std::vector<std::uint32_t> heard;
	for (const auto& packet : OfType(Settle(f, 3), MT::kPlayerJoined)) {
		heard.push_back(Protocol::DecodePlayerJoined(packet)->playerId);
	}
	std::ranges::sort(heard);
	std::vector<std::uint32_t> expected{ alice->playerId, bob->playerId };
	std::ranges::sort(expected);
	CHECK(heard == expected);
	// And both of them heard about Carol.
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));  // Bob, earlier
	const auto aliceHeard = f.fake->WaitFor(1, Type(MT::kPlayerJoined));
	REQUIRE(aliceHeard);
	CHECK(Protocol::DecodePlayerJoined(*aliceHeard)->playerId == carol->playerId);
	const auto bobHeard = f.fake->WaitFor(2, Type(MT::kPlayerJoined));  // Alice, then Carol
	REQUIRE(bobHeard);
	CHECK(Protocol::DecodePlayerJoined(*bobHeard)->playerId == alice->playerId);
	const auto bobHeardCarol = f.fake->WaitFor(2, Type(MT::kPlayerJoined));
	REQUIRE(bobHeardCarol);
	CHECK(Protocol::DecodePlayerJoined(*bobHeardCarol)->playerId == carol->playerId);
}

TEST("server session: a newcomer gets the others' equipment and status after hearing about them")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice);
	REQUIRE(bob);
	// Alice has gear and a status; Bob has neither.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Equipment{ 0, { 0x111, 0x222 } }, MT::kEquipment));
	f.fake->Deliver(1, Protocol::Encode(Status(12), MT::kReportStatus));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerStatus)));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	const auto packets = Settle(f, 3);

	const auto equipment = OfType(packets, MT::kPlayerEquipment);
	REQUIRE(equipment.size() == 1);
	const auto gear = Protocol::DecodeEquipment(equipment[0]);
	REQUIRE(gear);
	CHECK(gear->playerId == alice->playerId);
	CHECK((gear->items == std::vector<std::uint32_t>{ 0x111, 0x222 }));

	const auto statuses = OfType(packets, MT::kPlayerStatus);
	REQUIRE(statuses.size() == 1);
	const auto status = Protocol::DecodePlayerStatus(statuses[0]);
	REQUIRE(status);
	CHECK(status->playerId == alice->playerId);
	CHECK(status->level == 12);

	// The newcomer knows who Alice is before getting her gear and status.
	const auto first = [&](MT a_type, std::uint32_t a_joinedId = 0) {
		for (std::size_t i = 0; i < packets.size(); ++i) {
			if (packets[i][0] == Type(a_type) &&
				(a_type != MT::kPlayerJoined || Protocol::DecodePlayerJoined(packets[i])->playerId == a_joinedId)) {
				return i;
			}
		}
		return packets.size();
	};
	const auto joined = first(MT::kPlayerJoined, alice->playerId);
	CHECK(joined < packets.size());
	CHECK(joined < first(MT::kPlayerEquipment));
	CHECK(joined < first(MT::kPlayerStatus));
}

TEST("server session: the others are told when a player leaves")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(alice);
	REQUIRE(bob);
	REQUIRE(carol);
	f.fake->Take(1);
	f.fake->Take(3);

	f.fake->Drop(2);
	for (const PeerId peer : { 1u, 3u }) {
		const auto left = f.fake->WaitFor(peer, Type(MT::kPlayerLeft));
		REQUIRE(left);
		CHECK(Protocol::DecodePlayerLeft(*left)->playerId == bob->playerId);
	}
	CHECK(f.Logged("'Bob' left"));
	// Only once.
	CHECK(OfType(Settle(f, 1), MT::kPlayerLeft).empty());
}

TEST("server session: a connection that leaves before its hello is never announced")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Take(1);
	f.fake->Connect(2);
	f.fake->Drop(2);
	auto old = Fixture::MakeHello("Mallory");
	old.version = Protocol::VERSION - 1;
	REQUIRE(JoinRejected(f, 3, Protocol::Encode(old)));
	const auto packets = Settle(f, 1);
	CHECK(OfType(packets, MT::kPlayerLeft).empty());
	CHECK(OfType(packets, MT::kPlayerJoined).empty());
}

TEST("server session: names lose control characters; an empty name becomes Player N")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("\x01" "Al\ti\nce\x7F"));
	REQUIRE(alice);
	const auto nameless = f.Join(2, Fixture::MakeHello("\x01\x02\x1F"));
	REQUIRE(nameless);
	const auto empty = f.Join(3, Fixture::MakeHello(""));
	REQUIRE(empty);

	std::vector<Protocol::PlayerJoined> heard;
	for (const auto& packet : OfType(Settle(f, 1), MT::kPlayerJoined)) {
		heard.push_back(*Protocol::DecodePlayerJoined(packet));
	}
	REQUIRE(heard.size() == 2);
	std::ranges::sort(heard, {}, &Protocol::PlayerJoined::playerId);
	CHECK(heard[0].playerId == nameless->playerId);
	CHECK(heard[0].name == std::format("Player {}", nameless->playerId));
	CHECK(heard[1].name == std::format("Player {}", empty->playerId));

	const auto toNameless = OfType(f.fake->Take(2), MT::kPlayerJoined);
	REQUIRE(!toNameless.empty());
	CHECK(Protocol::DecodePlayerJoined(toNameless[0])->name == "Alice");
	CHECK(f.Logged("'Alice' joined as player"));
}

// ---- identities --------------------------------------------------------------------------

TEST("server session: players without an identity never get an old ID back")
{
	Fixture f;
	const auto first = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(first);
	f.fake->Drop(1);
	const auto again = f.Join(2, Fixture::MakeHello("Alice"));
	REQUIRE(again);
	CHECK(again->playerId != first->playerId);
	// Two at once without identities don't replace each other either.
	const auto bob = f.Join(3, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	CHECK(bob->playerId != again->playerId);
	CHECK(!f.fake->Disconnected(2));
}

TEST("server session: different identities get different IDs and each gets its own back")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice", 0xA11CE));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob", 0xB0B));
	REQUIRE(alice);
	REQUIRE(bob);
	CHECK(alice->playerId != bob->playerId);
	CHECK(!f.fake->Disconnected(1));

	f.fake->Drop(1);
	f.fake->Drop(2);
	// Back in the opposite order.
	const auto bobAgain = f.Join(3, Fixture::MakeHello("Bob", 0xB0B));
	const auto aliceAgain = f.Join(4, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(bobAgain);
	REQUIRE(aliceAgain);
	CHECK(bobAgain->playerId == bob->playerId);
	CHECK(aliceAgain->playerId == alice->playerId);
	CHECK(f.Logged("'Bob' came back as player"));
}

TEST("server session: a returning player without a name is named after their old ID")
{
	Fixture f;
	const auto first = f.Join(1, Fixture::MakeHello("", 0xA11CE));
	REQUIRE(first);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Drop(1);
	f.fake->Take(2);
	const auto again = f.Join(3, Fixture::MakeHello("", 0xA11CE));
	REQUIRE(again);
	CHECK(again->playerId == first->playerId);
	const auto joined = f.fake->WaitFor(2, Type(MT::kPlayerJoined));
	REQUIRE(joined);
	CHECK(Protocol::DecodePlayerJoined(*joined)->name == std::format("Player {}", first->playerId));
}

// A hello that is turned away must not count as the player coming back: anyone who knows (or
// guesses) a player's identity could otherwise kick them with a bad hello.
TEST("server session: a rejected hello with a player's identity doesn't drop that player")
{
	Server::Options options;
	options.password = "pw";
	Fixture f{ options };
	auto aliceHello = Fixture::MakeHello("Alice", 0xA11CE);
	aliceHello.password = "pw";
	auto bobHello = Fixture::MakeHello("Bob", 0xB0B);
	bobHello.password = "pw";
	const auto alice = f.Join(1, aliceHello);
	const auto bob = f.Join(2, bobHello);
	REQUIRE(alice);
	REQUIRE(bob);
	f.fake->Take(1);
	f.fake->Take(2);

	auto wrongPassword = aliceHello;
	wrongPassword.password = "nope";
	auto wrongMods = aliceHello;
	wrongMods.contentHash = 0x9999;
	auto wrongVersion = aliceHello;
	wrongVersion.version = Protocol::VERSION + 1;
	REQUIRE(JoinRejected(f, 3, Protocol::Encode(wrongPassword)));
	REQUIRE(JoinRejected(f, 4, Protocol::Encode(wrongMods)));
	REQUIRE(JoinRejected(f, 5, Protocol::Encode(wrongVersion)));

	CHECK(!f.fake->Disconnected(1));
	CHECK(!f.Logged("dropping the old connection"));
	CHECK(OfType(Settle(f, 2), MT::kPlayerLeft).empty());

	// Alice is still playing: her status still reaches Bob.
	f.fake->Deliver(1, Protocol::Encode(Status(3), MT::kReportStatus));
	const auto status = f.fake->WaitFor(2, Type(MT::kPlayerStatus));
	REQUIRE(status);
	CHECK(Protocol::DecodePlayerStatus(*status)->playerId == alice->playerId);

	// And her real comeback still gets her ID.
	const auto again = f.Join(6, aliceHello);
	REQUIRE(again);
	CHECK(again->playerId == alice->playerId);
}

TEST("server session: a rejected hello on a player's own connection makes them leave")
{
	Server::Options options;
	options.password = "pw";
	Fixture f{ options };
	auto aliceHello = Fixture::MakeHello("Alice", 0xA11CE);
	aliceHello.password = "pw";
	auto bobHello = Fixture::MakeHello("Bob", 0xB0B);
	bobHello.password = "pw";
	const auto alice = f.Join(1, aliceHello);
	REQUIRE(alice);
	REQUIRE(f.Join(2, bobHello));
	f.fake->Take(2);

	auto wrong = aliceHello;
	wrong.password = "nope";
	f.fake->Deliver(1, Protocol::Encode(wrong));
	REQUIRE(WaitReject(f, 1));
	CHECK(f.fake->Disconnected(1));
	const auto left = f.fake->WaitFor(2, Type(MT::kPlayerLeft));
	REQUIRE(left);
	CHECK(Protocol::DecodePlayerLeft(*left)->playerId == alice->playerId);
	// Told once, not again when the connection closes.
	CHECK(OfType(Settle(f, 2), MT::kPlayerLeft).empty());
}

TEST("server session: hello again on the same connection takes the new name and restarts the state sequence")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice", 0xA11CE));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Protocol::Encode(State(500)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerStates)));
	f.fake->Take(2);

	// Their game restarted: new name, and its state samples count from 1 again.
	f.fake->Deliver(1, Protocol::Encode(Fixture::MakeHello("Alicia", 0xA11CE)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kWelcome)));
	const auto joined = f.fake->WaitFor(2, Type(MT::kPlayerJoined));
	REQUIRE(joined);
	CHECK(Protocol::DecodePlayerJoined(*joined)->name == "Alicia");

	f.fake->Deliver(1, Protocol::Encode(State(1, 42.0f)));
	const auto states = StatesOf(Settle(f, 2), alice->playerId);
	REQUIRE(!states.empty());
	CHECK(states.back().sequence == 1);
	CHECK(states.back().x == 42.0f);
}

// ---- player state ------------------------------------------------------------------------

TEST("server session: player states go to the others, not back to the sender")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(alice);
	REQUIRE(bob);
	REQUIRE(carol);

	f.fake->Deliver(1, Protocol::Encode(State(1, 10.0f)));
	for (const PeerId peer : { 2u, 3u }) {
		const auto packet = f.fake->WaitFor(peer, Type(MT::kPlayerStates));
		REQUIRE(packet);
		const auto batch = Protocol::DecodePlayerStates(*packet);
		REQUIRE(batch);
		REQUIRE(batch->players.size() == 1);
		CHECK(batch->players[0].playerId == alice->playerId);
		CHECK(batch->players[0].state.x == 10.0f);
	}
	CHECK(OfType(Settle(f, 1), MT::kPlayerStates).empty());
	// Sent once: nothing new, nothing sent.
	CHECK(StatesOf(f.fake->Take(2), alice->playerId).empty());
}

TEST("server session: older and repeated player states are dropped")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));

	f.fake->Deliver(1, Protocol::Encode(State(5, 5.0f)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerStates)));
	f.fake->Take(2);

	f.fake->Deliver(1, Protocol::Encode(State(3, 3.0f)));
	f.fake->Deliver(1, Protocol::Encode(State(5, 55.0f)));
	CHECK(StatesOf(Settle(f, 2), alice->playerId).empty());

	f.fake->Deliver(1, Protocol::Encode(State(6, 6.0f)));
	const auto states = StatesOf(Settle(f, 2), alice->playerId);
	REQUIRE(states.size() == 1);
	CHECK(states[0].sequence == 6);
	CHECK(states[0].x == 6.0f);
}

TEST("server session: a newcomer sees players who are standing still")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Deliver(1, Protocol::Encode(State(9, 99.0f)));
	std::this_thread::sleep_for(100ms);

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerStates));
	REQUIRE(packet);
	const auto batch = Protocol::DecodePlayerStates(*packet);
	REQUIRE(batch);
	REQUIRE(batch->players.size() == 1);
	CHECK(batch->players[0].playerId == alice->playerId);
	CHECK(batch->players[0].state.sequence == 9);
}

TEST("server session: a state that doesn't decode is ignored")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	auto bad = State(1);
	bad.x = std::numeric_limits<float>::quiet_NaN();
	f.fake->Deliver(1, Protocol::Encode(bad));
	auto truncated = Protocol::Encode(State(2));
	truncated.pop_back();
	f.fake->Deliver(1, truncated);
	CHECK(OfType(Settle(f, 2), MT::kPlayerStates).empty());
}

// ---- player status -----------------------------------------------------------------------

TEST("server session: status goes to the others under the sender's ID")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice);
	REQUIRE(bob);
	f.fake->Take(1);

	auto status = Status(20, 75);
	status.playerId = bob->playerId;  // pretending to be Bob doesn't work
	status.downed = true;
	f.fake->Deliver(1, Protocol::Encode(status, MT::kReportStatus));
	const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerStatus));
	REQUIRE(packet);
	const auto relayed = Protocol::DecodePlayerStatus(*packet);
	REQUIRE(relayed);
	CHECK(relayed->playerId == alice->playerId);
	CHECK(relayed->level == 20);
	CHECK(relayed->health == 75);
	CHECK(relayed->downed);
	CHECK(OfType(Settle(f, 1), MT::kPlayerStatus).empty());
}

TEST("server session: a late joiner gets each player's latest status")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Deliver(1, Protocol::Encode(Status(1), MT::kReportStatus));
	f.fake->Deliver(1, Protocol::Encode(Status(2), MT::kReportStatus));
	f.fake->Deliver(1, Protocol::Encode(Status(3, 50), MT::kReportStatus));
	std::this_thread::sleep_for(100ms);

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto statuses = OfType(Settle(f, 2), MT::kPlayerStatus);
	REQUIRE(statuses.size() == 1);
	const auto status = Protocol::DecodePlayerStatus(statuses[0]);
	REQUIRE(status);
	CHECK(status->playerId == alice->playerId);
	CHECK(status->level == 3);
	CHECK(status->health == 50);
}

TEST("server session: an invalid status is ignored")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	f.fake->Deliver(1, Protocol::Encode(Status(1, 101), MT::kReportStatus));  // over 100%
	CHECK(OfType(Settle(f, 2), MT::kPlayerStatus).empty());

	// ... and isn't kept for late joiners either.
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	CHECK(OfType(Settle(f, 3), MT::kPlayerStatus).empty());
}

// ---- world time --------------------------------------------------------------------------

TEST("server session: without a local player, the lowest ID's clock is used")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice);
	REQUIRE(bob);
	REQUIRE(alice->playerId < bob->playerId);
	f.fake->Take(1);
	f.fake->Take(2);

	f.fake->Deliver(2, TimeReport(8.0f));
	CHECK(OfType(Settle(f, 1), MT::kWorldTime).empty());

	f.fake->Deliver(1, TimeReport(13.5f));
	const auto packet = f.fake->WaitFor(2, Type(MT::kWorldTime));
	REQUIRE(packet);
	const auto time = Protocol::DecodeWorldTime(*packet);
	REQUIRE(time);
	CHECK(time->playerId == alice->playerId);
	CHECK(time->gameHour == 13.5f);
	CHECK(time->weather == 0x1234);
	CHECK(OfType(Settle(f, 1), MT::kWorldTime).empty());
}

TEST("server session: a player on the server's machine owns the clock")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->SetLocal(2);
	const auto host = f.Join(2, Fixture::MakeHello("Host"));
	REQUIRE(host);
	REQUIRE(alice->playerId < host->playerId);
	f.fake->Take(1);
	f.fake->Take(2);

	// Alice has the lower ID, but the host is local.
	f.fake->Deliver(1, TimeReport(8.0f));
	CHECK(OfType(Settle(f, 2), MT::kWorldTime).empty());

	f.fake->Deliver(2, TimeReport(20.0f));
	const auto packet = f.fake->WaitFor(1, Type(MT::kWorldTime));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWorldTime(*packet)->playerId == host->playerId);
	CHECK(Protocol::DecodeWorldTime(*packet)->gameHour == 20.0f);
}

TEST("server session: when the clock's owner leaves, the next player's clock is used")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(alice);
	REQUIRE(bob);
	REQUIRE(carol);
	f.fake->Drop(1);
	f.fake->Deliver(2, TimeReport(6.0f));
	const auto packet = f.fake->WaitFor(3, Type(MT::kWorldTime));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWorldTime(*packet)->playerId == bob->playerId);
}

TEST("server session: a newcomer gets the session's time on joining")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Deliver(1, TimeReport(17.25f));
	std::this_thread::sleep_for(50ms);

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto packet = f.fake->WaitFor(2, Type(MT::kWorldTime));
	REQUIRE(packet);
	const auto time = Protocol::DecodeWorldTime(*packet);
	REQUIRE(time);
	CHECK(time->playerId == alice->playerId);
	CHECK(time->gameHour == 17.25f);
	CHECK(time->worldspace == 0x3C);
}

TEST("server session: the session's time is forgotten when everyone has left")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Deliver(1, TimeReport(17.25f));
	std::this_thread::sleep_for(50ms);
	f.fake->Drop(1);

	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	CHECK(!f.fake->WaitFor(2, Type(MT::kWorldTime), 300ms));
}

TEST("server session: a time report that doesn't decode is ignored")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, TimeReport(25.0f));  // no such hour
	CHECK(OfType(Settle(f, 2), MT::kWorldTime).empty());
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	CHECK(!f.fake->WaitFor(3, Type(MT::kWorldTime), 200ms));
}

// ---- XP ----------------------------------------------------------------------------------

TEST("server session: XP is shared with the others under the sender's ID")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Take(1);

	f.fake->Deliver(1, Protocol::Encode(Protocol::XpGain{ 999, 40.0f }, MT::kReportXp));
	for (const PeerId peer : { 2u, 3u }) {
		const auto packet = f.fake->WaitFor(peer, Type(MT::kPartyXp));
		REQUIRE(packet);
		const auto gain = Protocol::DecodeXpGain(*packet);
		REQUIRE(gain);
		CHECK(gain->playerId == alice->playerId);
		CHECK(gain->xp == 40.0f);
	}
	CHECK(OfType(Settle(f, 1), MT::kPartyXp).empty());
}

TEST("server session: shared XP is capped by the player's budget")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, XpReport(6000.0f));
	f.fake->Deliver(1, XpReport(6000.0f));
	f.fake->Deliver(1, XpReport(Protocol::MAX_XP_SHARE));
	const auto xp = XpOf(Settle(f, 2));
	REQUIRE(xp.size() >= 2);
	CHECK(xp[0] == 6000.0f);
	// The rest of the budget (plus what refilled in a moment, 1000 a second).
	CHECK(xp[1] >= 4000.0f);
	CHECK(xp[1] < 4500.0f);
	float total = 0.0f;
	for (const auto x : xp) {
		total += x;
	}
	CHECK(total < Protocol::MAX_XP_SHARE + 500.0f);
}

TEST("server session: shared XP reports are limited to a burst")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	for (int i = 0; i < 15; ++i) {
		f.fake->Deliver(1, XpReport(5.0f));
	}
	const auto xp = XpOf(Settle(f, 2));
	// 10 at once, one more every half second.
	CHECK(xp.size() >= 10);
	CHECK(xp.size() <= 11);
}

TEST("server session: XP from a connection without a hello is not shared")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	f.fake->Take(1);
	f.fake->Connect(2);
	f.fake->Deliver(2, XpReport(100.0f));
	CHECK(OfType(Settle(f, 1), MT::kPartyXp).empty());
}

// ---- pings, lines, heartbeats ------------------------------------------------------------

TEST("server session: pings are passed on at most once a second")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(1);

	Protocol::Ping ping;
	ping.worldspace = 0x3C;
	ping.x = 1.0f;
	ping.y = 2.0f;
	ping.z = 3.0f;
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	auto pinged = OfType(Settle(f, 2), MT::kPinged);
	REQUIRE(pinged.size() == 1);
	const auto relayed = Protocol::DecodePing(pinged[0]);
	REQUIRE(relayed);
	CHECK(relayed->playerId == alice->playerId);
	CHECK(relayed->x == 1.0f);
	CHECK(relayed->worldspace == 0x3C);
	CHECK(OfType(f.fake->Take(1), MT::kPinged).empty());

	std::this_thread::sleep_for(900ms);
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	CHECK(OfType(Settle(f, 2), MT::kPinged).size() == 1);
}

TEST("server session: spoken lines are passed on at most four times a second")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Line{ 0, 0, "Hello there" }, MT::kReportLine));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Line{ 0, 0, "Too soon" }, MT::kReportLine));
	const auto lines = OfType(Settle(f, 2), MT::kLineSpoken);
	REQUIRE(lines.size() == 1);
	const auto line = Protocol::DecodeLine(lines[0]);
	REQUIRE(line);
	CHECK(line->playerId == alice->playerId);
	CHECK(line->text == "Hello there");

	std::this_thread::sleep_for(100ms);
	f.fake->Deliver(1, Protocol::Encode(Protocol::Line{ 0, 0, "Later" }, MT::kReportLine));
	CHECK(OfType(Settle(f, 2), MT::kLineSpoken).size() == 1);
}

TEST("server session: a heartbeat is answered with the same token, to the sender only")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::Heartbeat{ 0xC0FFEE }, MT::kHeartbeat));
	const auto ack = f.fake->WaitFor(1, Type(MT::kHeartbeatAck));
	REQUIRE(ack);
	const auto beat = Protocol::DecodeHeartbeat(*ack);
	REQUIRE(beat);
	CHECK(beat->token == 0xC0FFEE);
	CHECK(OfType(Settle(f, 2), MT::kHeartbeatAck).empty());
}

TEST("server session: heartbeats are answered at most ten times a second")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	for (std::uint32_t i = 0; i < 15; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::Heartbeat{ i }, MT::kHeartbeat));
	}
	const auto acks = OfType(Settle(f, 1), MT::kHeartbeatAck);
	CHECK(acks.size() >= 10);
	CHECK(acks.size() < 15);
	REQUIRE(!acks.empty());
	CHECK(Protocol::DecodeHeartbeat(acks[0])->token == 0);
}

TEST("server session: heartbeats don't use up the event budget")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	// The event budget is spent...
	for (int i = 0; i < 520; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Status(static_cast<std::uint16_t>(i)), MT::kReportStatus));
	}
	// ... but heartbeats are still answered.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Heartbeat{ 77 }, MT::kHeartbeat));
	const auto ack = f.fake->WaitFor(1, Type(MT::kHeartbeatAck));
	REQUIRE(ack);
	CHECK(Protocol::DecodeHeartbeat(*ack)->token == 77);
}

// ---- rate limits -------------------------------------------------------------------------

TEST("server session: a player flooding events loses the excess, and it's logged")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	constexpr int sent = 700;
	for (int i = 0; i < sent; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Status(static_cast<std::uint16_t>(i)), MT::kReportStatus));
	}
	const auto relayed = OfType(Settle(f, 2, 400ms), MT::kPlayerStatus);
	CHECK(!relayed.empty());
	CHECK(relayed.size() < static_cast<std::size_t>(sent));
	CHECK(relayed.size() <= 500 + 100);  // 500 a second (a window could roll over mid-flood)
	CHECK(f.Logged("'Alice' sends too many events"));
	// Bob, who didn't flood, still gets through.
	f.fake->Deliver(2, Protocol::Encode(Status(1), MT::kReportStatus));
	CHECK(f.fake->WaitFor(1, Type(MT::kPlayerStatus)));
}

TEST("server session: a player flooding states loses the excess")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	// 200 states at once: only the first 60 a second count, so the last one is never seen.
	for (std::uint32_t i = 1; i <= 200; ++i) {
		f.fake->Deliver(1, Protocol::Encode(State(i, static_cast<float>(i))));
	}
	const auto states = StatesOf(Settle(f, 2), alice->playerId);
	REQUIRE(!states.empty());
	CHECK(states.back().sequence < 200);
}
