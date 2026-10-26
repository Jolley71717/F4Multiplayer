// The session server's lobby and session logic: who gets in, what they are told about each
// other, and the per-player relays (state, status, clock, XP, pings, heartbeats, rate limits).

#include "ServerFixture.h"
#include "FormIds.h"
#include "SessionFile.h"
#include "net/ModDiff.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <string>
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

	// The plugins that differ as a joiner's game with a_mine names them (src/net/ModDiff.h): from the last list of the
	// session's that a_peer was sent (kLoadOrder), the host's ignored plugins left out. None when no list came.
	std::optional<ModDiff::Difference> ModsTold(Fixture& a_f, PeerId a_peer, const std::vector<std::string>& a_mine)
	{
		const auto orders = Decoded(a_f.fake->Take(a_peer), MT::kLoadOrder, [](auto a_data) { return Protocol::DecodeLoadOrder(a_data); });
		if (orders.empty()) {
			return std::nullopt;
		}
		return ModDiff::Compare(a_mine, orders.back().plugins, orders.back().ignored);
	}

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
	CHECK(f.fake->WaitDisconnected(1));
	CHECK(OfType(f.fake->Take(1), MT::kWelcome).empty());
}

TEST("server session: a truncated hello is rejected")
{
	Fixture f;
	const auto full = Protocol::Encode(Fixture::MakeHello("Alice"));
	const auto reject = JoinRejected(f, 1, std::vector<std::uint8_t>(full.begin(), full.begin() + 5));
	REQUIRE(reject);
	CHECK(Contains(reject->reason, "Not an F4Multiplayer client"));
	CHECK(f.fake->WaitDisconnected(1));
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
	CHECK(f.fake->WaitDisconnected(1));
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
	CHECK(f.fake->WaitDisconnected(1));
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
	CHECK(f.fake->WaitDisconnected(3));
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
	CHECK(f.fake->WaitDisconnected(2));
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

TEST("server session: the welcome carries the host's session rules, the defaults when it set none")
{
	{
		Fixture    f;
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(welcome->rules == Protocol::SessionRules{});
		CHECK(welcome->rules.revive);
		CHECK(welcome->rules.bleedOutSeconds == 45);
		CHECK(welcome->rules.realTimeVats);
		CHECK(!welcome->rules.downedCrawl);
		CHECK(!welcome->rules.shareGuestBuilds);
	}
	{
		Server::Options options;
		options.rules.maxRevives = 2;
		options.rules.reviveWindowMinutes = 10;
		options.rules.downCooldownSeconds = 60;
		options.rules.reviveOutsideCombat = true;
		options.rules.downedCrawl = true;  // --downed-crawl
		options.rules.realTimeVats = false;
		options.rules.shareGuestBuilds = true;
		options.rules.bleedOutSeconds = 9999;  // past the limit: brought within it
		Fixture    f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(welcome->rules.maxRevives == 2);
		CHECK(welcome->rules.reviveWindowMinutes == 10);
		CHECK(welcome->rules.downCooldownSeconds == 60);
		CHECK(welcome->rules.reviveOutsideCombat);
		CHECK(welcome->rules.downedCrawl);
		CHECK(!welcome->rules.realTimeVats);
		CHECK(welcome->rules.shareGuestBuilds);
		CHECK(welcome->rules.bleedOutSeconds == Protocol::SessionRules::MAX_BLEED_OUT);
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
	CHECK(f.fake->WaitDisconnected(1));
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
	// Alice may still get the server's own periodic messages (time, her own ack); nothing of the
	// stranger's status or state may reach her.
	const auto toAlice = Settle(f, 1);
	CHECK(OfType(toAlice, MT::kPlayerStatus).empty());
	CHECK(OfType(toAlice, MT::kPlayerStates).empty());
	CHECK(OfType(toAlice, MT::kPlayerJoined).empty());
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
	CHECK(f.Logged(std::format("player {} left", bob->playerId)));
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
	CHECK(f.Logged(std::format("player {} joined from fake 1", alice->playerId)));
}

namespace
{
	// UTF-8 spelled out in bytes, so the file's own encoding plays no part.
	const std::string CYRILLIC = "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82";  // Нейт
	const std::string CJK = "\xE5\x86\x85\xE7\x89\xB9";              // 内特
	const std::string EMOJI = "\xF0\x9F\x98\x80";                    // 😀

	// The names the PlayerJoined packets among a_packets carry, by player ID.
	std::map<std::uint32_t, std::string> JoinedNames(const Packets& a_packets)
	{
		std::map<std::uint32_t, std::string> out;
		for (const auto& packet : OfType(a_packets, MT::kPlayerJoined)) {
			const auto joined = Protocol::DecodePlayerJoined(packet);
			REQUIRE(joined);
			out[joined->playerId] = joined->name;
		}
		return out;
	}

	std::vector<Protocol::Rename> Renames(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kRenamed, [](auto a_data) { return Protocol::DecodeRename(a_data); });
	}

	std::vector<std::uint8_t> RenameTo(std::string a_name)
	{
		return Protocol::Encode(Protocol::Rename{ 0, std::move(a_name) }, MT::kRename);
	}
}

TEST("server session: a name in another script is kept whole, and so is one of emoji")
{
	Fixture f;
	const auto cyrillic = f.Join(1, Fixture::MakeHello(CYRILLIC));
	const auto cjk = f.Join(2, Fixture::MakeHello(CJK));
	const auto emoji = f.Join(3, Fixture::MakeHello(EMOJI));
	const auto mixed = f.Join(4, Fixture::MakeHello("Nate " + CYRILLIC + " " + CJK + EMOJI));
	REQUIRE(cyrillic && cjk && emoji && mixed);

	const auto heard = JoinedNames(Settle(f, 1));
	REQUIRE(heard.size() == 3);
	CHECK(heard.at(cjk->playerId) == CJK);
	CHECK(heard.at(emoji->playerId) == EMOJI);
	CHECK(heard.at(mixed->playerId) == "Nate " + CYRILLIC + " " + CJK + EMOJI);
	const auto toCjk = JoinedNames(f.fake->Take(2));
	CHECK(toCjk.at(cyrillic->playerId) == CYRILLIC);
}

TEST("server session: markup, bytes that aren't UTF-8 and the invisible characters are dropped from a name; the cap counts characters")
{
	Server::Options options;
	options.maxPlayers = 8;  // five names to try (the default room is four)
	Fixture        f{ options };
	const auto markup = f.Join(1, Fixture::MakeHello("<b>Nate</b>"));
	const auto broken = f.Join(2, Fixture::MakeHello("Na\xFF" "te\xD0"));
	const auto reversed = f.Join(3, Fixture::MakeHello("\xE2\x80\xAE" "etaN"));  // a right-to-left override
	std::string longCyrillic;
	for (int i = 0; i < 10; ++i) {
		longCyrillic += CYRILLIC;  // 40 characters, 80 bytes
	}
	const auto capped = f.Join(4, Fixture::MakeHello(longCyrillic));
	const auto onlyBad = f.Join(5, Fixture::MakeHello("<\xFF>"));
	REQUIRE(markup && broken && reversed && capped && onlyBad);

	const auto heard = JoinedNames(Settle(f, 5));
	REQUIRE(heard.size() == 4);
	CHECK(heard.at(markup->playerId) == "bNate/b");
	CHECK(heard.at(broken->playerId) == "Nate");
	CHECK(heard.at(reversed->playerId) == "etaN");
	CHECK(heard.at(capped->playerId) == longCyrillic.substr(0, 64));  // 32 characters, whole ones
	const auto toMarkup = JoinedNames(f.fake->Take(1));
	CHECK(toMarkup.at(onlyBad->playerId) == std::format("Player {}", onlyBad->playerId));
}

TEST("server session: a player's new name reaches the others, cleaned, and a later joiner hears it too")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("SteamAlice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice && bob);
	Clear(f, { 1, 2 });

	// Alice named her character in the opening after connecting: the others see the character's name.
	f.fake->Deliver(1, RenameTo(CYRILLIC));
	auto toBob = Renames(Received(f, 2));
	REQUIRE(toBob.size() == 1);
	CHECK(toBob[0].playerId == alice->playerId);
	CHECK(toBob[0].name == CYRILLIC);
	CHECK(Renames(Received(f, 1)).empty());  // not echoed to her
	CHECK(f.Logged(std::format("player {} renamed", alice->playerId)));
	CHECK(!f.Logged(CYRILLIC));  // the log never holds a name

	// The same name again changes nothing.
	std::this_thread::sleep_for(1100ms);
	f.fake->Deliver(1, RenameTo(CYRILLIC));
	CHECK(Renames(Received(f, 2)).empty());

	// A name with markup is cleaned as a hello's; one nothing is left of becomes Player N.
	std::this_thread::sleep_for(1100ms);
	f.fake->Deliver(1, RenameTo("<b>" + CYRILLIC + "</b> Jr"));
	toBob = Renames(Received(f, 2));
	REQUIRE(toBob.size() == 1);
	CHECK(toBob[0].name == "b" + CYRILLIC + "/b Jr");
	std::this_thread::sleep_for(1100ms);
	f.fake->Deliver(1, RenameTo("\x01\x02"));
	toBob = Renames(Received(f, 2));
	REQUIRE(toBob.size() == 1);
	CHECK(toBob[0].name == std::format("Player {}", alice->playerId));

	// Whoever joins now hears the name she has now.
	std::this_thread::sleep_for(1100ms);
	f.fake->Deliver(1, RenameTo("Nora"));
	Sync(f, 1);
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(carol);
	const auto toCarol = JoinedNames(Received(f, 3));
	CHECK(toCarol.at(alice->playerId) == "Nora");
	CHECK(toCarol.at(bob->playerId) == "Bob");
}

TEST("server session: a name changes twice a second at most, the last one sent stays, and not before the hello")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(alice && bob);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, RenameTo("One"));
	// Right after the first (the network brought two of her game's changes together): it waits, and the last one
	// sent is the one that stays, for the others and for later joiners.
	f.fake->Deliver(1, RenameTo("Two"));
	f.fake->Deliver(1, RenameTo("Three"));
	auto toBob = Renames(Received(f, 2));
	REQUIRE(toBob.size() == 1);
	CHECK(toBob[0].name == "One");
	const auto later = f.fake->WaitFor(2, Type(MT::kRenamed));
	REQUIRE(later);
	const auto three = Protocol::DecodeRename(*later);
	REQUIRE(three);
	CHECK(three->playerId == alice->playerId);
	CHECK(three->name == "Three");  // "Two" never went out: a newer name took its place
	std::this_thread::sleep_for(700ms);
	CHECK(Renames(Received(f, 2)).empty());
	const auto carol = f.Join(3, Fixture::MakeHello("Carol"));
	REQUIRE(carol);
	CHECK(JoinedNames(Received(f, 3)).at(alice->playerId) == "Three");

	// A flood reaches the others as two renames a second at most.
	for (int i = 0; i < 50; ++i) {
		f.fake->Deliver(1, RenameTo(std::format("Flood {}", i)));
	}
	std::this_thread::sleep_for(1200ms);
	toBob = Renames(Received(f, 2));
	CHECK(toBob.size() >= 1);
	CHECK(toBob.size() <= 3);
	REQUIRE(!toBob.empty());
	CHECK(toBob.back().name == "Flood 49");

	// A connection that never said hello has no name to change and nobody to tell.
	f.fake->Connect(4);
	f.fake->Deliver(4, RenameTo("Nobody"));
	std::this_thread::sleep_for(600ms);  // past the floor: nothing was kept for later either
	CHECK(Renames(Received(f, 2)).empty());
	CHECK(!f.fake->Disconnected(4));
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
	CHECK(f.Logged(std::format("player {} came back from fake 3", bob->playerId)));
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
	CHECK(f.fake->WaitDisconnected(1));
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

