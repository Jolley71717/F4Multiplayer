// Host controls (protocol 42): the host's own game kicks, bans, unbans, locks and changes the password through the real
// server; nobody else may; and the dedicated server's console does the same.

#include "ServerFixture.h"

#include "BanList.h"

#include <fstream>
#include <iterator>

using namespace TestServer;

namespace
{
	constexpr std::uint64_t HOST = 0x1111;
	constexpr PeerId        HOST_PEER = 1;
	constexpr std::uint64_t BOB_STEAM = 76561198000000002ull;  // a made-up account in Steam's range
	constexpr std::uint64_t BOB_GAME = 0xB0B;

	// A ban file of its own for one test, removed at the end.
	struct TempBanFile
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("f4mp-test-{:016X}.bans", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
		~TempBanFile()
		{
			std::error_code error;
			std::filesystem::remove(path, error);
			std::filesystem::remove(path.string() + ".tmp", error);
		}
		[[nodiscard]] std::string Text() const
		{
			std::ifstream in{ path, std::ios::binary };
			return { std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
		}
	};

	Server::Options HostedOptions(const std::filesystem::path& a_bans = {})
	{
		Server::Options options;
		options.hostIdentity = HOST;
		options.banFile = a_bans.string();
		return options;
	}

	// The host's game joins first (a hosted server lets nobody in before it); returns its first HostInfo.
	Protocol::HostInfo JoinHost(Fixture& a_f)
	{
		a_f.fake->SetLocal(HOST_PEER);
		REQUIRE(a_f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST)));
		const auto info = a_f.fake->WaitFor(HOST_PEER, Type(MT::kHostInfo));
		REQUIRE(info);
		const auto decoded = Protocol::DecodeHostInfo(*info);
		REQUIRE(decoded);
		return *decoded;
	}

	// Bob joins through a_peer as a Steam friend would (the transport knows his account).
	std::uint32_t JoinBob(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetIdentity(a_peer, BOB_STEAM);
		return JoinAs(a_f, a_peer, "Bob", BOB_GAME);
	}

	// Sends a host command as a_peer and returns the HostInfo that answers it (none came: nullopt). One sent before it
	// (the host's welcome has one, and may still be on its way) is skipped: the answer names the command's action and a result.
	std::optional<Protocol::HostInfo> Command(Fixture& a_f, PeerId a_peer, const Protocol::HostCommand& a_command)
	{
		a_f.fake->Take(a_peer);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_command));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
		for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
			const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kHostInfo), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
			if (!packet) {
				break;
			}
			if (auto info = Protocol::DecodeHostInfo(*packet); info && info->action == a_command.action && info->result != Protocol::HostResult::kNone) {
				return info;
			}
		}
		return std::nullopt;
	}

	std::optional<Protocol::Removed> WaitRemoved(Fixture& a_f, PeerId a_peer)
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kRemoved));
		return packet ? Protocol::DecodeRemoved(*packet) : std::nullopt;
	}

	std::optional<Protocol::Reject> JoinRefused(Fixture& a_f, PeerId a_peer, const Protocol::Hello& a_hello)
	{
		a_f.fake->Connect(a_peer);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kReject));
		return packet ? Protocol::DecodeReject(*packet) : std::nullopt;
	}

	bool Contains(std::string_view a_text, std::string_view a_part) { return a_text.find(a_part) != std::string_view::npos; }

	// No line of the server's log names a player or holds an account (the log never names anyone; Redact).
	void CheckLogNamesNobody(Fixture& a_f)
	{
		for (const auto& line : a_f.LogLines()) {
			CHECK(!Contains(line, "Bob"));
			CHECK(!Contains(line, "Carol"));
			CHECK(!Contains(line, std::to_string(BOB_STEAM)));
		}
	}
}

TEST("host controls: the host kicks a player, who is told why and may join again")
{
	Fixture    f{ HostedOptions() };
	const auto start = JoinHost(f);
	CHECK(!start.locked);
	CHECK(!start.password);
	CHECK(start.bans == 0u);
	const auto bob = JoinBob(f, 2);
	Clear(f, { 2 });

	const auto info = Command(f, HOST_PEER, { Protocol::HostAction::kKick, bob });
	REQUIRE(info);
	CHECK(info->result == Protocol::HostResult::kDone);
	CHECK(info->action == Protocol::HostAction::kKick);
	CHECK(info->bans == 0u);
	const auto removed = WaitRemoved(f, 2);
	REQUIRE(removed);
	CHECK(removed->reason == Protocol::RemovedReason::kKicked);
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Disconnected(2));
	// The host sees him leave.
	const auto left = f.fake->WaitFor(HOST_PEER, Type(MT::kPlayerLeft));
	REQUIRE(left);
	CHECK(Protocol::DecodePlayerLeft(*left)->playerId == bob);
	CHECK(f.Logged(std::format("player {} was kicked by the host", bob)));

	// A kick isn't a ban: he comes back under his old number.
	CHECK(JoinBob(f, 3) == bob);
	CheckLogNamesNobody(f);
}

