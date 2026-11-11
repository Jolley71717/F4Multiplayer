// Settlement attacks against a real server (common/AttackBook.h in Server.cpp): the host's game's attacks reach the
// others and joiners, a friend's roll is a question the server answers from what the host's game said or asks of it,
// and the host's game leaving makes everyone forget it all.

#include "ServerFixture.h"

#include "AttackBook.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Phase = Protocol::AttackPhase;
	using Outcome = Protocol::AttackOutcome;

	constexpr std::uint64_t HOST = 0x11;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;   // its workbench
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;  // a workbench (any plugin's reference serves)
	constexpr std::uint32_t RAIDERS = 0x001125E2;     // WorkshopRaiderAttack01
	constexpr std::uint32_t LOCATION = 0x0001F228;

	Server::Options Hosted()
	{
		Server::Options options;
		options.hostIdentity = HOST;
		return options;
	}

	void JoinHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		JoinAs(a_f, a_peer, "Host", HOST);
	}

	auto Attack(std::uint32_t a_workshop, Phase a_phase, Outcome a_outcome = Outcome::kNone)
	{
		return Protocol::Encode(Protocol::SettlementAttack{ a_workshop, RAIDERS, LOCATION, a_phase, a_outcome });
	}

	auto Owner(std::uint32_t a_workshop, bool a_owned) { return Protocol::Encode(Protocol::SettlementOwner{ a_workshop, a_owned }); }

	std::vector<Protocol::SettlementAttack> Attacks(Fixture& a_f, PeerId a_peer)
	{
		return Decoded(Received(a_f, a_peer), MT::kSettlementAttack, [](auto a_data) { return Protocol::DecodeSettlementAttack(a_data); });
	}

	std::vector<Protocol::SettlementOwner> Owners(Fixture& a_f, PeerId a_peer)
	{
		return Decoded(Received(a_f, a_peer), MT::kSettlementOwner, [](auto a_data) { return Protocol::DecodeSettlementOwner(a_data); });
	}
}

TEST("server attacks: the host's attack reaches the others each phase, once each, and not its own game")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kRolled));
	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kRolled));  // said again: nothing new
	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kUnderway));
	auto bob = Attacks(f, 2);
	REQUIRE(bob.size() == 2);
	CHECK(bob[0].workshop == SANCTUARY && bob[0].quest == RAIDERS && bob[0].location == LOCATION && bob[0].phase == Phase::kRolled);
	CHECK(bob[1].phase == Phase::kUnderway);
	CHECK(Attacks(f, 1).empty());

	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kOver, Outcome::kDefended));
	bob = Attacks(f, 2);
	REQUIRE(bob.size() == 1);
	CHECK(bob[0].phase == Phase::kOver && bob[0].outcome == Outcome::kDefended);
	CHECK(f.LoggedSoon(std::format("server: the host's attack {:08X} on settlement {:08X}", RAIDERS, SANCTUARY)));
}

TEST("server attacks: a joiner hears the host's settlements and the attacks going on, and none that are over")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	f.fake->Deliver(1, Owner(SANCTUARY, true));
	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kUnderway));
	Clear(f, { 1 });

	JoinAs(f, 2, "Bob", 0x22);
	const auto packets = Received(f, 2);
	const auto owners = Decoded(packets, MT::kSettlementOwner, [](auto a_data) { return Protocol::DecodeSettlementOwner(a_data); });
	const auto attacks = Decoded(packets, MT::kSettlementAttack, [](auto a_data) { return Protocol::DecodeSettlementAttack(a_data); });
	REQUIRE(owners.size() == 1);
	CHECK(owners[0].workshop == SANCTUARY && owners[0].hostOwned);
	REQUIRE(attacks.size() == 1);
	CHECK(attacks[0].phase == Phase::kUnderway && attacks[0].workshop == SANCTUARY);

	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kOver, Outcome::kResolved));
	Clear(f, { 1, 2 });
	JoinAs(f, 3, "Carol", 0x33);
	CHECK(Count(Received(f, 3), MT::kSettlementAttack) == 0);
}

