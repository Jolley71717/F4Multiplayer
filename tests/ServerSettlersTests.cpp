// The settlers of the host's settlements against a real server (common/SettlerRoster.h in Server.cpp): the host's game's
// lists reach the others and joiners, a friend's game has no say, a telling-again of the same is told to nobody, and
// the host's game leaving (or loading) makes everyone forget them.

#include "ServerFixture.h"

#include "SettlerRoster.h"

using namespace TestServer;

namespace
{
	using Protocol::BenchSettlers;

	constexpr std::uint64_t HOST = 0x11;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;  // its workbench
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t CODSWORTH = 0x0001CA7D;
	constexpr std::uint32_t STURGES = 0x0001E7D7;

	Server::Options Hosted(bool a_shared = true)
	{
		Server::Options options;
		options.hostIdentity = HOST;
		options.rules.sharedWorkshop = a_shared;
		return options;
	}

	void JoinHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		JoinAs(a_f, a_peer, "Host", HOST);
	}

	auto Report(const BenchSettlers& a_list) { return Protocol::Encode(a_list, MT::kReportSettlers); }

	std::vector<BenchSettlers> Lists(Fixture& a_f, PeerId a_peer)
	{
		return Decoded(Received(a_f, a_peer), MT::kSettlers, [](auto a_data) { return Protocol::DecodeBenchSettlers(a_data); });
	}
}

TEST("server settlers: the host's game's settlers of a settlement reach the others and a joiner; the same again reaches nobody")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(BenchSettlers{ SANCTUARY, { STURGES, CODSWORTH } }));
	Sync(f, 1);
	auto lists = Lists(f, 2);
	REQUIRE(lists.size() == 1);
	CHECK((lists[0] == BenchSettlers{ SANCTUARY, { CODSWORTH, STURGES } }));  // in the book's order
	CHECK(Count(Received(f, 1), MT::kSettlers) == 0);                       // not back to the host's game

	// The same, in another order: nobody told.
	f.fake->Deliver(1, Report(BenchSettlers{ SANCTUARY, { CODSWORTH, STURGES } }));
	Sync(f, 1);
	CHECK(Lists(f, 2).empty());

	// A joiner gets every list kept.
	f.fake->Deliver(1, Report(BenchSettlers{ RED_ROCKET, { 0xFF000812 } }));
	Sync(f, 1);
	CHECK(Lists(f, 2).size() == 1);
	JoinAs(f, 3, "Carol", 0x33);
	lists = Lists(f, 3);
	CHECK(lists.size() == 2);

	// None left at a settlement: told once, and a joiner hears nothing of it.
	f.fake->Deliver(1, Report(BenchSettlers{ RED_ROCKET, {} }));
	Sync(f, 1);
	lists = Lists(f, 2);
	REQUIRE(lists.size() == 1);
	CHECK((lists[0] == BenchSettlers{ RED_ROCKET, {} }));
	f.fake->Deliver(1, Report(BenchSettlers{ RED_ROCKET, {} }));
	Sync(f, 1);
	CHECK(Lists(f, 2).empty());
	JoinAs(f, 4, "Dave", 0x44);
	lists = Lists(f, 4);
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].bench == SANCTUARY);

	// The host's world loaded: forget them all, told to everyone once.
	f.fake->Deliver(1, Report(BenchSettlers{}));
	Sync(f, 1);
	lists = Lists(f, 2);
	REQUIRE(lists.size() == 1);
	CHECK((lists[0] == BenchSettlers{}));
}

TEST("server settlers: a friend's game has no say in the host's settlers, and with the rule off nobody does")
{
	{
		Fixture f{ Hosted() };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		JoinAs(f, 3, "Carol", 0x33);
		Clear(f, { 1, 2, 3 });
		f.fake->Deliver(2, Report(BenchSettlers{ SANCTUARY, { CODSWORTH } }));
		Sync(f, 2);
		CHECK(Lists(f, 3).empty());
		CHECK(Lists(f, 1).empty());
		JoinAs(f, 4, "Dave", 0x44);
		CHECK(Lists(f, 4).empty());
	}
	{
		Fixture f{ Hosted(false) };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Report(BenchSettlers{ SANCTUARY, { CODSWORTH } }));
		Sync(f, 1);
		CHECK(Lists(f, 2).empty());
	}
}

TEST("server settlers: the host's game leaving makes everyone forget its settlers; a joiner hears none of them")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	f.fake->Deliver(1, Report(BenchSettlers{ SANCTUARY, { CODSWORTH } }));
	Sync(f, 1);
	Clear(f, { 1, 2 });

	f.fake->Drop(1);
	std::vector<std::vector<std::uint8_t>> got;
	REQUIRE(Await(f, 2, MT::kSettlers, got));
	const auto lists = Decoded(got, MT::kSettlers, [](auto a_data) { return Protocol::DecodeBenchSettlers(a_data); });
	REQUIRE(lists.size() == 1);
	CHECK((lists[0] == BenchSettlers{}));

	JoinAs(f, 3, "Carol", 0x33);
	CHECK(Count(Received(f, 3), MT::kSettlers) == 0);
}