TEST("host controls: the host can't kick themselves or nobody")
{
	Fixture    f{ HostedOptions() };
	const auto host = JoinHost(f);
	(void)host;
	const auto self = Command(f, HOST_PEER, { Protocol::HostAction::kKick, 1 });
	REQUIRE(self);
	CHECK(self->result == Protocol::HostResult::kNotTheHost);
	CHECK(!f.fake->Disconnected(HOST_PEER));
	const auto nobody = Command(f, HOST_PEER, { Protocol::HostAction::kBan, 99 });
	REQUIRE(nobody);
	CHECK(nobody->result == Protocol::HostResult::kNoSuchPlayer);
	CHECK(nobody->bans == 0u);
}

TEST("host controls: a banned player is dropped, refused at every later join, and the ban outlives the server")
{
	TempBanFile file;
	{
		Fixture f{ HostedOptions(file.path) };
		JoinHost(f);
		const auto bob = JoinBob(f, 2);
		Clear(f, { 2 });

		const auto info = Command(f, HOST_PEER, { Protocol::HostAction::kBan, bob });
		REQUIRE(info);
		CHECK(info->result == Protocol::HostResult::kDone);
		CHECK(info->bans == 1u);
		REQUIRE(info->list.size() == 1u);
		CHECK(info->list[0].name == "Bob");  // for the host's menu, this run only
		CHECK(info->list[0].alias.starts_with("~"));
		CHECK(info->list[0].bannedAt > 1700000000u);
		const auto removed = WaitRemoved(f, 2);
		REQUIRE(removed);
		CHECK(removed->reason == Protocol::RemovedReason::kBanned);
		REQUIRE(f.fake->Settle());
		CHECK(f.fake->Disconnected(2));
		CHECK(f.Logged(std::format("player {} was banned by the host ({}", bob, info->list[0].alias)));

		// His Steam account is refused, whatever his game says it is.
		f.fake->SetIdentity(3, BOB_STEAM);
		const auto again = JoinRefused(f, 3, Fixture::MakeHello("Bob", 0xFEED));
		REQUIRE(again);
		CHECK(again->reason == Protocol::REJECT_BANNED);
		// ... and so is his game's identity over a connection that names no account (UDP).
		const auto udp = JoinRefused(f, 4, Fixture::MakeHello("Bob", BOB_GAME));
		REQUIRE(udp);
		CHECK(udp->reason == Protocol::REJECT_BANNED);
		CHECK(f.Logged(std::format("banned ({})", info->list[0].alias)));
		// Someone else still gets in.
		CHECK(JoinAs(f, 5, "Carol", 0xCA201));
		CheckLogNamesNobody(f);
	}

	// The file names nobody and holds no account, only hashes.
	const auto text = file.Text();
	CHECK(Contains(text, "salt "));
	CHECK(Contains(text, "\nban "));
	CHECK(!Contains(text, "Bob"));
	CHECK(!Contains(text, std::to_string(BOB_STEAM)));
	CHECK(!Contains(text, std::format("{:x}", BOB_STEAM)));
	CHECK(!Contains(text, std::format("{:016x}", BOB_GAME)));

	// A new server run reads it: still refused.
	Fixture f{ HostedOptions(file.path) };
	const auto start = JoinHost(f);
	CHECK(f.Logged("1 banned player in"));  // said at the start, before the host came
	CHECK(start.bans == 1u);
	REQUIRE(start.list.size() == 1u);
	CHECK(start.list[0].name.empty());  // names are never saved
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto refused = JoinRefused(f, 2, Fixture::MakeHello("Bob", BOB_GAME));
	REQUIRE(refused);
	CHECK(refused->reason == Protocol::REJECT_BANNED);
}

