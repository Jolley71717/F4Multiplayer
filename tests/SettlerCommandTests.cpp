// Giving a settler a job in workshop mode: the settlers of the workbench our player is at are run by our game while
// the mode lasts (game/SettlerCommand.h).

#include "Test.h"

#include "game/SettlerCommand.h"

namespace
{
	using SettlerCommand::Step;
	constexpr std::uint32_t BENCH = 0x1F0B9;
	constexpr std::uint32_t US = 2;
	constexpr std::uint32_t FRIEND = 1;
}

TEST("settler command: a settler of the workbench we command at is taken over from a friend, and kept when ours")
{
	CHECK(SettlerCommand::Decide(BENCH, BENCH, FRIEND, US) == Step::kTakeOver);
	CHECK(SettlerCommand::Decide(BENCH, BENCH, US, US) == Step::kKeep);
}

TEST("settler command: nothing is claimed outside workshop mode, for another settlement, an NPC of none, or one nobody runs")
{
	CHECK(SettlerCommand::Decide(BENCH, 0, FRIEND, US) == Step::kNone);       // not in workshop mode
	CHECK(SettlerCommand::Decide(0x2000, BENCH, FRIEND, US) == Step::kNone);  // another settlement's
	CHECK(SettlerCommand::Decide(0, BENCH, FRIEND, US) == Step::kNone);       // no settler
	CHECK(SettlerCommand::Decide(0, 0, FRIEND, US) == Step::kNone);
	CHECK(SettlerCommand::Decide(BENCH, BENCH, 0, US) == Step::kNone);      // nobody runs it: offered as usual
	CHECK(SettlerCommand::Decide(BENCH, BENCH, FRIEND, 0) == Step::kNone);  // not welcomed
}

TEST("settler command: every settler we run is kept, and only a few are taken over a round")
{
	std::vector<SettlerCommand::Judged> settlers;
	for (std::uint32_t id = 1; id <= 10; ++id) {
		settlers.push_back({ id, id % 2 ? Step::kTakeOver : Step::kKeep });
	}
	settlers.push_back({ 99, Step::kNone });
	const auto round = SettlerCommand::Plan(settlers);
	CHECK((round.keep == std::vector<std::uint32_t>{ 2, 4, 6, 8, 10 }));
	REQUIRE(round.takeOver.size() == SettlerCommand::MAX_TAKEOVERS);
	CHECK((round.takeOver == std::vector<std::uint32_t>{ 1, 3, 5, 7 }));
	CHECK(SettlerCommand::Plan({}).keep.empty());
}
