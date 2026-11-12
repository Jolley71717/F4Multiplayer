// The settlers of the host's settlements as the host's game names them (common/SettlerRoster.h): the wire's message, what
// the host's game tells, what the server and a friend's game keep, and when a friend's workshop mode offers Command on
// them. The server's relay is in ServerSettlersTests.

#include "Test.h"

#include "Protocol.h"
#include "SettlerRoster.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace
{
	using MT = Protocol::MessageType;
	using Protocol::BenchSettlers;

	constexpr std::uint32_t BENCH = 0x000250FE;     // Sanctuary's workbench
	constexpr std::uint32_t OTHER = 0x0001F0C5;     // another settlement's
	constexpr std::uint32_t CODSWORTH = 0x0001CA7D;  // a plugin's NPC
	constexpr std::uint32_t STURGES = 0x0001E7D7;
	constexpr std::uint32_t RECRUIT = 0xFF000812;    // a spawn id

	using Now = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;
}

TEST("settler roster: a settlement's settlers go on the wire and back, and nothing else decodes")
{
	for (const auto type : { MT::kReportSettlers, MT::kSettlers }) {
		const BenchSettlers list{ BENCH, { CODSWORTH, RECRUIT } };
		const auto          back = Protocol::DecodeBenchSettlers(Protocol::Encode(list, type));
		REQUIRE(back);
		CHECK(*back == list);
		// Forget them all, and a settlement with none left.
		CHECK((Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{}, type)) == BenchSettlers{}));
		CHECK((Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ BENCH, {} }, type)) == BenchSettlers{ BENCH, {} }));
		// No workbench with settlers, a runtime workbench, a settler named twice, the workbench as a settler, nobody (0), the
		// player.
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ 0, { CODSWORTH } }, type))));
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ 0xFF000801, { CODSWORTH } }, type))));
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ BENCH, { CODSWORTH, CODSWORTH } }, type))));
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ BENCH, { BENCH } }, type))));
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ BENCH, { 0 } }, type))));
		CHECK((!Protocol::DecodeBenchSettlers(Protocol::Encode(BenchSettlers{ BENCH, { Protocol::PLAYER_REF_ID } }, type))));
		// A byte short, a byte over.
		auto packet = Protocol::Encode(list, type);
		packet.pop_back();
		CHECK(!Protocol::DecodeBenchSettlers(packet));
		packet = Protocol::Encode(list, type);
		packet.push_back(0);
		CHECK(!Protocol::DecodeBenchSettlers(packet));
	}
	// More than a message holds is cut to MAX_BENCH_SETTLERS on the way out.
	BenchSettlers many{ BENCH, {} };
	for (std::uint32_t i = 0; i < Protocol::MAX_BENCH_SETTLERS + 5; ++i) {
		many.settlers.push_back(0x00100000 + i);
	}
	const auto cut = Protocol::DecodeBenchSettlers(Protocol::Encode(many, MT::kSettlers));
	REQUIRE(cut);
	CHECK(cut->settlers.size() == Protocol::MAX_BENCH_SETTLERS);
}

TEST("settler roster: the host's game tells a settlement's settlers when they change, and an empty list once none are loaded")
{
	SettlerRoster::Reporter reporter;
	// The first round tells each settlement with settlers, in ascending order, each once; a settlement with none is told
	// nothing, nor one whose workbench no other game has.
	auto out = reporter.Round(Now{ { BENCH, { STURGES, CODSWORTH, CODSWORTH, 0 } }, { OTHER, {} }, { 0xFF000900, { RECRUIT } } });
	REQUIRE(out.size() == 1);
	CHECK((out[0] == BenchSettlers{ BENCH, { CODSWORTH, STURGES } }));
	CHECK(reporter.Benches() == 1);
	// The same again, in another order: nothing.
	CHECK((reporter.Round(Now{ { BENCH, { CODSWORTH, STURGES } } }).empty()));
	CHECK((reporter.Round(Now{ { BENCH, { STURGES, CODSWORTH } }, { OTHER, {} } }).empty()));
	// One more: the whole list again.
	out = reporter.Round(Now{ { BENCH, { STURGES, CODSWORTH, RECRUIT } } });
	REQUIRE(out.size() == 1);
	CHECK((out[0] == BenchSettlers{ BENCH, { CODSWORTH, STURGES, RECRUIT } }));
	// Unloaded (or the settlement lost): an empty list, once.
	out = reporter.Round(Now{});
	REQUIRE(out.size() == 1);
	CHECK((out[0] == BenchSettlers{ BENCH, {} }));
	CHECK((reporter.Round(Now{}).empty()));
	CHECK((reporter.Round(Now{ { BENCH, {} } }).empty()));
	// Two settlements: both told; then one goes.
	out = reporter.Round(Now{ { BENCH, { CODSWORTH } }, { OTHER, { STURGES } } });
	CHECK(out.size() == 2);
	out = reporter.Round(Now{ { OTHER, { STURGES } } });
	REQUIRE(out.size() == 1);
	CHECK((out[0] == BenchSettlers{ BENCH, {} }));
	// After a welcome everything is told again.
	reporter.Forget();
	out = reporter.Round(Now{ { OTHER, { STURGES } } });
	REQUIRE(out.size() == 1);
	CHECK((out[0] == BenchSettlers{ OTHER, { STURGES } }));
	CHECK(reporter.Reports() == 7);
}