TEST("host controls: an unbanned player joins again, and the file forgets them")
{
	TempBanFile file;
	Fixture     f{ HostedOptions(file.path) };
	JoinHost(f);
	const auto bob = JoinBob(f, 2);
	const auto banned = Command(f, HOST_PEER, { Protocol::HostAction::kBan, bob });
	REQUIRE(banned);
	REQUIRE(banned->list.size() == 1u);
	REQUIRE(f.fake->Settle());

	const auto none = Command(f, HOST_PEER, { Protocol::HostAction::kUnban, 0, 0x1234 });  // no such ban
	REQUIRE(none);
	CHECK(none->result == Protocol::HostResult::kNoSuchBan);
	CHECK(none->bans == 1u);

	const auto lifted = Command(f, HOST_PEER, { Protocol::HostAction::kUnban, 0, banned->list[0].id });
	REQUIRE(lifted);
	CHECK(lifted->result == Protocol::HostResult::kDone);
	CHECK(lifted->bans == 0u);
	CHECK(lifted->list.empty());
	CHECK(f.Logged(std::format("{} unbanned by the host", banned->list[0].alias)));
	CHECK(!Contains(file.Text(), "\nban "));
	CHECK(JoinBob(f, 3) == bob);

	// Unban 0 lifts every ban.
	REQUIRE(Command(f, HOST_PEER, { Protocol::HostAction::kBan, bob }));
	const auto all = Command(f, HOST_PEER, { Protocol::HostAction::kUnban, 0, 0 });
	REQUIRE(all);
	CHECK(all->bans == 0u);
}

TEST("host controls: a locked session lets nobody new in, but keeps those in it")
{
	Fixture f{ HostedOptions() };
	JoinHost(f);
	const auto bob = JoinBob(f, 2);
	Clear(f, { 2 });

	const auto locked = Command(f, HOST_PEER, { Protocol::HostAction::kLock });
	REQUIRE(locked);
	CHECK(locked->locked);
	CHECK(f.Logged("the session was locked by the host"));
	const auto carol = JoinRefused(f, 3, Fixture::MakeHello("Carol", 0xCA201));
	REQUIRE(carol);
	CHECK(carol->reason == Protocol::REJECT_LOCKED);
	CHECK(f.Logged("the session is locked"));

	// Bob stays, and his game restarting (a hello again on his connection, or a new one while the old is still there)
	// is not someone new.
	CHECK(!f.fake->Disconnected(2));
	f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Bob", BOB_GAME)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWelcome)));
	f.fake->SetIdentity(4, BOB_STEAM);
	CHECK(JoinAs(f, 4, "Bob", BOB_GAME) == bob);

	// The host's own game is never locked out.
	f.fake->Drop(HOST_PEER);
	REQUIRE(f.fake->Settle());
	f.fake->SetLocal(5);
	REQUIRE(f.Join(5, Fixture::MakeHello("Host", HOST)));

	const auto open = Command(f, 5, { Protocol::HostAction::kUnlock });
	REQUIRE(open);
	CHECK(!open->locked);
	CHECK(JoinAs(f, 6, "Carol", 0xCA201));
}

TEST("host controls: the host changes and removes the password, and the log never has it")
{
	Server::Options options = HostedOptions();
	options.password = "hunter2";
	Fixture f{ options };
	f.fake->SetLocal(HOST_PEER);
	auto host = Fixture::MakeHello("Host", HOST);
	host.password = "hunter2";
	REQUIRE(f.Join(HOST_PEER, host));
	const auto first = f.fake->WaitFor(HOST_PEER, Type(MT::kHostInfo));
	REQUIRE(first);
	CHECK(Protocol::DecodeHostInfo(*first)->password);

	const auto changed = Command(f, HOST_PEER, { Protocol::HostAction::kSetPassword, 0, 0, "swordfish" });
	REQUIRE(changed);
	CHECK(changed->password);
	CHECK(f.Logged("the password was changed by the host"));

	auto old = Fixture::MakeHello("Carol", 0xCA201);
	old.password = "hunter2";
	const auto refused = JoinRefused(f, 2, old);
	REQUIRE(refused);
	CHECK(refused->reason == "Wrong password");
	auto right = Fixture::MakeHello("Carol", 0xCA201);
	right.password = "swordfish";
	CHECK(f.Join(3, right));

	const auto removed = Command(f, HOST_PEER, { Protocol::HostAction::kSetPassword, 0, 0, "" });
	REQUIRE(removed);
	CHECK(!removed->password);
	CHECK(f.Logged("the password was removed by the host"));
	CHECK(f.Join(4, Fixture::MakeHello("Dave", 0xDA5E)));

	for (const auto& line : f.LogLines()) {
		CHECK(!Contains(line, "swordfish"));
		CHECK(!Contains(line, "hunter2"));
	}
}

