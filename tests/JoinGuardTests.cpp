// Joins by address, the guard and the gates: the password challenge, the host's approval of each joiner by address and
// the limits on a reachable port. The hashing is Windows CNG (common/JoinProof.h), so what needs it is Windows only.

#include "ServerFixture.h"

#include "JoinGuard.h"
#include "JoinProof.h"

#include <string>
#include <vector>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	std::vector<std::uint8_t> Bytes(std::string_view a_text) { return { a_text.begin(), a_text.end() }; }

	std::string Hex(std::span<const std::uint8_t> a_bytes)
	{
		std::string out;
		for (const auto byte : a_bytes) {
			out += std::format("{:02x}", byte);
		}
		return out;
	}
}

TEST("join guard: the protocol's challenge sizes are the proof's")
{
	CHECK(Protocol::CHALLENGE_NONCE_SIZE == JoinProof::NONCE_SIZE);
	CHECK(Protocol::CHALLENGE_SALT_SIZE == JoinProof::SALT_SIZE);
	CHECK(Protocol::PROOF_SIZE == JoinProof::PROOF_SIZE);
}

TEST("join guard: a source is limited on wrong passwords, and a source of 0 never is")
{
	JoinGuard   guard;
	const auto  t0 = JoinGuard::Clock::now();
	const auto  source = std::uint64_t{ 77 };
	for (int i = 0; i < JoinGuard::MAX_WRONG - 1; ++i) {
		guard.OnWrongPassword(source, t0 + std::chrono::seconds(i));
		CHECK(!guard.Blocked(source, t0 + std::chrono::seconds(i)));
	}
	guard.OnWrongPassword(source, t0 + 10s);
	CHECK(guard.Blocked(source, t0 + 11s));
	CHECK(!guard.Blocked(99, t0 + 11s));  // another address
	CHECK(guard.Blocked(source, t0 + 10s + JoinGuard::BLOCK - 1s));
	CHECK(!guard.Blocked(source, t0 + 10s + JoinGuard::BLOCK + 1s));
	for (int i = 0; i < 50; ++i) {
		guard.OnWrongPassword(0, t0);
	}
	CHECK(!guard.Blocked(0, t0));
}

TEST("join guard: wrong passwords spread over more than the window do not block")
{
	JoinGuard  guard;
	const auto t0 = JoinGuard::Clock::now();
	for (int i = 0; i < 20; ++i) {
		guard.OnWrongPassword(5, t0 + i * 20s);  // three a minute: under the limit
	}
	CHECK(!guard.Blocked(5, t0 + 400s));
}

TEST("join guard: only so many connections of a source are open, and so many a minute")
{
	JoinGuard  guard;
	const auto t0 = JoinGuard::Clock::now();
	CHECK(guard.MayConnect(3, t0));
	CHECK(guard.MayConnect(3, t0));
	CHECK(!guard.MayConnect(3, t0));  // two aren't in yet
	CHECK(guard.MayConnect(4, t0));   // another address is not held back
	guard.OnSettled(3);
	CHECK(guard.MayConnect(3, t0));
	for (int i = 0; i < 100; ++i) {
		CHECK(guard.MayConnect(0, t0));  // this machine and Steam: no limit
	}
	// A minute's connects: the rest is turned away until the window passes.
	JoinGuard  busy;
	int        taken = 0;
	for (int i = 0; i < 60; ++i) {
		if (busy.MayConnect(8, t0 + std::chrono::seconds(i))) {
			++taken;
			busy.OnSettled(8);
		}
	}
	CHECK(taken == JoinGuard::MAX_CONNECTS);
	CHECK(busy.MayConnect(8, t0 + 61s + 5s));
}

TEST("join guard: the sources it keeps are bounded")
{
	JoinGuard  guard;
	const auto t0 = JoinGuard::Clock::now();
	for (std::uint64_t i = 1; i <= JoinGuard::MAX_SOURCES + 200; ++i) {
		static_cast<void>(guard.MayConnect(i, t0 + std::chrono::milliseconds(i)));
		guard.OnSettled(i);
	}
	CHECK(guard.Size() <= JoinGuard::MAX_SOURCES);
}

#ifdef _WIN32