TEST("settler roster: the host's game names at most MAX_BENCH_SETTLERS settlers of a settlement, and keeps at most MAX_BENCHES settlements")
{
	SettlerRoster::Reporter    reporter;
	std::vector<std::uint32_t> many;
	for (std::uint32_t i = 0; i < Protocol::MAX_BENCH_SETTLERS + 10; ++i) {
		many.push_back(0x00100000 + i);
	}
	auto out = reporter.Round(Now{ { BENCH, many } });
	REQUIRE(out.size() == 1);
	CHECK(out[0].settlers.size() == Protocol::MAX_BENCH_SETTLERS);
	CHECK(Protocol::DecodeBenchSettlers(Protocol::Encode(out[0], MT::kReportSettlers)) == out[0]);

	Now benches;
	for (std::uint32_t i = 0; i < SettlerRoster::MAX_BENCHES + 3; ++i) {
		benches[0x00200000 + i] = { CODSWORTH };
	}
	SettlerRoster::Reporter wide;
	CHECK(wide.Round(benches).size() == SettlerRoster::MAX_BENCHES);
	CHECK(wide.Benches() == SettlerRoster::MAX_BENCHES);
}

TEST("settler roster: the book keeps the latest list per settlement, says when it changed, and forgets them all on workbench 0")
{
	SettlerRoster::Book book;
	CHECK(!book.Any(BENCH));
	CHECK((book.Take(BenchSettlers{ BENCH, { STURGES, CODSWORTH } })));
	CHECK(book.Any(BENCH));
	CHECK(book.Has(BENCH, CODSWORTH));
	CHECK(book.Has(BENCH, STURGES));
	CHECK(!book.Has(BENCH, RECRUIT));
	CHECK(!book.Has(OTHER, CODSWORTH));
	CHECK(book.Of(OTHER).empty());
	// The same in another order changes nothing; a new one does.
	CHECK((!book.Take(BenchSettlers{ BENCH, { CODSWORTH, STURGES } })));
	CHECK((book.Take(BenchSettlers{ BENCH, { CODSWORTH, RECRUIT } })));
	CHECK(!book.Has(BENCH, STURGES));
	CHECK(book.Has(BENCH, RECRUIT));
	CHECK((book.Take(BenchSettlers{ OTHER, { STURGES } })));
	CHECK(book.Benches() == 2);
	CHECK(book.All().size() == 2);
	// An empty list forgets that settlement (none kept: nothing changes).
	CHECK((book.Take(BenchSettlers{ OTHER, {} })));
	CHECK((!book.Take(BenchSettlers{ OTHER, {} })));
	CHECK(!book.Any(OTHER));
	// Workbench 0 forgets them all; again, nothing.
	CHECK((book.Take(BenchSettlers{})));
	CHECK(book.Benches() == 0);
	CHECK((!book.Take(BenchSettlers{})));
	CHECK(!book.Clear());
	// No more than MAX_BENCHES settlements; one kept still changes.
	for (std::uint32_t i = 0; i < SettlerRoster::MAX_BENCHES; ++i) {
		CHECK((book.Take(BenchSettlers{ 0x00200000 + i, { CODSWORTH } })));
	}
	CHECK((!book.Take(BenchSettlers{ BENCH, { CODSWORTH } })));
	CHECK((book.Take(BenchSettlers{ 0x00200000, { STURGES } })));
	CHECK(book.Benches() == SettlerRoster::MAX_BENCHES);
}

TEST("settler roster: a friend's workshop mode offers Command on the host's settlers only at a settlement the host's game runs, and only for the command keyword")
{
	using SettlerRoster::Answer;
	using SettlerRoster::Offers;
	CHECK(Offers(true, true, BENCH, true));
	CHECK(!Offers(false, true, BENCH, true));  // the rule doesn't hand over (the host's own game, a dedicated server, the host gone)
	CHECK(!Offers(true, false, BENCH, true));  // the give-command hook isn't in: a job could not be asked
	CHECK(!Offers(true, true, 0, true));       // workshop mode closed
	CHECK(!Offers(true, true, BENCH, false));  // not a settlement the host's game runs
	// The engine's yes stays yes; a no turns yes only for the command keyword on an offered settler.
	CHECK(Answer(true, false, false));
	CHECK(Answer(true, true, false));
	CHECK(Answer(false, true, true));
	CHECK(!Answer(false, true, false));
	CHECK(!Answer(false, false, true));
}
