// What the server writes to its log while real players come and go. Players attach that log (the
// host's game log holds it) to public bug reports, so it must never name a player, hold a password,
// a whole IP address or a whole SteamID64, while still telling a maintainer who did what by player
// ID. The players themselves still see each other's names: only the log is redacted.

#include "ServerFixture.h"

#include "Net.h"
#include "Redact.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
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
	// Not even as an alias: the name is not an account, and a name has no place in the log at all.
	CHECK(NowhereInLog(f, Redact::Alias(76561198087654321ull)));
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

TEST("log privacy: joiners whose load order differs only in an ignored texture pack get in, and the line saying so names neither their name nor a Steam id")
{
	Server::Options options;
	options.ignoredPlugins = { "Textures.esp" };
	Fixture f{ options };

	// The host's game has a texture pack the others don't.
	auto hostHello = Fixture::MakeHello("Host");
	hostHello.plugins = { "Fallout4.esm", "Textures.esp" };
	const auto host = f.Join(1, hostHello);
	REQUIRE(host);

	// Two joiners without it (so another load order hash): one over UDP whose name is a forged server
	// line, one over Steam whose name is someone else's SteamID64.
	auto forgerHello = Fixture::MakeHello("Zed\nserver: player 1 left", 0xF0F0);
	forgerHello.contentHash = 0x5678;
	forgerHello.plugins = { "Fallout4.esm" };
	auto squatterHello = forgerHello;
	squatterHello.name = "76561198087654321";
	squatterHello.identity = 0x5A5A;
	f.fake->SetAddress(2, "udp 198.51.100.7:4321");
	f.fake->SetAddress(3, std::format("steam {}", STEAM_ID));
	const auto forger = f.Join(2, forgerHello);
	const auto squatter = f.Join(3, squatterHello);
	REQUIRE(forger);
	REQUIRE(squatter);
	std::this_thread::sleep_for(100ms);

	// The host still hears them by name.
	const auto heard = NamesHeard(f, 1);
	CHECK(heard.contains(forger->playerId) && heard.at(forger->playerId).starts_with("Zed"));
	CHECK(heard.contains(squatter->playerId) && heard.at(squatter->playerId) == "76561198087654321");

	// The log says which connection it was, masked, and nothing they chose.
	CHECK(f.Logged("load order differs only in ignored plugins (udp 198.51.100.x:4321)"));
	CHECK(f.Logged(std::format("load order differs only in ignored plugins (steam {})", Redact::Alias(STEAM_ID))));
	for (const std::string_view secret : { "Zed", "76561198087654321", "8087654321", "76561198012345678", "198.51.100.7" }) {
		CHECK(NowhereInLog(f, secret));
	}
	CHECK(NowhereInLog(f, Redact::Alias(76561198087654321ull)));
	CHECK(!f.Logged(std::format("player {} left", host->playerId)));
	const auto lines = f.LogLines();
	CHECK(std::ranges::none_of(lines, [](const std::string& a_line) { return a_line.find_first_of("\r\n") != std::string::npos; }));
}