TEST("host controls: a player who isn't the host is refused and logged")
{
	Fixture    f{ HostedOptions() };
	JoinHost(f);
	const auto bob = JoinBob(f, 2);
	const auto carol = JoinAs(f, 3, "Carol", 0xCA201);
	Clear(f, { 2, 3 });

	// Bob tries everything: kick Carol, ban the host, lock, set a password, ask for the state.
	for (const auto& command : { Protocol::HostCommand{ Protocol::HostAction::kKick, carol }, Protocol::HostCommand{ Protocol::HostAction::kBan, 1 },
			 Protocol::HostCommand{ Protocol::HostAction::kLock }, Protocol::HostCommand{ Protocol::HostAction::kSetPassword, 0, 0, "mine" } }) {
		f.fake->Deliver(2, Protocol::Encode(command));
	}
	f.fake->Deliver(2, Protocol::Encode(Protocol::HostInfoRequest{}));
	const auto toBob = Received(f, 2);
	CHECK(Count(toBob, MT::kHostInfo) == 0u);
	CHECK(Count(toBob, MT::kRemoved) == 0u);
	CHECK(f.Logged(std::format("player {} is not the host: their host command was refused", bob)));
	// One line, not one per command: the rest are counted in the next.
	CHECK(f.LoggedCount("is not the host") == 1u);

	// Nothing happened: Carol and the host are in, the session isn't locked and has no password.
	Sync(f, 3);
	CHECK(!f.fake->Disconnected(3));
	CHECK(!f.fake->Disconnected(HOST_PEER));
	CHECK(JoinAs(f, 4, "Dave", 0xDA5E));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::HostInfoRequest{}));
	const auto state = f.fake->WaitFor(HOST_PEER, Type(MT::kHostInfo));
	REQUIRE(state);
	const auto info = Protocol::DecodeHostInfo(*state);
	REQUIRE(info);
	CHECK(!info->locked);
	CHECK(!info->password);
	CHECK(info->bans == 0u);

	// A dedicated server has no host's game: every player is refused.
	Fixture dedicated;
	const auto alice = JoinAs(dedicated, 1, "Alice", 0xA11CE);
	JoinAs(dedicated, 2, "Bob", BOB_GAME);
	dedicated.fake->Deliver(1, Protocol::Encode(Protocol::HostCommand{ Protocol::HostAction::kKick, 2 }));
	Sync(dedicated, 1);
	CHECK(!dedicated.fake->Disconnected(2));
	CHECK(dedicated.Logged(std::format("player {} is not the host", alice)));
}