TEST("join proof: HMAC-SHA256 gives the RFC 4231 test vectors")
{
	const auto one = JoinProof::Hmac(std::vector<std::uint8_t>(20, 0x0b), Bytes("Hi There"));
	REQUIRE(one);
	CHECK(Hex(*one) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	const auto two = JoinProof::Hmac(Bytes("Jefe"), Bytes("what do ya want for nothing?"));
	REQUIRE(two);
	CHECK(Hex(*two) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST("join proof: PBKDF2-HMAC-SHA256 gives the known test vectors")
{
	std::array<std::uint8_t, 32> out{};
	REQUIRE(JoinProof::Pbkdf2("password", Bytes("salt"), 1, out));
	CHECK(Hex(out) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	REQUIRE(JoinProof::Pbkdf2("password", Bytes("salt"), 2, out));
	CHECK(Hex(out) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	REQUIRE(JoinProof::Pbkdf2("password", Bytes("salt"), 4096, out));
	CHECK(Hex(out) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

TEST("join proof: the answer follows the password, the salt and the nonce")
{
	const auto salt = Bytes("0123456789abcdef");
	const auto key = JoinProof::DeriveKey("hunter2", salt);
	const auto other = JoinProof::DeriveKey("hunter3", salt);
	const auto resalted = JoinProof::DeriveKey("hunter2", Bytes("fedcba9876543210"));
	REQUIRE(key);
	REQUIRE(other);
	REQUIRE(resalted);
	const auto nonce = Bytes("a nonce of the right size 32 byt");
	const auto answer = JoinProof::Answer(*key, nonce);
	REQUIRE(answer);
	CHECK(JoinProof::Equal(*answer, *JoinProof::Answer(*key, nonce)));  // the same again
	CHECK(!JoinProof::Equal(*answer, *JoinProof::Answer(*other, nonce)));
	CHECK(!JoinProof::Equal(*answer, *JoinProof::Answer(*resalted, nonce)));
	CHECK(!JoinProof::Equal(*answer, *JoinProof::Answer(*key, Bytes("another nonce of 32 bytes exactly"))));
	// The password is not in what is sent.
	CHECK(Hex(*answer).find("hunter2") == std::string::npos);
}

TEST("join proof: random bytes differ, and Equal looks at every byte")
{
	JoinProof::Nonce a{};
	JoinProof::Nonce b{};
	REQUIRE(JoinProof::Random(a));
	REQUIRE(JoinProof::Random(b));
	CHECK(a != b);
	auto c = a;
	CHECK(JoinProof::Equal(a, c));
	c.back() ^= 1;
	CHECK(!JoinProof::Equal(a, c));
	CHECK(!JoinProof::Equal(std::span<const std::uint8_t>{ a }.first(31), a));
}

namespace
{
	constexpr std::uint64_t HOST = 0x1111;
	constexpr PeerId        HOST_PEER = 1;
	constexpr std::uint64_t REMOTE_A = 0xAAAA;  // a made-up address's source
	constexpr std::uint64_t REMOTE_B = 0xBBBB;

	Server::Options Options(std::string a_password, bool a_hosted)
	{
		Server::Options options;
		options.password = std::move(a_password);
		if (a_hosted) {
			options.hostIdentity = HOST;
			options.approveJoins = true;
		}
		return options;
	}

	// A hello of a joiner by address: no password in it.
	Protocol::Hello RemoteHello(std::string a_name, std::uint64_t a_identity)
	{
		return Fixture::MakeHello(std::move(a_name), a_identity);
	}

	std::optional<Protocol::Challenge> Challenged(Fixture& a_f, PeerId a_peer)
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kChallenge));
		return packet ? Protocol::DecodeChallenge(*packet) : std::nullopt;
	}

	// The proof a game gives for a challenge, under a_password.
	std::vector<std::uint8_t> ProofFor(const Protocol::Challenge& a_challenge, std::string_view a_password)
	{
		const auto key = JoinProof::DeriveKey(a_password, a_challenge.salt);
		REQUIRE(key);
		const auto answer = JoinProof::Answer(*key, a_challenge.nonce);
		REQUIRE(answer);
		return { answer->begin(), answer->end() };
	}

	// Connects a_peer from a_source, says hello, gets the challenge and answers it with a_password.
	void JoinAnswering(Fixture& a_f, PeerId a_peer, std::uint64_t a_source, Protocol::Hello a_hello, std::string_view a_password)
	{
		a_f.fake->SetSource(a_peer, a_source);
		a_f.fake->Connect(a_peer);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto challenge = Challenged(a_f, a_peer);
		REQUIRE(challenge);
		a_hello.proof = ProofFor(*challenge, a_password);
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_hello));
	}

	std::optional<Protocol::Reject> Refused(Fixture& a_f, PeerId a_peer)
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kReject));
		return packet ? Protocol::DecodeReject(*packet) : std::nullopt;
	}

	std::optional<Protocol::HostInfo> WaitingInfo(Fixture& a_f, std::size_t a_waiting)
	{
		for (int i = 0; i < 20; ++i) {
			const auto packet = a_f.fake->WaitFor(HOST_PEER, Type(MT::kHostInfo), 5s);
			if (!packet) {
				return std::nullopt;
			}
			if (auto info = Protocol::DecodeHostInfo(*packet); info && info->waiting.size() == a_waiting) {
				return info;
			}
		}
		return std::nullopt;
	}

	// The host's own game: from this machine, so its password goes as typed (nothing readable leaves the PC).
	void JoinHost(Fixture& a_f, std::string a_password = {})
	{
		a_f.fake->SetLocal(HOST_PEER);
		auto hello = Fixture::MakeHello("Host", HOST);
		hello.password = std::move(a_password);
		REQUIRE(a_f.Join(HOST_PEER, hello));
	}
}