TEST("log privacy: a host whose plugin names carry a line break or a Steam id refuses a joiner, and the joiner's log gets one clean line")
{
	Fixture f;

	// The host's game (the first to arrive sets the session's load order) has a plugin whose file
	// name holds a line break and a fake "connected" line, and one named after a SteamID64.
	auto hostHello = Fixture::MakeHello("Host");
	hostHello.plugins = { "Fallout4.esm", "Evil.esp\r\nSession: Multiplayer: connected (shared story)", "76561198012345678.esp" };
	REQUIRE(f.Join(1, hostHello));

	// A joiner with only the base game is refused, and told what they are missing.
	auto joinerHello = Fixture::MakeHello("Joiner");
	joinerHello.contentHash = 0x5678;
	joinerHello.plugins = { "Fallout4.esm" };
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(joinerHello));
	const auto packet = f.fake->WaitFor(2, Type(MT::kReject));
	REQUIRE(packet);
	const auto reject = Protocol::DecodeReject(*packet);
	REQUIRE(reject);
	CHECK(reject->reason.find("You are missing: Evil.esp") != std::string::npos);
	// The joiner's game drops control characters as it reads any text off the wire.
	CHECK(reject->reason.find_first_of("\r\n") == std::string::npos);

	// The line the joiner's game writes (Session::Notify, then the log sink's Scrub): one line, the
	// refusal and not a forged "connected", and an alias where the Steam id was.
	const auto line = Redact::Scrub("Session: " + Redact::OneLine("Multiplayer: " + reject->reason));
	CHECK(line.find_first_of("\r\n") == std::string::npos);
	CHECK(line.starts_with("Session: Multiplayer: Your load order is different"));
	CHECK(line.find("76561198012345678") == std::string::npos);
	CHECK(line.find(Redact::Alias(STEAM_ID) + ".esp") != std::string::npos);

	// The server's own log (the host's) does not echo the reason or the joiner's name.
	CHECK(NowhereInLog(f, "Evil.esp"));
	CHECK(NowhereInLog(f, "Joiner"));

	// A host running a changed server can send any refusal it likes. The joiner's game still writes
	// one line from it, whether the text came off the wire or (a message of its own) did not.
	const std::string sent = "Wrong password\r\nSession: Multiplayer: connected (shared story)\x7F";
	const auto        forged = Protocol::DecodeReject(Protocol::Encode(Protocol::Reject{ sent }));
	REQUIRE(forged);
	for (const auto& reason : { forged->reason, sent }) {
		CHECK(Redact::Scrub("Session: " + Redact::OneLine("Multiplayer: " + reason)) ==
			  "Session: Multiplayer: Wrong passwordSession: Multiplayer: connected (shared story)");
	}
}

namespace
{
	// A player's game on this machine, connected to the server's real UDP transport (ENet) the way
	// the host's own game and the bot connect.
	class UdpPlayer
	{
	public:
		explicit UdpPlayer(std::uint16_t a_port)
		{
			initialized = Net::Initialize();
			host = initialized ? enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0) : nullptr;
			if (host) {
				ENetAddress address{};
				enet_address_set_host_ip(&address, "127.0.0.1");
				address.port = a_port;
				peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
			}
		}

		~UdpPlayer()
		{
			if (peer) {
				enet_peer_reset(peer);
			}
			if (host) {
				enet_host_destroy(host);
			}
			if (initialized) {
				Net::Shutdown();
			}
		}

		UdpPlayer(const UdpPlayer&) = delete;
		UdpPlayer& operator=(const UdpPlayer&) = delete;

		// Services the connection until a packet of a_type arrives (or a_timeout passes).
		std::optional<std::vector<std::uint8_t>> WaitFor(std::uint8_t a_type, std::chrono::milliseconds a_timeout = 3000ms)
		{
			const auto until = std::chrono::steady_clock::now() + a_timeout;
			while (host && std::chrono::steady_clock::now() < until) {
				ENetEvent event;
				while (enet_host_service(host, &event, 10) > 0) {
					if (event.type == ENET_EVENT_TYPE_CONNECT) {
						connected = true;
					} else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
						connected = false;
						peer = nullptr;
					} else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
						std::vector<std::uint8_t> data{ event.packet->data, event.packet->data + event.packet->dataLength };
						enet_packet_destroy(event.packet);
						if (!data.empty() && data[0] == a_type) {
							return data;
						}
					}
				}
			}
			return std::nullopt;
		}

		bool Connect()
		{
			const auto until = std::chrono::steady_clock::now() + 3s;
			while (peer && !connected && std::chrono::steady_clock::now() < until) {
				WaitFor(0xFF, 20ms);
			}
			return connected;
		}

		void Send(const std::vector<std::uint8_t>& a_packet)
		{
			if (peer) {
				Net::Send(peer, a_packet, true);
				enet_host_flush(host);
			}
		}

		// Quits the way a game does: tells the server, and waits for it to agree.
		void Leave()
		{
			if (!peer) {
				return;
			}
			enet_peer_disconnect(peer, 0);
			const auto until = std::chrono::steady_clock::now() + 3s;
			while (peer && std::chrono::steady_clock::now() < until) {
				WaitFor(0xFF, 20ms);
			}
		}

	private:
		bool      initialized = false;
		ENetHost* host = nullptr;
		ENetPeer* peer = nullptr;
		bool      connected = false;
	};

	// Waits for the server's log to have a line holding a_text.
	bool LoggedSoon(Fixture& a_f, std::string_view a_text)
	{
		const auto until = std::chrono::steady_clock::now() + 2s;
		while (!a_f.Logged(a_text) && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(10ms);
		}
		return a_f.Logged(a_text);
	}
}

