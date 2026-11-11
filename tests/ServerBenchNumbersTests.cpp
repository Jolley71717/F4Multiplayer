// The host's settlement figures against a real server (common/BenchNumbersBook.h in Server.cpp; the rules of the two
// games are in WorkshopNumbersTests.cpp): only the host's game is heard, the others are told what changed and a joiner
// all of it, and the host's game leaving makes everyone forget them.

#include "ServerFixture.h"

#include "BenchNumbersBook.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Figures = Protocol::BenchFigures;

	constexpr std::uint64_t HOST = 0x11;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;   // its workbench
	constexpr std::uint32_t RED_ROCKET = 0x00054BAE;

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

	// A settlement of the host's with that much food (the rest as a small settlement's).
	Figures Built(std::uint32_t a_bench, std::uint16_t a_food = 2)
	{
		return Figures{ a_bench, Figures::kHosts | Figures::kBedsShort, 5, 3, a_food, 6, 8, 2, 3, 55, 50 };
	}

	auto Report(std::vector<Figures> a_benches) { return Protocol::Encode(Protocol::BenchNumbers{ std::move(a_benches) }); }

	std::vector<Protocol::BenchNumbers> Numbers(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kBenchNumbers, [](auto a_data) { return Protocol::DecodeBenchNumbers(a_data); });
	}

	std::vector<Protocol::BenchNumbers> Numbers(Fixture& a_f, PeerId a_peer) { return Numbers(Received(a_f, a_peer)); }
}

TEST("server settlement figures: the host's game's figures reach the others, only what changed, and never its own game")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report({ Built(SANCTUARY), Built(RED_ROCKET) }));
	auto bob = Numbers(f, 2);
	REQUIRE(bob.size() == 1);
	CHECK((bob[0].benches == std::vector<Figures>{ Built(SANCTUARY), Built(RED_ROCKET) }));
	CHECK(Numbers(f, 1).empty());

	// Told again as they are (the host's game does, every so often): nobody hears it. One of them changed: that one alone.
	f.fake->Deliver(1, Report({ Built(SANCTUARY), Built(RED_ROCKET) }));
	CHECK(Numbers(f, 2).empty());
	f.fake->Deliver(1, Report({ Built(SANCTUARY), Built(RED_ROCKET, 7) }));
	bob = Numbers(f, 2);
	REQUIRE(bob.size() == 1);
	CHECK(bob[0].benches == std::vector<Figures>{ Built(RED_ROCKET, 7) });

	// A settlement that isn't the host's any more: told once, as such.
	f.fake->Deliver(1, Report({ Figures{ .bench = SANCTUARY } }));
	f.fake->Deliver(1, Report({ Figures{ .bench = SANCTUARY } }));
	bob = Numbers(f, 2);
	REQUIRE(bob.size() == 1);
	CHECK(bob[0].benches == std::vector<Figures>{ Figures{ .bench = SANCTUARY } });

	// The host's game after a load: forget them all, then what it has now.
	f.fake->Deliver(1, Report({}));
	f.fake->Deliver(1, Report({ Built(RED_ROCKET, 7) }));
	bob = Numbers(f, 2);
	REQUIRE(bob.size() == 2);
	CHECK(bob[0].benches.empty());
	CHECK(bob[1].benches == std::vector<Figures>{ Built(RED_ROCKET, 7) });
}

TEST("server settlement figures: a friend's game has no say, and neither has the host's game when the host doesn't share its settlements")
{
	{
		Fixture f{ Hosted() };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		JoinAs(f, 3, "Carol", 0x33);
		Clear(f, { 1, 2, 3 });
		f.fake->Deliver(2, Report({ Built(SANCTUARY) }));
		CHECK(Numbers(f, 3).empty());
		CHECK(Numbers(f, 1).empty());
		// ... nor can it make the others forget the host's.
		f.fake->Deliver(1, Report({ Built(SANCTUARY) }));
		Clear(f, { 1, 2, 3 });
		f.fake->Deliver(2, Report({}));
		f.fake->Deliver(2, Report({ Figures{ .bench = SANCTUARY } }));
		CHECK(Numbers(f, 3).empty());
		JoinAs(f, 4, "Dave", 0x44);
		const auto dave = Numbers(f, 4);
		REQUIRE(dave.size() == 1);
		CHECK(dave[0].benches == std::vector<Figures>{ Built(SANCTUARY) });
	}
	{
		Fixture f{ Hosted(false) };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Report({ Built(SANCTUARY) }));
		CHECK(Numbers(f, 2).empty());
		JoinAs(f, 3, "Carol", 0x33);
		CHECK(Numbers(f, 3).empty());
	}
	{
		// A dedicated server has no host's game: nobody's figures are anyone else's.
		Fixture f;
		JoinAs(f, 2, "Bob", 0x22);
		JoinAs(f, 3, "Carol", 0x33);
		Clear(f, { 2, 3 });
		f.fake->Deliver(2, Report({ Built(SANCTUARY) }));
		CHECK(Numbers(f, 3).empty());
	}
}

TEST("server settlement figures: a joiner hears them all as they are now, by workbench")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	f.fake->Deliver(1, Report({ Built(SANCTUARY), Built(RED_ROCKET) }));
	f.fake->Deliver(1, Report({ Built(SANCTUARY, 9) }));
	Clear(f, { 1 });

	JoinAs(f, 2, "Bob", 0x22);
	const auto bob = Numbers(f, 2);
	REQUIRE(bob.size() == 1);
	CHECK((bob[0].benches == std::vector<Figures>{ Built(SANCTUARY, 9), Built(RED_ROCKET) }));  // by workbench
	CHECK(Numbers(f, 1).empty());
}

TEST("server settlement figures: the host's game leaving makes everyone forget them; a joiner hears none, and nothing is said when there were none")
{
	{
		Fixture f{ Hosted() };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		f.fake->Deliver(1, Report({ Built(SANCTUARY) }));
		Clear(f, { 1, 2 });

		f.fake->Drop(1);
		std::vector<std::vector<std::uint8_t>> got;
		REQUIRE(Await(f, 2, MT::kBenchNumbers, got));
		const auto words = Numbers(got);
		REQUIRE(words.size() == 1);
		CHECK(words[0].benches.empty());

		JoinAs(f, 3, "Carol", 0x33);
		CHECK(Count(Received(f, 3), MT::kBenchNumbers) == 0);
	}
	{
		Fixture f{ Hosted() };
		JoinHost(f, 1);
		JoinAs(f, 2, "Bob", 0x22);
		Clear(f, { 1, 2 });
		f.fake->Drop(1);
		CHECK(f.LoggedSoon("left"));
		CHECK(Count(Received(f, 2), MT::kBenchNumbers) == 0);
	}
}

TEST("server settlement figures: a malformed report changes nothing")
{
	Fixture f{ Hosted() };
	JoinHost(f, 1);
	JoinAs(f, 2, "Bob", 0x22);
	f.fake->Deliver(1, Report({ Built(SANCTUARY) }));
	Clear(f, { 1, 2 });

	auto odd = Report({ Built(SANCTUARY, 9) });
	odd.push_back(0);  // a byte too many
	f.fake->Deliver(1, odd);
	auto runtime = Built(0xFF000123, 4);  // a runtime reference is no settlement
	f.fake->Deliver(1, Report({ runtime }));
	CHECK(Numbers(f, 2).empty());
	JoinAs(f, 3, "Carol", 0x33);
	const auto carol = Numbers(f, 3);
	REQUIRE(carol.size() == 1);
	CHECK(carol[0].benches == std::vector<Figures>{ Built(SANCTUARY) });
}