TEST("join challenge: a joiner over UDP proves the password and never sends it")
{
	Fixture f{ Options("hunter2", false) };
	f.fake->SetSource(2, REMOTE_A);
	f.fake->Connect(2);
	auto hello = RemoteHello("Bob", 0xB0B);
	f.fake->Deliver(2, Protocol::Encode(hello));
	const auto challenge = Challenged(f, 2);
	REQUIRE(challenge);
	CHECK(challenge->nonce.size() == Protocol::CHALLENGE_NONCE_SIZE);
	hello.proof = ProofFor(*challenge, "hunter2");
	f.fake->Deliver(2, Protocol::Encode(hello));
	CHECK(f.fake->WaitFor(2, Type(MT::kWelcome)));
	for (const auto& line : f.LogLines()) {
		CHECK(line.find("hunter2") == std::string::npos);
	}
}

TEST("join challenge: a wrong answer is refused, and so is the password as typed from another machine")
{
	Fixture f{ Options("hunter2", false) };
	JoinAnswering(f, 2, REMOTE_A, RemoteHello("Bob", 0xB0B), "hunter3");
	const auto reject = Refused(f, 2);
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password");

	// The right password in the clear is not taken from UDP: it is asked to prove it, and gets no welcome.
	f.fake->SetSource(3, REMOTE_B);
	f.fake->Connect(3);
	auto clear = RemoteHello("Carol", 0xCA);
	clear.password = "hunter2";
	f.fake->Deliver(3, Protocol::Encode(clear));
	CHECK(Challenged(f, 3));
	REQUIRE(f.fake->Settle());  // the server has handled everything sent: a welcome would be here by now
	CHECK(Count(f.fake->Take(3), MT::kWelcome) == 0u);
}

TEST("join challenge: an answer is good for its nonce only")
{
	Fixture f{ Options("hunter2", false) };
	f.fake->SetSource(2, REMOTE_A);
	f.fake->Connect(2);
	auto hello = RemoteHello("Bob", 0xB0B);
	f.fake->Deliver(2, Protocol::Encode(hello));
	const auto first = Challenged(f, 2);
	REQUIRE(first);
	const auto proof = ProofFor(*first, "hunter2");
	// Someone who heard that answer says it on another connection: it was for another nonce.
	f.fake->SetSource(3, REMOTE_B);
	f.fake->Connect(3);
	auto replay = RemoteHello("Mallory", 0xBAD);
	replay.proof = proof;
	f.fake->Deliver(3, Protocol::Encode(replay));
	const auto second = Challenged(f, 3);  // a proof with no challenge of its own is just a new challenge
	REQUIRE(second);
	CHECK(second->nonce != first->nonce);
	REQUIRE(f.fake->Settle());  // the server has handled everything sent: a welcome would be here by now
	CHECK(Count(f.fake->Take(3), MT::kWelcome) == 0u);
	replay.proof = proof;  // and against the right challenge: it fails
	f.fake->Deliver(3, Protocol::Encode(replay));
	const auto reject = Refused(f, 3);
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password");
}

TEST("join challenge: a Steam friend or the host's own game sends the password as before")
{
	Fixture f{ Options("hunter2", false) };
	f.fake->SetIdentity(2, 0x57EA000000000002ull);
	auto hello = Fixture::MakeHello("Bob", 0xB0B);
	hello.password = "hunter2";
	CHECK(f.Join(2, hello));
	auto wrong = Fixture::MakeHello("Carol", 0xCA);
	wrong.password = "nope";
	f.fake->Connect(3);
	f.fake->Deliver(3, Protocol::Encode(wrong));
	const auto reject = Refused(f, 3);
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password");
}