TEST("log privacy: over the real UDP transport, a wrong password, a join and a quit are logged by address and player number, never by name")
{
	Server::Options options;
	options.password = "hunter2";
	Fixture f{ options };
	REQUIRE(f.udpPort != 0);

	// Someone on this machine guesses the password and is turned away.
	{
		UdpPlayer guesser{ f.udpPort };
		REQUIRE(guesser.Connect());
		auto guess = Fixture::MakeHello("Mallory");
		guess.password = "letmein";
		guesser.Send(Protocol::Encode(guess));
		REQUIRE(guesser.WaitFor(Type(MT::kReject)));
	}

	// Alice's game joins and quits.
	UdpPlayer alice{ f.udpPort };
	REQUIRE(alice.Connect());
	auto hello = Fixture::MakeHello("Alice", 0xA11CE);
	hello.password = "hunter2";
	alice.Send(Protocol::Encode(hello));
	const auto packet = alice.WaitFor(Type(MT::kWelcome));
	REQUIRE(packet);
	const auto welcome = Protocol::DecodeWelcome(*packet);
	REQUIRE(welcome);
	alice.Leave();

	// The real transport's own description, as tools/soak.ps1 reads it: "udp", then this machine's
	// address in full (loopback is no secret) and a port.
	CHECK(LoggedSoon(f, "server: wrong password from udp 127.0.0.1:"));
	CHECK(LoggedSoon(f, std::format("server: player {} joined from udp 127.0.0.1:", welcome->playerId)));
	CHECK(LoggedSoon(f, std::format("server: player {} left", welcome->playerId)));
	for (const std::string_view secret : { "Alice", "Mallory", "hunter2", "letmein" }) {
		CHECK(NowhereInLog(f, secret));
	}
}

// The game's own log (F4Multiplayer.log) can't be played in a test: its lines are written where the game runs. So
// the guard is on the source. A line that names a friend by what they call themselves (their Steam name, by
// default) slipped back in once with a merge; with this, the next one fails the tests instead of a bug report.
namespace
{
	// The repository the tests were built in (they run from its build folder), or else the one they run inside.
	std::optional<std::filesystem::path> SourceRoot()
	{
		std::vector<std::filesystem::path> starts;
		if (char* exe = nullptr; _get_pgmptr(&exe) == 0 && exe && *exe) {
			starts.emplace_back(std::filesystem::path{ exe }.parent_path());
		}
		starts.emplace_back(std::filesystem::current_path());
		for (auto at : starts) {
			for (;; at = at.parent_path()) {
				std::error_code error;
				if (std::filesystem::exists(at / "src" / "game" / "RemotePlayers.cpp", error) && std::filesystem::exists(at / "common" / "Server.cpp", error)) {
					return at;
				}
				if (at == at.parent_path()) {
					break;
				}
			}
		}
		return std::nullopt;
	}

	struct LogCall
	{
		int         line = 0;
		std::string code;  // its arguments with the string and character literals left out
	};

