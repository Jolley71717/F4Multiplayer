// The host's supply lines at a friend's builds (game/SupplyLines.h): what the host's game tells the session, what a friend's
// game hears, and the order the engine's gather finds the workbenches in. The session end to end is in StorageBenchTests.

#include "Test.h"

#include "Protocol.h"
#include "game/MenuKinds.h"
#include "game/OpenMenus.h"
#include "game/SupplyLines.h"

#include <cstdint>
#include <vector>

namespace
{
	constexpr std::uint32_t BENCH = 0x0002'50FE;
	constexpr std::uint32_t A = 0x0002'5101;
	constexpr std::uint32_t B = 0x0002'5102;
	constexpr std::uint32_t C = 0x0002'5103;

	using Ids = std::vector<std::uint32_t>;

	bool Any(std::uint32_t)
	{
		return true;
	}
}

TEST("supply lines: the host's game tells a workbench's lines once, sorted, without itself, nothing or repeats")
{
	SupplyLines::Reporter reporter;
	CHECK(!reporter.Update(BENCH, {}));  // none linked: nothing to tell yet
	CHECK(!reporter.Update(BENCH, { BENCH, 0 }));
	const auto first = reporter.Update(BENCH, { B, A, B, BENCH, 0 });
	REQUIRE(first);
	CHECK(first->bench == BENCH);
	CHECK((first->links == Ids{ A, B }));
	CHECK(!reporter.Update(BENCH, { A, B }));
	CHECK(!reporter.Update(BENCH, { B, A }));
	const auto fewer = reporter.Update(BENCH, { A });
	REQUIRE(fewer);
	CHECK(fewer->links == Ids{ A });
	const auto none = reporter.Update(BENCH, {});  // the line taken back: told as none
	REQUIRE(none);
	CHECK(none->links.empty());
	CHECK(!reporter.Update(BENCH, {}));
	CHECK(reporter.Reports() == 3);
}

TEST("supply lines: after a welcome or a load the host's game tells its lines again")
{
	SupplyLines::Reporter reporter;
	REQUIRE(reporter.Update(BENCH, { A }));
	CHECK(!reporter.Update(BENCH, { A }));
	reporter.Forget();
	CHECK(reporter.Benches() == 0);
	CHECK(reporter.Update(BENCH, { A }));
}

TEST("supply lines: no more lines go out for a workbench than the wire carries")
{
	SupplyLines::Reporter reporter;
	Ids many;
	for (std::uint32_t i = 0; i < Protocol::MAX_BENCH_LINKS + 10; ++i) {
		many.push_back(0x0003'0000 + i);
	}
	const auto report = reporter.Update(BENCH, many);
	REQUIRE(report);
	CHECK(report->links.size() == Protocol::MAX_BENCH_LINKS);
	const auto packet = Protocol::Encode(*report, Protocol::MessageType::kReportBenchLinks);
	const auto back = Protocol::DecodeBenchLinks(packet);
	REQUIRE(back);
	CHECK(*back == *report);
}

TEST("supply lines: a friend's game views the lines another game keeps, in the keeper's order; none taken back")
{
	SupplyLines::Heard heard;
	CHECK(heard.With(BENCH, Any).empty());
	heard.Take({ BENCH, { B, A, C } });
	CHECK((heard.With(BENCH, Any) == Ids{ B, A, C }));
	CHECK((heard.With(BENCH, [](std::uint32_t a_id) { return a_id != A; }) == Ids{ B, C }));  // A kept by nobody (or by us)
	CHECK(heard.With(A, Any).empty());
	heard.Take({ BENCH, { C } });
	CHECK(heard.With(BENCH, Any) == Ids{ C });
	heard.Take({ BENCH, {} });
	CHECK(heard.With(BENCH, Any).empty());
	CHECK(heard.Benches() == 0);
	heard.Take({ BENCH, { A } });
	heard.Clear();
	CHECK(heard.Benches() == 0);
}

TEST("supply lines: the gather finds the keeper's lines after what it held, then our world's others, none twice")
{
	// The workbench (and a crafting station's own containers) first, as the engine put them in.
	const Ids have{ BENCH };
	CHECK(SupplyLines::Gathered(have, {}, {}).empty());
	CHECK(SupplyLines::Gathered(have, Ids{ C }, {}) == Ids{ C });
	CHECK((SupplyLines::Gathered(have, {}, Ids{ B, A }) == Ids{ B, A }));
	// Our world links A and C; the keeper's lines B and A: the keeper's first, in its order, then our own C.
	CHECK((SupplyLines::Gathered(have, Ids{ A, C }, Ids{ B, A }) == Ids{ B, A, C }));
	// Nothing already in the list, no repeats, no nothing.
	CHECK((SupplyLines::Gathered(have, Ids{ BENCH, 0, C, C }, Ids{ BENCH, B, B, 0 }) == Ids{ B, C }));
}

TEST("supply lines: the wire refuses a list with the workbench itself, repeats, nothing, our player, or bytes after it")
{
	using Protocol::BenchLinks;
	using MT = Protocol::MessageType;
	const auto good = Protocol::Encode(BenchLinks{ BENCH, { A, B } }, MT::kBenchLinks);
	CHECK(Protocol::DecodeBenchLinks(good));
	CHECK(Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ BENCH, {} }, MT::kBenchLinks)));
	CHECK(!Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ BENCH, { A, BENCH } }, MT::kBenchLinks)));
	CHECK(!Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ BENCH, { A, A } }, MT::kBenchLinks)));
	CHECK(!Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ BENCH, { 0 } }, MT::kBenchLinks)));
	CHECK(!Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ BENCH, { Protocol::PLAYER_REF_ID } }, MT::kBenchLinks)));
	CHECK(!Protocol::DecodeBenchLinks(Protocol::Encode(BenchLinks{ 0xFF00'0801, { A } }, MT::kBenchLinks)));
	auto longer = good;
	longer.push_back(0);
	CHECK(!Protocol::DecodeBenchLinks(longer));
	auto shorter = good;
	shorter.pop_back();
	CHECK(!Protocol::DecodeBenchLinks(shorter));
}

TEST("supply lines: the robot workbench's menu is a crafting menu, known by the game's name")
{
	OpenMenus::Book book;
	book.Seen("RobotModMenu", true);
	CHECK(book.Open(OpenMenus::Menu::kRobotMod));
	CHECK(MenuKinds::Of(book) == Protocol::MenuKind::kCrafting);
}
