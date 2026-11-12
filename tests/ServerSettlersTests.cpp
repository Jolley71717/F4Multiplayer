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