TEST("server session: the host's own game owns the clock")
{
	constexpr std::uint64_t HOST = 0x1111;
	Server::Options         options;
	options.hostIdentity = HOST;
	Fixture    f{ options };
	// Alice connected first (IDs go by connection), but a hosted server lets nobody in before the host's game.
	f.fake->Connect(1);
	f.fake->SetLocal(2);
	const auto host = f.Join(2, Fixture::MakeHello("Host", HOST));
	REQUIRE(host);
	f.fake->Deliver(1, Protocol::Encode(Fixture::MakeHello("Alice")));
	const auto welcome = f.fake->WaitFor(1, Type(MT::kWelcome));
	REQUIRE(welcome);
	const auto alice = Protocol::DecodeWelcome(*welcome);
	REQUIRE(alice);
	REQUIRE(alice->playerId < host->playerId);
	f.fake->Take(1);
	f.fake->Take(2);

	// Alice has the lower ID, but the host is the host.
	f.fake->Deliver(1, TimeReport(8.0f));
	CHECK(OfType(Settle(f, 2), MT::kWorldTime).empty());

	f.fake->Deliver(2, TimeReport(20.0f));
	const auto packet = f.fake->WaitFor(1, Type(MT::kWorldTime));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWorldTime(*packet)->playerId == host->playerId);
	CHECK(Protocol::DecodeWorldTime(*packet)->gameHour == 20.0f);
}