TEST("server settlers: a settlement's recruits loading at once leave a fight's spawns their whole burst")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	f.HoldClock();  // no token comes back while the test runs: only the burst counts
	Clear(f, { 1, 2 });

	constexpr std::uint32_t RAIDER = 0x0002898B;  // LvlRaider
	constexpr std::uint32_t WORLD = 0x0000003C;
	const auto spawn = [&](std::uint32_t a_id, Protocol::SpawnKind a_kind) {
		const Protocol::Spawn s{ 0, a_id, RAIDER, 0, WORLD, 1000.0f + static_cast<float>(a_id & 0xFF), 2000.0f, 100.0f, 0.0f,
			a_kind == Protocol::SpawnKind::kSettler ? SANCTUARY : 0u, 0, a_kind };
		f.fake->Deliver(1, Protocol::Encode(s, MT::kReportSpawn));
	};
	// The settlers' places' worth, then a fight's worth.
	for (std::uint32_t i = 0; i < Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER; ++i) {
		spawn(0xFF001000 + i, Protocol::SpawnKind::kSettler);
	}
	for (std::uint32_t i = 0; i < Protocol::MAX_SPAWNS_PER_PLAYER; ++i) {
		spawn(0xFF002000 + i, Protocol::SpawnKind::kPlain);
	}
	Sync(f, 1);
	const auto spawned = Decoded(Received(f, 2), MT::kSpawned, [](auto a_data) { return Protocol::DecodeSpawn(a_data); });
	std::size_t settlers = 0, fight = 0;
	for (const auto& s : spawned) {
		++(s.kind == Protocol::SpawnKind::kSettler ? settlers : fight);
	}
	CHECK(settlers == Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER);
	CHECK(fight == Protocol::MAX_SPAWNS_PER_PLAYER);
	CHECK(!f.Logged("they share them faster than any fight makes them"));
}

TEST("server settlers: the host's game's settlements and Local Leader reach the others and a joiner, from the host's game alone")
{
	using Protocol::HostSettlements;
	const auto told = [](Fixture& a_f, PeerId a_peer) {
		return Decoded(Received(a_f, a_peer), MT::kHostSettlements, [](auto a_data) { return Protocol::DecodeHostSettlements(a_data); });
	};
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	// A friend's game has no say.
	f.fake->Deliver(2, Protocol::Encode(HostSettlements{ 2, { SANCTUARY } }, MT::kReportHostSettlements));
	Sync(f, 2);
	CHECK(told(f, 1).empty());

	f.fake->Deliver(1, Protocol::Encode(HostSettlements{ 1, { SANCTUARY, RED_ROCKET } }, MT::kReportHostSettlements));
	Sync(f, 1);
	auto heard = told(f, 2);
	REQUIRE(heard.size() == 1);
	CHECK((heard[0] == HostSettlements{ 1, { SANCTUARY, RED_ROCKET } }));
	CHECK(Count(Received(f, 1), MT::kHostSettlements) == 0);  // not back to the host's game
	// The same again: nobody told.
	f.fake->Deliver(1, Protocol::Encode(HostSettlements{ 1, { SANCTUARY, RED_ROCKET } }, MT::kReportHostSettlements));
	Sync(f, 1);
	CHECK(told(f, 2).empty());
	// A joiner hears them.
	JoinAs(f, 3, "Carol", 0x33);
	heard = told(f, 3);
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].benches.size() == 2);

	// The host's game leaving: everyone forgets them.
	Clear(f, { 2, 3 });
	f.fake->Drop(1);
	std::vector<std::vector<std::uint8_t>> got;
	REQUIRE(Await(f, 2, MT::kHostSettlements, got));
	const auto forget = Decoded(got, MT::kHostSettlements, [](auto a_data) { return Protocol::DecodeHostSettlements(a_data); });
	REQUIRE(forget.size() == 1);
	CHECK((forget[0] == HostSettlements{}));
	JoinAs(f, 4, "Dave", 0x44);
	CHECK(Count(Received(f, 4), MT::kHostSettlements) == 0);

	// With the rule off nobody's word is kept.
	Fixture off{ Hosted(false) };
	JoinHost(off, 1);
	JoinAs(off, 2, "Bob", 0x22);
	Clear(off, { 1, 2 });
	off.fake->Deliver(1, Protocol::Encode(HostSettlements{ 1, { SANCTUARY } }, MT::kReportHostSettlements));
	Sync(off, 1);
	CHECK(told(off, 2).empty());
}

TEST("server settlers: the host's settlers and settlements get through while its event budget is spent")
{
	using Protocol::HostSettlements;
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	// A load's catch-up traffic: the host's game spends its whole budget.
	for (int i = 0; i < 600; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::PlayerStatus{}, MT::kReportStatus));
	}
	f.fake->Deliver(1, Report(BenchSettlers{ SANCTUARY, { CODSWORTH } }));
	f.fake->Deliver(1, Protocol::Encode(HostSettlements{ 1, { SANCTUARY } }, MT::kReportHostSettlements));
	Sync(f, 1);
	CHECK(f.Logged("sends too many events"));
	const auto got = Received(f, 2);
	const auto lists = Decoded(got, MT::kSettlers, [](auto a_data) { return Protocol::DecodeBenchSettlers(a_data); });
	REQUIRE(lists.size() == 1);
	CHECK((lists[0] == BenchSettlers{ SANCTUARY, { CODSWORTH } }));
	const auto heard = Decoded(got, MT::kHostSettlements, [](auto a_data) { return Protocol::DecodeHostSettlements(a_data); });
	REQUIRE(heard.size() == 1);
	CHECK((heard[0] == HostSettlements{ 1, { SANCTUARY } }));
}