	// Every call of one of a_calls in a_text (a whole name: "log(" is not "dialog("), with its arguments as far as
	// the bracket that closes it.
	std::vector<LogCall> LogCalls(std::string_view a_text, std::initializer_list<std::string_view> a_calls)
	{
		std::vector<LogCall> out;
		const auto ident = [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_' || a_c == ':'; };
		for (std::size_t at = 0; at < a_text.size(); ++at) {
			const auto call = std::ranges::find_if(a_calls, [&](std::string_view a_call) { return a_text.substr(at).starts_with(a_call) && (at == 0 || !ident(a_text[at - 1])); });
			if (call == a_calls.end()) {
				continue;
			}
			LogCall found{ .line = static_cast<int>(std::ranges::count(a_text.substr(0, at), '\n')) + 1 };
			int depth = 1;
			auto i = at + call->size();
			for (; i < a_text.size() && depth > 0; ++i) {
				const char c = a_text[i];
				if (c == '"' || c == '\'') {
					for (++i; i < a_text.size() && a_text[i] != c; ++i) {
						i += a_text[i] == '\\';  // what a backslash escapes is skipped
					}
					continue;
				}
				depth += c == '(';
				depth -= c == ')';
				found.code += c;
			}
			out.push_back(std::move(found));
			at = i;
		}
		return out;
	}

	// A player's name, as the code holds it: a member called name (a player's, a stand-in's, a hello's) or a Steam
	// persona.
	bool ReadsAName(const LogCall& a_call)
	{
		static const std::regex name{ R"((\.|->)\s*name\b(?!\s*\()|[Pp]ersona)" };
		return std::regex_search(a_call.code, name);
	}

	std::vector<std::string> NamedInLog(const std::filesystem::path& a_folder, std::initializer_list<std::string_view> a_calls)
	{
		std::vector<std::string> out;
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ a_folder }) {
			const auto ext = entry.path().extension();
			if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
				continue;
			}
			std::ifstream      file{ entry.path(), std::ios::binary };
			std::ostringstream text;
			text << file.rdbuf();
			for (const auto& call : LogCalls(text.str(), a_calls)) {
				if (ReadsAName(call)) {
					out.push_back(std::format("{}:{}", std::filesystem::relative(entry.path(), a_folder.parent_path()).generic_string(), call.line));
				}
			}
		}
		return out;
	}
}

TEST("log privacy: the guard on the game's log finds a line that names a friend, and lets one that says player N through")
{
	// The line that slipped back in, and the one it became.
	const auto named = LogCalls(R"x(		REX::WARN("RemotePlayers: '{}' is in power armor, but the frame they told ({:08X}) is {}", remote.name, remote.powerArmorFrame,
			remote.powerArmorFrame ? "no frame in this game" : "none");)x",
		{ "REX::WARN(" });
	REQUIRE(named.size() == 1);
	CHECK(ReadsAName(named[0]));
	const auto numbered = LogCalls(R"x(REX::WARN("RemotePlayers: player {} is in power armor (never their .name or ->name)", remote.id);)x", { "REX::WARN(" });
	REQUIRE(numbered.size() == 1);
	CHECK(!ReadsAName(numbered[0]));
	// A persona read straight from Steam, a server line, and a file's name (not a player's).
	CHECK(ReadsAName(LogCalls(R"x(REX::INFO("Steam: {}", friends->GetPersonaName());)x", { "REX::INFO(" }).at(0)));
	CHECK(ReadsAName(LogCalls(R"x(log(std::format("server: '{}' left", a_player.name));)x", { "log(" }).at(0)));
	CHECK(!ReadsAName(LogCalls(R"x(REX::INFO("Identity: couldn't write {}", path->filename().string());)x", { "REX::INFO(" }).at(0)));
	CHECK(LogCalls(R"x(dialog(a_player.name);)x", { "log(" }).empty());
}

TEST("log privacy: no line the game or the server logs names a friend; they are \"player N\" (a bug report's log names nobody)")
{
	const auto root = SourceRoot();
	REQUIRE(root);  // run the tests from the repository or its build folder
	auto named = NamedInLog(*root / "src", { "REX::INFO(", "REX::WARN(", "REX::ERROR(", "REX::DEBUG(", "REX::TRACE(", "REX::CRITICAL(" });
	std::ranges::move(NamedInLog(*root / "common", { "log(", "a_log(" }), std::back_inserter(named));
	for (const auto& at : named) {
		Test::Fail(__FILE__, __LINE__, std::format("a log line reads a player's name at {}", at));
	}
}