TEST("server session: another game on the server's machine, or the host's identity from elsewhere, isn't the host")
{
	constexpr std::uint64_t HOST = 0x1111;
	Server::Options         options;
	options.hostIdentity = HOST;
	Fixture    f{ options };
	// The host's game lets the others in, then goes (the others carry on). Alice connected before it, so
	// she has the lowest ID (IDs go by connection).
	f.fake->Connect(1);
	f.fake->SetLocal(9);
	REQUIRE(f.Join(9, Fixture::MakeHello("Host", HOST)));
	f.fake->Deliver(1, Protocol::Encode(Fixture::MakeHello("Alice")));
	const auto welcome = f.fake->WaitFor(1, Type(MT::kWelcome));
	REQUIRE(welcome);
	const auto alice = Protocol::DecodeWelcome(*welcome);
	REQUIRE(alice);
	f.fake->SetLocal(2);
	const auto bot = f.Join(2, Fixture::MakeHello("Bot", 0x2222));  // a bot, or a tunnel to another PC
	REQUIRE(bot);
	f.fake->Drop(9);
	Settle(f, 9);
	const auto faker = f.Join(3, Fixture::MakeHello("Faker", HOST));  // not on this machine
	REQUIRE(faker);
	f.fake->Take(1);
	f.fake->Take(2);
	f.fake->Take(3);

	// Neither is the host, so Alice, here longest, owns the clock.
	f.fake->Deliver(2, TimeReport(20.0f));
	f.fake->Deliver(3, TimeReport(21.0f));
	CHECK(OfType(Settle(f, 1), MT::kWorldTime).empty());
	f.fake->Deliver(1, TimeReport(8.0f));
	const auto packet = f.fake->WaitFor(2, Type(MT::kWorldTime));
	REQUIRE(packet);
	CHECK(Protocol::DecodeWorldTime(*packet)->playerId == alice->playerId);
}