TEST("host controls: the dedicated server's console kicks, bans, unbans, locks and sets the password")
{
	TempBanFile file;
	Server::Options options;
	options.banFile = file.path.string();
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice", 0xA11CE);
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto bob = JoinAs(f, 2, "Bob", BOB_GAME);
	Clear(f, { 1, 2 });

	const auto wait = [&](std::string_view a_text) {
		for (int i = 0; i < 100 && !f.Logged(a_text); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return f.Logged(a_text);
	};

	f.server.Console("players");
	CHECK(wait(std::format("console: player {}: fake 2", bob)));
	f.server.Console("kick 99");
	CHECK(wait("console: no player 99"));
	f.server.Console(std::format("kick {}", alice));
	const auto kicked = WaitRemoved(f, 1);
	REQUIRE(kicked);
	CHECK(kicked->reason == Protocol::RemovedReason::kKicked);
	CHECK(wait(std::format("player {} was kicked by the console", alice)));

	f.server.Console(std::format("ban {}", bob));
	const auto banned = WaitRemoved(f, 2);
	REQUIRE(banned);
	CHECK(banned->reason == Protocol::RemovedReason::kBanned);
	CHECK(wait(std::format("player {} was banned by the console", bob)));
	f.fake->SetIdentity(3, BOB_STEAM);
	const auto refused = JoinRefused(f, 3, Fixture::MakeHello("Bob", BOB_GAME));
	REQUIRE(refused);
	CHECK(refused->reason == Protocol::REJECT_BANNED);
	f.server.Console("bans");
	CHECK(wait("console: ban 1: ~"));
	f.server.Console("unban 1");
	CHECK(wait("unbanned by the console (0 banned)"));
	f.fake->SetIdentity(4, BOB_STEAM);
	CHECK(JoinAs(f, 4, "Bob", BOB_GAME) == bob);

	f.server.Console("lock");
	CHECK(wait("the session was locked by the console"));
	const auto locked = JoinRefused(f, 5, Fixture::MakeHello("Carol", 0xCA201));
	REQUIRE(locked);
	CHECK(locked->reason == Protocol::REJECT_LOCKED);
	f.server.Console("unlock");
	CHECK(wait("the session was unlocked by the console"));

	f.server.Console("password open sesame");
	CHECK(wait("the password was changed by the console"));
	auto right = Fixture::MakeHello("Carol", 0xCA201);
	right.password = "open sesame";
	CHECK(f.Join(6, right));
	const auto wrong = JoinRefused(f, 7, Fixture::MakeHello("Dave", 0xDA5E));
	REQUIRE(wrong);
	CHECK(wrong->reason == "Wrong password");
	f.server.Console("password -");
	CHECK(wait("the password was removed by the console"));
	CHECK(f.Join(8, Fixture::MakeHello("Dave", 0xDA5E)));

	f.server.Console("frobnicate");
	CHECK(wait("console: unknown command"));
	for (const auto& line : f.LogLines()) {
		CHECK(!Contains(line, "sesame"));
	}
	CheckLogNamesNobody(f);
}

TEST("ban list: saved and read back, matched by either identity, unbanned by id or alias")
{
	TempBanFile file;
	BanList     bans{ 0x5A17 };
	CHECK(!bans.Find(BOB_STEAM, BOB_GAME));
	CHECK(bans.Add(0, 0, 1, "Nobody") == nullptr);  // nothing to ban by
	const auto entry = bans.Add(BOB_STEAM, BOB_GAME, 1792000000, "Bob");
	REQUIRE(entry);
	const auto id = entry->Id();
	const auto alias = entry->Alias();
	CHECK(alias.size() == 7u);
	CHECK(bans.Find(BOB_STEAM, 0) != nullptr);
	CHECK(bans.Find(0, BOB_GAME) != nullptr);
	CHECK(bans.Find(BOB_STEAM + 1, BOB_GAME + 1) == nullptr);
	CHECK(bans.Add(BOB_STEAM, 0, 5, "Bob again") != nullptr);  // the same account: still one ban
	CHECK(bans.Size() == 1u);
	REQUIRE(bans.Save(file.path));

	BanList read;
	CHECK(read.Load(file.path).empty());
	REQUIRE(read.Size() == 1u);
	CHECK(read.Find(BOB_STEAM, 0) != nullptr);  // the file's salt, not the new list's own
	CHECK(read.Entries()[0].Id() == id);
	CHECK(read.Entries()[0].bannedAt == 1792000000u);
	CHECK(read.Entries()[0].name.empty());
	CHECK(read.FindAlias(alias) != nullptr);
	CHECK(read.FindAlias(alias.substr(1)) != nullptr);
	CHECK(read.Remove(id));
	CHECK(!read.Remove(id));
	CHECK(read.Find(BOB_STEAM, BOB_GAME) == nullptr);

	// Another salt (another host's file) matches nothing.
	BanList other{ 0x0DD };
	other.Add(BOB_STEAM, BOB_GAME, 1, "");
	CHECK(other.Entries()[0].Id() != id);

	// Bans without the salt line can't be matched: none are kept, and Load says so.
	{
		std::ofstream out{ file.path, std::ios::trunc };
		out << "ban 0123456789abcdef 0000000000000000 1\nnonsense\n";
	}
	BanList unsalted;
	CHECK(!unsalted.Load(file.path).empty());
	CHECK(unsalted.Size() == 0u);
	// A missing file is an empty list.
	std::filesystem::remove(file.path);
	BanList missing;
	CHECK(missing.Load(file.path).empty());
	CHECK(missing.Size() == 0u);
}

TEST("protocol: host controls' messages read back as written")
{
	const Protocol::HostCommand command{ Protocol::HostAction::kUnban, 7, 0xABCDEF0123456789ull, "" };
	CHECK(Protocol::DecodeHostCommand(Protocol::Encode(command)) == command);
	auto bad = Protocol::Encode(command);
	bad[1] = 9;  // no such action
	CHECK(!Protocol::DecodeHostCommand(bad));
	CHECK(Protocol::DecodeRemoved(Protocol::Encode(Protocol::Removed{ Protocol::RemovedReason::kKicked }))->reason == Protocol::RemovedReason::kKicked);

	Protocol::HostInfo info{ true, false, Protocol::HostResult::kNotSaved, Protocol::HostAction::kBan, 70, {} };
	for (std::uint64_t i = 0; i < 70; ++i) {
		info.list.push_back({ i + 1, "~00000" + std::to_string(i % 10), "", 1792000000 + i });
	}
	const auto decoded = Protocol::DecodeHostInfo(Protocol::Encode(info));
	REQUIRE(decoded);
	CHECK(decoded->bans == 70u);
	CHECK(decoded->list.size() == Protocol::MAX_BANS_SENT);  // the rest stay on the host's disk
	CHECK(decoded->locked && !decoded->password);
	CHECK(decoded->result == Protocol::HostResult::kNotSaved);
	CHECK(decoded->list[63] == info.list[63]);
}