TEST("join challenge: no password set, no challenge")
{
	Fixture f{ Options("", false) };
	f.fake->SetSource(2, REMOTE_A);
	CHECK(f.Join(2, RemoteHello("Bob", 0xB0B)));
}

TEST("join guard: five wrong passwords from one address block it, other addresses are not")
{
	Fixture f{ Options("hunter2", false) };
	for (PeerId peer = 2; peer < 7; ++peer) {
		JoinAnswering(f, peer, REMOTE_A, RemoteHello("Bob", 0xB0B), "wrong");
		CHECK(Refused(f, peer));
		REQUIRE(f.fake->WaitDisconnected(peer));
		REQUIRE(f.fake->Settle());
	}
	f.fake->SetSource(8, REMOTE_A);
	f.fake->Connect(8);
	f.fake->Deliver(8, Protocol::Encode(RemoteHello("Bob", 0xB0B)));
	const auto reject = Refused(f, 8);
	REQUIRE(reject);
	CHECK(reject->reason == Protocol::REJECT_TOO_MANY);
	// Another address still gets in.
	JoinAnswering(f, 9, REMOTE_B, RemoteHello("Carol", 0xCA), "hunter2");
	CHECK(f.fake->WaitFor(9, Type(MT::kWelcome)));
}

TEST("join guard: a third connection of one address that isn't in yet is turned away")
{
	Fixture f{ Options("hunter2", false) };
	for (PeerId peer = 2; peer <= 3; ++peer) {
		f.fake->SetSource(peer, REMOTE_A);
		f.fake->Connect(peer);
	}
	f.fake->SetSource(4, REMOTE_A);
	f.fake->Connect(4);
	CHECK(f.fake->WaitDisconnected(4));
	CHECK(!f.fake->Disconnected(2));
	CHECK(!f.fake->Disconnected(3));
	CHECK(f.LoggedSoon("turned away 1 connections"));
}

TEST("join approval: the host lets a joiner by address in, and the joiner is told to wait")
{
	Fixture f{ Options("hunter2", true) };
	JoinHost(f, "hunter2");
	JoinAnswering(f, 2, REMOTE_A, RemoteHello("Bob", 0xB0B), "hunter2");
	CHECK(f.fake->WaitFor(2, Type(MT::kJoinWait)));
	const auto waiting = WaitingInfo(f, 1);
	REQUIRE(waiting);
	CHECK(waiting->waiting[0].name == "Bob");
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::HostCommand{ Protocol::HostAction::kAdmit, waiting->waiting[0].id }));
	CHECK(f.fake->WaitFor(2, Type(MT::kWelcome)));
	CHECK(WaitingInfo(f, 0));
	for (const auto& line : f.LogLines()) {
		CHECK(line.find("Bob") == std::string::npos);
		CHECK(line.find("hunter2") == std::string::npos);
	}
}

TEST("join approval: a joiner the host refuses is told so and dropped")
{
	Fixture f{ Options("hunter2", true) };
	JoinHost(f, "hunter2");
	JoinAnswering(f, 2, REMOTE_A, RemoteHello("Bob", 0xB0B), "hunter2");
	const auto waiting = WaitingInfo(f, 1);
	REQUIRE(waiting);
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::HostCommand{ Protocol::HostAction::kRefuse, waiting->waiting[0].id }));
	const auto reject = Refused(f, 2);
	REQUIRE(reject);
	CHECK(reject->reason == Protocol::REJECT_JOIN_REFUSED);
	CHECK(f.fake->WaitDisconnected(2));
}

TEST("join approval: a joiner the host doesn't answer is turned away when the wait is over")
{
	Fixture f{ Options("hunter2", true) };
	f.HoldClock();
	JoinHost(f, "hunter2");
	JoinAnswering(f, 2, REMOTE_A, RemoteHello("Bob", 0xB0B), "hunter2");
	REQUIRE(WaitingInfo(f, 1));
	f.Advance(30s);
	f.Advance(35s);
	const auto reject = Refused(f, 2);
	REQUIRE(reject);
	CHECK(reject->reason == Protocol::REJECT_JOIN_TIMEOUT);
	CHECK(WaitingInfo(f, 0));
}

TEST("join approval: a Steam friend and the host's own game are never asked about")
{
	Fixture f{ Options("", true) };
	JoinHost(f);
	f.fake->SetIdentity(2, 0x57EA000000000002ull);
	CHECK(f.Join(2, RemoteHello("Steamy", 0x5EA)));
	REQUIRE(f.fake->Settle());  // the server thread has sent all it will for this join
	CHECK(f.fake->Take(2).size() > 0u);
	CHECK(!f.Logged("waits for the host"));
}