TEST("server session: a dedicated server has no host")
{
	Fixture    f;  // no host identity
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->SetLocal(2);
	const auto local = f.Join(2, Fixture::MakeHello("Local", 0x1111));
	REQUIRE(local);
	f.fake->Take(1);
	f.fake->Take(2);

	f.fake->Deliver(2, TimeReport(20.0f));
	CHECK(OfType(Settle(f, 1), MT::kWorldTime).empty());
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

TEST("server session: the host's own game isn't sent the saved time; a friend is")
{
	constexpr std::uint64_t HOST = 0x1111;
	const TempSessionFile file;
	const auto&           path = file.path;
	SessionFile::Snapshot saved;
	saved.contentHash = 0x1234;
	saved.sessionId = 0x5E55;
	Protocol::WorldTime time;
	time.gameHour = 9.0f;
	saved.worldTime = time;
	REQUIRE(SessionFile::Save(path, saved));

	Server::Options options;
	options.hostIdentity = HOST;
	options.sessionFile = path.string();
	{
		Fixture f{ options };
		f.fake->SetLocal(1);
		REQUIRE(f.Join(1, Fixture::MakeHello("Host", HOST)));
		CHECK(!f.fake->WaitFor(1, Type(MT::kWorldTime), 300ms));  // its own clock is the session's

		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		const auto packet = f.fake->WaitFor(2, Type(MT::kWorldTime));
		REQUIRE(packet);
		CHECK(Protocol::DecodeWorldTime(*packet)->gameHour == 9.0f);
	}
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

TEST("server session: a ping pressed a second after the last that arrives a little early is passed on too")
{
	// The game spaces pings a second apart by key press (PingRules::MIN_GAP); the network brings one sooner or later.
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(1);

	Protocol::Ping ping;
	ping.worldspace = 0x3C;
	const auto first = std::chrono::steady_clock::now();
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	REQUIRE(OfType(Settle(f, 2), MT::kPinged).size() == 1);

	// The next, its press a second later, comes 150 ms early (the first was slow on the way).
	std::this_thread::sleep_until(first + 1s - 150ms);
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	CHECK(OfType(Settle(f, 2), MT::kPinged).size() == 1);

	// One sooner than the server's gap is still dropped.
	f.fake->Deliver(1, Protocol::Encode(ping, MT::kPing));
	CHECK(OfType(Settle(f, 2), MT::kPinged).empty());
}

TEST("server session: a ping reaches the others with what it names, as the sender's, and a malformed one goes nowhere")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(1);

	// A spot with an NPC's id is no ping: dropped, and it does not spend the second's ping.
	Protocol::Ping bad{ 77, 0, 0x3C, 1.0f, 2.0f, 3.0f, Protocol::PingKind::kSpot, 0x0001F3A2, 0, "" };
	f.fake->Deliver(1, Protocol::Encode(bad, MT::kPing));
	Protocol::Ping raider{ 77, 0, 0x3C, 1.0f, 2.0f, 3.0f, Protocol::PingKind::kActor, 0xFF000123, 0x0001B56D, "Raider" };
	f.fake->Deliver(1, Protocol::Encode(raider, MT::kPing));
	const auto pinged = OfType(Settle(f, 2), MT::kPinged);
	REQUIRE(pinged.size() == 1);
	const auto relayed = Protocol::DecodePing(pinged[0]);
	REQUIRE(relayed);
	CHECK(relayed->playerId == alice->playerId);  // not the 77 it claimed
	CHECK(relayed->kind == Protocol::PingKind::kActor);
	CHECK(relayed->target == 0xFF000123);  // the spawn id: every game finds its own copy of the NPC by it
	CHECK(relayed->base == 0x0001B56D);
	CHECK(relayed->name == "Raider");
	CHECK(OfType(f.fake->Take(1), MT::kPinged).empty());
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
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
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
	CHECK(f.Logged(std::format("player {} sends too many events", alice->playerId)));
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

TEST("server session: a joiner with other mods is let in, and the list before the welcome names the plugins that differ")
{
	Fixture f;
	auto    alice = Fixture::MakeHello("Alice");
	alice.plugins = { "Fallout4.esm", "DLCCoast.esm" };
	REQUIRE(f.Join(1, alice));
	f.fake->Take(1);

	auto bob = Fixture::MakeHello("Bob");
	bob.contentHash = 0x9999;
	bob.plugins = { "Fallout4.esm", "DLCCoast.esm", "Extra.esp" };
	REQUIRE(f.Join(2, bob));
	CHECK(f.Logged("their form IDs are translated by plugin name"));
	const auto told = ModsTold(f, 2, bob.plugins);
	REQUIRE(told);
	CHECK(told->extra == std::vector<std::string>{ "Extra.esp" });
	CHECK(told->Count() == 1u);
}

TEST("server session: a hello without a plugin list (a bot, a test tool) still needs the session's load order")
{
	Fixture f;
	auto    alice = Fixture::MakeHello("Alice");
	alice.plugins = { "Fallout4.esm", "DLCCoast.esm" };
	REQUIRE(f.Join(1, alice));

	auto bot = Fixture::MakeHello("Bot");
	bot.contentHash = 0x9999;
	bot.plugins.clear();
	const auto reject = JoinRejected(f, 2, Protocol::Encode(bot));
	REQUIRE(reject);
	CHECK(reject->reason.starts_with(Protocol::REJECT_LOAD_ORDER));

	// One with the session's hash and no list gets in on the hash, as before: nothing to translate for it.
	auto same = Fixture::MakeHello("Same");
	same.contentHash = alice.contentHash;
	same.plugins.clear();
	REQUIRE(f.Join(3, same));
}

TEST("server session: the session's plugin table comes before the welcome, grows by a joiner's other plugins, and goes to everyone")
{
	Fixture f;
	auto    host = Fixture::MakeHello("Host");
	host.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "DLCCoast.esm", "Story.esp" }, { "Light.esl" });
	f.fake->Connect(1);
	f.fake->Deliver(1, Protocol::Encode(host));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kWelcome)));
	auto tables = Decoded(f.fake->Take(1), MT::kPluginTable, [](auto a_data) { return Protocol::DecodePluginTable(a_data); });
	REQUIRE(tables.size() == 1u);
	CHECK(tables[0].plugins == host.plugins);

	// Bob has the host's mods in another order, and two of his own: the table grows by those, after the rest of their kind.
	auto bob = Fixture::MakeHello("Bob");
	bob.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "Story.esp", "Mine.esp", "DLCCoast.esm" }, { "MyLight.esl", "Light.esl" });
	bob.contentHash = Protocol::LoadOrderHash(bob.plugins);
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(bob));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWelcome)));
	const auto grown = Protocol::ComparedLoadOrder({ "Fallout4.esm", "DLCCoast.esm", "Story.esp", "Mine.esp" }, { "Light.esl", "MyLight.esl" });
	tables = Decoded(f.fake->Take(2), MT::kPluginTable, [](auto a_data) { return Protocol::DecodePluginTable(a_data); });
	REQUIRE(tables.size() == 1u);
	CHECK(tables[0].plugins == grown);
	CHECK(f.Logged("the session's plugin table has 6 plugins (2 new)"));
	// The host hears of it too: Bob's forms are numbered by it from now on.
	tables = Decoded(Settle(f, 1), MT::kPluginTable, [](auto a_data) { return Protocol::DecodePluginTable(a_data); });
	REQUIRE(tables.size() == 1u);
	CHECK(tables[0].plugins == grown);

	// Carol brings nothing new: only she is sent the table.
	auto carol = Fixture::MakeHello("Carol");
	carol.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "DLCCoast.esm" }, {});
	carol.contentHash = Protocol::LoadOrderHash(carol.plugins);
	REQUIRE(f.Join(3, carol));
	CHECK(Count(Settle(f, 1), MT::kPluginTable) == 0u);
}