TEST("server attacks: a friend's roll is asked of the host's game once, its answer reaches everyone, and later rolls are answered from it")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	JoinAs(f, 3, "Carol", 0x33);
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(2, Attack(SANCTUARY, Phase::kRolled));
	f.fake->Deliver(3, Attack(SANCTUARY, Phase::kRolled));  // within ASK_EVERY: not asked again
	const auto asks = Attacks(f, 1);
	REQUIRE(asks.size() == 1);
	CHECK(asks[0].workshop == SANCTUARY && asks[0].phase == Phase::kRolled);
	CHECK(Attacks(f, 3).empty());  // a friend's roll is no attack of the host's

	f.fake->Deliver(1, Owner(SANCTUARY, true));
	for (const PeerId peer : { 2, 3 }) {
		const auto words = Owners(f, peer);
		REQUIRE(words.size() == 1);
		CHECK(words[0].workshop == SANCTUARY && words[0].hostOwned);
	}
	CHECK(Owners(f, 1).empty());
	f.fake->Deliver(1, Owner(SANCTUARY, true));  // the same word: nobody hears it again
	CHECK(Owners(f, 2).empty());

	// Bob rolls there again: answered by the server at once, the host's game not asked.
	f.fake->Deliver(2, Attack(SANCTUARY, Phase::kRolled));
	const auto words = Owners(f, 2);
	REQUIRE(words.size() == 1);
	CHECK(words[0].hostOwned);
	CHECK(Attacks(f, 1).empty());
}

TEST("server attacks: a friend can't speak for the host: its phases, and its word on a settlement, are dropped")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	JoinAs(f, 3, "Carol", 0x33);
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(2, Attack(SANCTUARY, Phase::kUnderway));
	f.fake->Deliver(2, Owner(SANCTUARY, false));
	CHECK(Attacks(f, 3).empty());
	CHECK(Owners(f, 3).empty());
	CHECK(Attacks(f, 1).empty());
	CHECK(Owners(f, 1).empty());
}

TEST("server attacks: on a dedicated server every settlement is the friend's own; a hosted one without its host's game answers nothing")
{
	{
		Fixture f;
		JoinAs(f, 2, "Bob", 0x22);
		Clear(f, { 2 });
		f.fake->Deliver(2, Attack(RED_ROCKET, Phase::kRolled));
		const auto words = Owners(f, 2);
		REQUIRE(words.size() == 1);
		CHECK(words[0].workshop == RED_ROCKET && !words[0].hostOwned);
	}
	{
		Fixture f{ Hosted() };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		Clear(f, { 1, 2 });
		f.fake->Drop(1);
		CHECK(f.LoggedSoon("left"));
		Clear(f, { 2 });
		f.fake->Deliver(2, Attack(RED_ROCKET, Phase::kRolled));
		CHECK(Owners(f, 2).empty());
	}
}

TEST("server attacks: the host's game leaving makes everyone forget its settlements and attacks; a joiner hears none of them")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	f.fake->Deliver(1, Owner(SANCTUARY, true));
	f.fake->Deliver(1, Attack(SANCTUARY, Phase::kUnderway));
	Clear(f, { 1, 2 });

	f.fake->Drop(1);
	std::vector<std::vector<std::uint8_t>> got;
	REQUIRE(Await(f, 2, MT::kSettlementOwner, got));
	const auto words = Decoded(got, MT::kSettlementOwner, [](auto a_data) { return Protocol::DecodeSettlementOwner(a_data); });
	REQUIRE(words.size() == 1);
	CHECK(words[0].workshop == 0 && !words[0].hostOwned);

	JoinAs(f, 3, "Carol", 0x33);
	const auto packets = Received(f, 3);
	CHECK(Count(packets, MT::kSettlementOwner) == 0);
	CHECK(Count(packets, MT::kSettlementAttack) == 0);
}