TEST("join approval: a joiner who was let in and comes back is not asked again; one the host removed is")
{
	Fixture f{ Options("", true) };
	JoinHost(f);
	f.fake->SetSource(2, REMOTE_A);
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(RemoteHello("Bob", 0xB0B)));
	const auto waiting = WaitingInfo(f, 1);
	REQUIRE(waiting);
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::HostCommand{ Protocol::HostAction::kAdmit, waiting->waiting[0].id }));
	const auto welcome = f.fake->WaitFor(2, Type(MT::kWelcome));
	REQUIRE(welcome);
	f.fake->Drop(2);
	REQUIRE(f.fake->Settle());
	// His identity from another address is no one the host let in: that one waits (a game identity isn't a secret).
	f.fake->SetSource(5, REMOTE_B);
	f.fake->Connect(5);
	f.fake->Deliver(5, Protocol::Encode(RemoteHello("Bob", 0xB0B)));
	CHECK(f.fake->WaitFor(5, Type(MT::kJoinWait)));
	f.fake->Drop(5);
	REQUIRE(f.fake->Settle());
	f.fake->SetSource(3, REMOTE_A);
	CHECK(f.Join(3, RemoteHello("Bob", 0xB0B)));  // no wait
	// The host kicks him: coming again means asking again.
	const auto id = Protocol::DecodeWelcome(*welcome)->playerId;
	REQUIRE(f.fake->Settle());
	f.fake->Take(HOST_PEER);
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::HostCommand{ Protocol::HostAction::kKick, id }));
	REQUIRE(f.fake->WaitDisconnected(3));
	f.fake->SetSource(4, REMOTE_A);
	f.fake->Connect(4);
	f.fake->Deliver(4, Protocol::Encode(RemoteHello("Bob", 0xB0B)));
	CHECK(f.fake->WaitFor(4, Type(MT::kJoinWait)));
}

TEST("join approval: nobody by address is let in before the host's own game is in")
{
	Fixture f{ Options("", true) };
	f.fake->SetSource(2, REMOTE_A);
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(RemoteHello("Bob", 0xB0B)));
	const auto reject = Refused(f, 2);
	REQUIRE(reject);
	CHECK(reject->reason == Protocol::REJECT_HOST_NOT_READY);
}

TEST("join approval: only so many wait at once")
{
	Fixture f{ Options("", true) };
	JoinHost(f);
	for (PeerId peer = 2; peer < 2 + static_cast<PeerId>(Protocol::MAX_WAITING_SENT); ++peer) {
		f.fake->SetSource(peer, REMOTE_A + peer);
		f.fake->Connect(peer);
		f.fake->Deliver(peer, Protocol::Encode(RemoteHello("J", 0x100 + peer)));
		REQUIRE(f.fake->WaitFor(peer, Type(MT::kJoinWait)));
	}
	const PeerId extra = 40;
	f.fake->SetSource(extra, REMOTE_B);
	f.fake->Connect(extra);
	f.fake->Deliver(extra, Protocol::Encode(RemoteHello("Late", 0x999)));
	const auto reject = Refused(f, extra);
	REQUIRE(reject);
	CHECK(reject->reason == Protocol::REJECT_JOIN_TIMEOUT);
}

#endif

TEST("join approval: the messages round-trip and a wrong size is no challenge")
{
	const Protocol::Challenge challenge{ std::vector<std::uint8_t>(Protocol::CHALLENGE_SALT_SIZE, 1), std::vector<std::uint8_t>(Protocol::CHALLENGE_NONCE_SIZE, 2) };
	CHECK(Protocol::DecodeChallenge(Protocol::Encode(challenge)) == challenge);
	CHECK(!Protocol::DecodeChallenge(Protocol::Encode(Protocol::Challenge{ { 1, 2 }, { 3 } })));
	CHECK(Protocol::DecodeJoinWait(Protocol::Encode(Protocol::JoinWait{})));
	auto hello = Fixture::MakeHello("A", 1);
	hello.proof = { 1, 2, 3 };  // not a proof's size
	CHECK(!Protocol::DecodeHello(Protocol::Encode(hello)));
	hello.proof.assign(Protocol::PROOF_SIZE, 9);
	CHECK(Protocol::DecodeHello(Protocol::Encode(hello))->proof == hello.proof);
	Protocol::HostInfo info;
	info.waiting = { { 3, "Bob" }, { 4, "Carol" } };
	CHECK(Protocol::DecodeHostInfo(Protocol::Encode(info)) == info);
}