TEST("server session: a joiner's appearance is the session's form, translated from their own numbering")
{
	Fixture f;
	auto    host = Fixture::MakeHello("Host");
	host.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "A.esp", "B.esp" }, {});
	host.contentHash = Protocol::LoadOrderHash(host.plugins);
	REQUIRE(f.Join(1, host));
	f.fake->Take(1);

	auto bob = Fixture::MakeHello("Bob");
	bob.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "B.esp", "A.esp" }, {});
	bob.contentHash = Protocol::LoadOrderHash(bob.plugins);
	bob.appearance = 0x01000ABC;  // B.esp's NPC in his game: the host's 02000ABC
	REQUIRE(f.Join(2, bob));
	const auto joined = Decoded(Settle(f, 1), MT::kPlayerJoined, [](auto a_data) { return Protocol::DecodePlayerJoined(a_data); });
	REQUIRE(joined.size() == 1u);
	CHECK(joined[0].appearance == 0x02000ABCu);
}

TEST("server session: a load order that differs only in the host's ignored plugins is accepted")
{
	Server::Options options;
	options.ignoredPlugins = { "Textures.esp" };
	Fixture f{ options };
	auto    alice = Fixture::MakeHello("Alice");
	alice.plugins = { "Fallout4.esm", "Textures.esp" };
	REQUIRE(f.Join(1, alice));
	f.fake->Take(1);

	auto bob = Fixture::MakeHello("Bob");
	bob.contentHash = 0x9999;
	bob.plugins = { "Fallout4.esm" };
	REQUIRE(f.Join(2, bob));
	CHECK(f.Logged("ignored plugins"));
	// Their game, told the host's ignored plugins with the list, finds nothing to change.
	const auto bobTold = ModsTold(f, 2, bob.plugins);
	REQUIRE(bobTold);
	CHECK(bobTold->Empty());

	auto carol = Fixture::MakeHello("Carol");
	carol.contentHash = 0x7777;
	carol.plugins = { "Fallout4.esm", "Other.esp" };
	REQUIRE(f.Join(3, carol));  // in too (their form IDs are translated), and told what differs
	const auto told = ModsTold(f, 3, carol.plugins);
	REQUIRE(told);
	CHECK(told->extra == std::vector<std::string>{ "Other.esp" });
	CHECK(told->missing.empty());  // not the host's texture plugin, which loads last
	CHECK(told->Count() == 1u);
}

TEST("server session: a joiner whose load order differs is sent the session's, and its plugin table, before the welcome, and one whose matches only the table")
{
	Server::Options options;
	options.ignoredPlugins = { "Textures.esp" };
	Fixture f{ options };
	auto    host = Fixture::MakeHello("Host");
	host.plugins = { "Fallout4.esm", "DLCCoast.esm", "Story.esp", std::string{ Protocol::LIGHT_PLUGINS }, "Light.esl", "Textures.esp" };
	REQUIRE(f.Join(1, host));
	CHECK(Count(Received(f, 1), MT::kLoadOrder) == 0u);  // the first decides: nothing to compare

	// In: the list first, then the plugin table, then the welcome, all reliable on the same channel.
	auto bob = Fixture::MakeHello("Bob");
	bob.contentHash = 0x9999;
	bob.plugins = { "Fallout4.esm", "DLCCoast.esm", "Extra.esp" };
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(bob));
	std::vector<std::vector<std::uint8_t>> toBob;
	REQUIRE(Await(f, 2, MT::kWelcome, toBob));
	const auto at = [&](MT a_type) {
		return std::ranges::find_if(toBob, [&](const auto& a_packet) { return !a_packet.empty() && a_packet[0] == Type(a_type); }) - toBob.begin();
	};
	CHECK(at(MT::kLoadOrder) < at(MT::kPluginTable));
	CHECK(at(MT::kPluginTable) < at(MT::kWelcome));
	const auto orders = Decoded(toBob, MT::kLoadOrder, [](auto a_data) { return Protocol::DecodeLoadOrder(a_data); });
	REQUIRE(orders.size() == 1u);
	CHECK(orders[0].plugins == host.plugins);
	CHECK(orders[0].ignored == options.ignoredPlugins);  // so their game leaves them out as the server does
	CHECK(!f.fake->Disconnected(2));

	// Let in with only the host's ignored plugin different: the list comes with the welcome.
	auto carol = Fixture::MakeHello("Carol");
	carol.contentHash = 0x7777;
	carol.plugins = { "Fallout4.esm", "DLCCoast.esm", "Story.esp", std::string{ Protocol::LIGHT_PLUGINS }, "Light.esl" };
	REQUIRE(f.Join(3, carol));
	const auto toCarol = Received(f, 3);
	REQUIRE(Count(toCarol, MT::kLoadOrder) == 1u);

	// The same hash as the host's: no list.
	auto dave = Fixture::MakeHello("Dave");
	dave.plugins = host.plugins;
	REQUIRE(f.Join(4, dave));
	const auto toDave = Received(f, 4);
	CHECK(Count(toDave, MT::kLoadOrder) == 0u);
	CHECK(Count(toDave, MT::kPluginTable) == 1u);
}

