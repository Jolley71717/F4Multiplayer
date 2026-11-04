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

TEST("supply lines: the host's game tells a workbench's lines once, in its engine's order, without itself, nothing or repeats")
{
	SupplyLines::Reporter reporter;
	CHECK(!reporter.Update(BENCH, {}));  // none linked: nothing to tell yet
	CHECK(!reporter.Update(BENCH, { BENCH, 0 }));
	const auto first = reporter.Update(BENCH, { B, A, B, BENCH, 0 });
	REQUIRE(first);
	CHECK(first->bench == BENCH);
	CHECK((first->links == Ids{ B, A }));  // the order its engine takes from them in
	CHECK(!reporter.Update(BENCH, { B, A }));
	const auto turned = reporter.Update(BENCH, { A, B });  // another order: told again
	REQUIRE(turned);
	CHECK((turned->links == Ids{ A, B }));
	const auto fewer = reporter.Update(BENCH, { A });
	REQUIRE(fewer);
	CHECK(fewer->links == Ids{ A });
	const auto none = reporter.Update(BENCH, {});  // the line taken back: told as none
	REQUIRE(none);
	CHECK(none->links.empty());
	CHECK(!reporter.Update(BENCH, {}));
	CHECK(reporter.Reports() == 4);
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

TEST("supply lines: a friend's game holds no more workbenches' lines than the server's table")
{
	SupplyLines::Heard heard;
	for (std::uint32_t i = 0; i < SupplyLines::Heard::MAX_BENCHES + 5; ++i) {
		heard.Take({ 0x0010'0000 + i, { A } });
	}
	CHECK(heard.Benches() == SupplyLines::Heard::MAX_BENCHES);
	CHECK(heard.With(0x0010'0000 + SupplyLines::Heard::MAX_BENCHES, Any).empty());  // past the table: left out
	heard.Take({ 0x0010'0000, { B } });  // one already held still changes
	CHECK(heard.With(0x0010'0000, Any) == Ids{ B });
	heard.Take({ 0x0010'0001, {} });  // and one taken back makes room
	heard.Take({ 0x0010'0000 + SupplyLines::Heard::MAX_BENCHES, { C } });
	CHECK(heard.With(0x0010'0000 + SupplyLines::Heard::MAX_BENCHES, Any) == Ids{ C });
}

namespace
{
	using Views = SupplyLines::Views;
	const Views::Clock::time_point T0 = Views::Clock::time_point{} + std::chrono::hours{ 1 };

	Ids Many(std::size_t a_count)
	{
		Ids out;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			out.push_back(0x0003'0000 + i);
		}
		return out;
	}
}

TEST("supply lines: a friend's game views the first few of many lines, one at a time, never more than the server sends")
{
	Views      views;
	const auto linked = Many(Views::MAX + 6);
	Ids        viewed;
	auto       now = T0;
	// As fast as frames come (every 10 ms): one opens per interval.
	for (int frame = 0; frame < 1000; ++frame, now += std::chrono::milliseconds{ 10 }) {
		const auto step = views.Next(linked, viewed, now);
		CHECK(step.close.empty());
		CHECK(step.wanted.size() == Views::MAX);
		if (step.open) {
			viewed.push_back(*step.open);
		}
	}
	CHECK((viewed == Ids{ linked.begin(), linked.begin() + Views::MAX }));  // the first in the keeper's order
	// The opens were spread: at most a second's worth in any second, well under the server's 8.
	Views      paced;
	Ids        seen;
	int        opened = 0;
	for (auto at = T0; at < T0 + std::chrono::seconds{ 1 }; at += std::chrono::milliseconds{ 1 }) {
		if (const auto step = paced.Next(linked, seen, at); step.open) {
			seen.push_back(*step.open);
			++opened;
		}
	}
	CHECK(opened == 4);
	CHECK(opened < 8);
}

TEST("supply lines: a view the server closed is tried again after a backoff that doubles, and starts over once shown")
{
	Views      views;
	const Ids  linked{ A };
	auto       step = views.Next(linked, {}, T0);
	REQUIRE(step.open == A);
	// Answered closed (the server's view budget spent): not tried for 5 s, then 10 s.
	views.Closed(A, T0 + std::chrono::milliseconds{ 100 });
	CHECK(!views.Next(linked, {}, T0 + std::chrono::seconds{ 4 }).open);
	CHECK(views.Next(linked, {}, T0 + std::chrono::seconds{ 11 }).open == A);
	const auto again = T0 + std::chrono::seconds{ 11 };
	views.Closed(A, again);
	CHECK(!views.Next(linked, {}, again + std::chrono::seconds{ 19 }).open);
	CHECK(views.Next(linked, {}, again + std::chrono::seconds{ 41 }).open == A);
	// Its contents came: a later close waits only the first interval.
	views.Shown(A);
	const auto later = again + std::chrono::minutes{ 5 };
	views.Closed(A, later);
	CHECK(!views.Next(linked, {}, later + std::chrono::seconds{ 4 }).open);
	CHECK(views.Next(linked, {}, later + std::chrono::seconds{ 6 }).open == A);
	// The backoff never passes a minute.
	auto at = later + std::chrono::seconds{ 6 };
	for (int i = 0; i < 10; ++i) {
		views.Closed(A, at);
		at += Views::RETRY_MAX + std::chrono::seconds{ 1 };
		REQUIRE(views.Next(linked, {}, at).open == A);
	}
}

TEST("supply lines: a line no longer linked (or past the few viewed) ends its view")
{
	Views views;
	auto  step = views.Next(Ids{ B, A }, Ids{ A, C }, T0);
	CHECK((step.close == Ids{ C }));
	CHECK(step.open == B);
	step = views.Next(Many(Views::MAX), Ids{ A }, T0 + std::chrono::seconds{ 1 });
	CHECK((step.close == Ids{ A }));
	views.Clear();
	CHECK(views.Next(Ids{ A }, {}, T0).open == A);  // a new view of the workbench starts afresh
}