TEST("server session: a player in the session gets the session's load order when they ask, once in a while")
{
	Fixture f;
	auto    host = Fixture::MakeHello("Host");
	host.plugins = { "Fallout4.esm", "Story.esp" };
	REQUIRE(f.Join(1, host));
	auto bob = Fixture::MakeHello("Bob");
	bob.plugins = host.plugins;
	REQUIRE(f.Join(2, bob));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, Protocol::Encode(Protocol::LoadOrderRequest{}));
	f.fake->Deliver(2, Protocol::Encode(Protocol::LoadOrderRequest{}));  // again at once: not answered twice
	const auto got = Received(f, 2);
	const auto orders = Decoded(got, MT::kLoadOrder, [](auto a_data) { return Protocol::DecodeLoadOrder(a_data); });
	REQUIRE(orders.size() == 1u);
	CHECK(orders[0].plugins == host.plugins);
	CHECK(Count(Received(f, 1), MT::kLoadOrder) == 0u);  // only the one who asked

	// A connection that hasn't been let in hears nothing.
	f.fake->Connect(3);
	f.fake->Deliver(3, Protocol::Encode(Protocol::LoadOrderRequest{}));
	CHECK(!f.fake->WaitFor(3, Type(MT::kLoadOrder), std::chrono::milliseconds(200)));
}

namespace
{
	// A game's plugins as the engine loads them: the full ones (the HD texture pack among the game's own, when
	// installed) and the light ones.
	struct LoadOrder
	{
		std::vector<std::string> full;
		std::vector<std::string> light;
	};

	// The hello that game's Session sends: its load order as compared, and that list's hash.
	Protocol::Hello HelloWith(std::string a_name, const LoadOrder& a_game)
	{
		auto hello = Fixture::MakeHello(std::move(a_name));
		hello.plugins = Protocol::ComparedLoadOrder(a_game.full, a_game.light);
		hello.contentHash = Protocol::LoadOrderHash(hello.plugins);
		return hello;
	}

	// The first byte of the form IDs the game gives a plugin's records (FE for a light plugin, whose place among
	// the light ones is in the next 12 bits), or nullopt when the game hasn't it.
	std::optional<std::uint32_t> PrefixOf(const LoadOrder& a_game, std::string_view a_plugin)
	{
		for (std::size_t i = 0; i < a_game.full.size(); ++i) {
			if (Protocol::SamePluginName(a_game.full[i], a_plugin)) {
				return static_cast<std::uint32_t>(i) << 24;
			}
		}
		for (std::size_t i = 0; i < a_game.light.size(); ++i) {
			if (Protocol::SamePluginName(a_game.light[i], a_plugin)) {
				return 0xFE000000u | (static_cast<std::uint32_t>(i) << 12);
			}
		}
		return std::nullopt;
	}

	// Whether a mod's NPC, quest or topic is the same form in both games: every plugin both have gets the same form IDs.
	bool SameFormIds(const LoadOrder& a_one, const LoadOrder& a_other)
	{
		for (const auto& list : { a_one.full, a_one.light }) {
			for (const auto& plugin : list) {
				const auto theirs = PrefixOf(a_other, plugin);
				if (theirs && *theirs != *PrefixOf(a_one, plugin)) {
					return false;
				}
			}
		}
		return true;
	}

	const std::vector<std::string> OFFICIAL{ "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm", "DLCNukaWorld.esm" };

	std::vector<std::string> Official(std::initializer_list<std::string> a_after)
	{
		auto out = OFFICIAL;
		out.insert(out.end(), a_after);
		return out;
	}
}

TEST("server session: with a story mod, a friend without the host's HD texture pack gets in, told of no difference, and the mod's NPC is translated")
{
	// The report: a story mod and its companion mod after the game's own files. With the texture pack the mod's NPC
	// is 09000004, without it 08000004.
	const LoadOrder host{ Official({ "DLCUltraHighResolution.esm", "AmericaRising2.esp", "DanielTheRemnant.esp" }), { "ccbgsfo4044-hellfirepowerarmor.esl" } };
	const LoadOrder withoutPack{ Official({ "AmericaRising2.esp", "DanielTheRemnant.esp" }), host.light };
	CHECK(*PrefixOf(host, "DanielTheRemnant.esp") == 0x09000000u);
	CHECK(*PrefixOf(withoutPack, "DanielTheRemnant.esp") == 0x08000000u);
	CHECK(!SameFormIds(host, withoutPack));

	Fixture f;
	REQUIRE(f.Join(1, HelloWith("Host", host)));
	f.fake->Take(1);
	REQUIRE(f.Join(2, HelloWith("Friend", withoutPack)));
	const auto told = ModsTold(f, 2, HelloWith("Friend", withoutPack).plugins);
	REQUIRE(told);
	// The pack holds no records, so the friend's notice doesn't name it (src/net/ModDiff.h).
	CHECK(told->missing.empty());
	CHECK(told->Count() == 0u);
	// The NPC their game numbers 08000004 is the session's (the host's) 09000004, and back.
	const FormIds::Mapper friendForms(HelloWith("Friend", withoutPack).plugins, HelloWith("Host", host).plugins);
	CHECK(friendForms.ToSession(0x08000004) == 0x09000004u);
	CHECK(friendForms.ToLocal(0x09000004) == 0x08000004u);
	CHECK(!friendForms.ToLocal(0x07000001));  // the pack holds no records, but nothing of it is theirs anyway

	// A friend with the pack too has the host's load order.
	CHECK(SameFormIds(host, host));
	REQUIRE(f.Join(3, HelloWith("Friend", host)));
}

TEST("server session: without mods after it, the HD texture pack still makes no difference: the friend without it gets in")
{
	const LoadOrder host{ Official({ "DLCUltraHighResolution.esm" }), { "ccbgsfo4044-hellfirepowerarmor.esl" } };
	const LoadOrder withoutPack{ OFFICIAL, host.light };
	CHECK(SameFormIds(host, withoutPack));
	CHECK(HelloWith("Host", host).contentHash == HelloWith("Friend", withoutPack).contentHash);

	Fixture f;
	REQUIRE(f.Join(1, HelloWith("Host", host)));
	REQUIRE(f.Join(2, HelloWith("Friend", withoutPack)));
}

TEST("server session: a plugin the host lets differ that moves other plugins' form IDs is named to the friend, who still gets in")
{
	Server::Options options;
	options.maxPlayers = 8;
	options.ignoredPlugins = { "Textures.esp", "LightTextures.esl" };
	Fixture f{ options };
	// The host's texture plugin loads before the story mod: the story mod's forms are 09 in the host's game.
	const LoadOrder host{ Official({ "Textures.esp", "Story.esp" }), { "LightTextures.esl", "Weapons.esl" } };
	REQUIRE(f.Join(1, HelloWith("Host", host)));
	f.fake->Take(1);

	// Without the texture plugin, the story mod's forms are 08: in (they are translated), and told it is missing.
	const LoadOrder without{ Official({ "Story.esp" }), host.light };
	CHECK(!SameFormIds(host, without));
	REQUIRE(f.Join(2, HelloWith("Bob", without)));
	auto told = ModsTold(f, 2, HelloWith("Bob", without).plugins);
	REQUIRE(told);
	CHECK(told->missing == std::vector<std::string>{ "Textures.esp" });
	CHECK(told->Count() == 1u);

	// With it, but after the story mod: in too, told it moved.
	const LoadOrder after{ Official({ "Story.esp", "Textures.esp" }), host.light };
	CHECK(!SameFormIds(host, after));
	REQUIRE(f.Join(3, HelloWith("Carol", after)));
	told = ModsTold(f, 3, HelloWith("Carol", after).plugins);
	REQUIRE(told);
	REQUIRE(told->moved.size() == 1u);
	CHECK(told->moved[0].name == "Textures.esp");
	CHECK(told->Count() == 1u);

	// A light texture plugin missing before a light weapons plugin moves that one's form IDs the same way.
	const LoadOrder withoutLight{ host.full, { "Weapons.esl" } };
	CHECK(!SameFormIds(host, withoutLight));
	REQUIRE(f.Join(4, HelloWith("Dave", withoutLight)));
	told = ModsTold(f, 4, HelloWith("Dave", withoutLight).plugins);
	REQUIRE(told);
	CHECK(told->missing == std::vector<std::string>{ "LightTextures.esl" });
	CHECK(told->Count() == 1u);

	// The same load order gets in.
	REQUIRE(f.Join(5, HelloWith("Erin", host)));
}

TEST("server session: texture plugins the host lets differ that load last, full or light, let a friend without them in")
{
	Server::Options options;
	options.ignoredPlugins = { "Textures.esp", "LightTextures.esl" };
	Fixture f{ options };
	const LoadOrder host{ Official({ "Story.esp", "Textures.esp" }), { "Weapons.esl", "LightTextures.esl" } };
	REQUIRE(f.Join(1, HelloWith("Host", host)));

	// Nothing after either: every plugin both have is the same forms in both games.
	const LoadOrder without{ Official({ "Story.esp" }), { "Weapons.esl" } };
	CHECK(SameFormIds(host, without));
	REQUIRE(f.Join(2, HelloWith("Bob", without)));
	CHECK(f.Logged("ignored plugins"));
	// And their game, with the list that came, has nothing to tell them to change: no "your mods differ".
	const auto bobTold = ModsTold(f, 2, HelloWith("Bob", without).plugins);
	REQUIRE(bobTold);
	CHECK(bobTold->Empty());

	// A full texture plugin last moves no light plugin: they are numbered apart.
	const LoadOrder fullOnly{ Official({ "Story.esp" }), host.light };
	CHECK(SameFormIds(host, fullOnly));
	REQUIRE(f.Join(3, HelloWith("Carol", fullOnly)));
	const auto carolTold = ModsTold(f, 3, HelloWith("Carol", fullOnly).plugins);
	REQUIRE(carolTold);
	CHECK(carolTold->Empty());
}

TEST("server session file: a host whose HD texture pack now counts keeps the world saved by an older version; another load order still starts fresh")
{
	// The host's game: the pack, then a story mod. Older versions left the pack out of the hash wherever it loaded.
	const LoadOrder host{ Official({ "DLCUltraHighResolution.esm", "Story.esp" }), { "Weapons.esl" } };
	const auto      oldHash = Protocol::LoadOrderHash(Protocol::ComparedLoadOrder(Official({ "Story.esp" }), host.light));
	REQUIRE(HelloWith("Host", host).contentHash != oldHash);
	CHECK(Protocol::LoadOrderHashBeforeTexturePack(HelloWith("Host", host).plugins) == oldHash);
	CHECK(!Protocol::LoadOrderHashBeforeTexturePack({}));

	constexpr std::uint32_t STORY_NPC = 0x08000004;
	const auto saveOld = [&](const TempSessionFile& a_file) {
		SessionFile::Snapshot saved;
		saved.contentHash = oldHash;
		saved.sessionId = 0x5E55;
		saved.deadActors = { STORY_NPC };
		REQUIRE(SessionFile::Save(a_file.path, saved));
	};
	{
		const TempSessionFile file;
		saveOld(file);
		Server::Options options;
		options.sessionFile = file.path.string();
		Fixture    f{ options };
		const auto welcome = f.Join(1, HelloWith("Host", host));
		REQUIRE(welcome);
		CHECK(welcome->sessionId == 0x5E55);
		CHECK(f.Logged("hashed as older versions did"));
		CHECK(!f.Logged("load order changed"));
		const auto world = WorldStates(Received(f, 1));
		REQUIRE(world.size() == 1);
		CHECK(std::ranges::contains(world[0].deadActors, STORY_NPC));

		// A friend on the new version with the host's load order joins on the hash; one without the pack is let in too, translated.
		REQUIRE(f.Join(2, HelloWith("Friend", host)));
		REQUIRE(f.Join(3, HelloWith("Other", { Official({ "Story.esp" }), host.light })));
	}
	{
		// A load order whose old hash is not the saved one is another load order: a fresh session.
		const TempSessionFile file;
		saveOld(file);
		Server::Options options;
		options.sessionFile = file.path.string();
		Fixture    f{ options };
		const auto welcome = f.Join(1, HelloWith("Host", { Official({ "DLCUltraHighResolution.esm", "Other.esp" }), host.light }));
		REQUIRE(welcome);
		CHECK(welcome->sessionId != 0x5E55);
		CHECK(f.Logged("load order changed"));
	}
}

TEST("server session file: a session saved with its plugin table carries on when the host's mods change order, numbered as saved")
{
	const LoadOrder before{ Official({ "A.esp", "B.esp" }), { "Weapons.esl" } };
	const LoadOrder after{ Official({ "B.esp", "New.esp", "A.esp" }), { "Weapons.esl" } };
	constexpr std::uint32_t B_NPC = 0x08000801;  // B.esp's, as the saved table numbers it
	const TempSessionFile   file;
	{
		SessionFile::Snapshot saved;
		saved.contentHash = HelloWith("Host", before).contentHash;
		saved.sessionId = 0x5E55;
		saved.deadActors = { B_NPC };
		saved.plugins = HelloWith("Host", before).plugins;
		REQUIRE(SessionFile::Save(file.path, saved));
	}
	Server::Options options;
	options.sessionFile = file.path.string();
	Fixture f{ options };
	f.fake->Connect(1);
	f.fake->Deliver(1, Protocol::Encode(HelloWith("Host", after)));
	std::vector<std::vector<std::uint8_t>> got;
	REQUIRE(Await(f, 1, MT::kWelcome, got));
	const auto welcome = Decoded(got, MT::kWelcome, [](auto a_data) { return Protocol::DecodeWelcome(a_data); });
	REQUIRE(welcome.size() == 1u);
	CHECK(welcome[0].sessionId == 0x5E55);
	CHECK(!f.Logged("load order changed"));
	// The table is the saved one with the new plugin added after the rest: the saved IDs mean what they meant.
	const auto tables = Decoded(got, MT::kPluginTable, [](auto a_data) { return Protocol::DecodePluginTable(a_data); });
	REQUIRE(tables.size() == 1u);
	CHECK(tables[0].plugins == HelloWith("Host", { Official({ "A.esp", "B.esp", "New.esp" }), { "Weapons.esl" } }).plugins);
	std::ranges::move(Received(f, 1), std::back_inserter(got));
	const auto world = WorldStates(got);
	REQUIRE(world.size() == 1);
	CHECK(std::ranges::contains(world[0].deadActors, B_NPC));
	// In the host's game now, that NPC is 07000801.
	CHECK(FormIds::Mapper(HelloWith("Host", after).plugins, tables[0].plugins).ToLocal(B_NPC) == 0x07000801u);
}
