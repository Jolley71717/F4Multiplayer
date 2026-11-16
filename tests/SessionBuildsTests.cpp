// The build each robot has in the session (common/SessionBuilds.h): the first runner's, kept through hand-offs, replaced
// only by its keeper, its companion owner or a change seen where it runs.

#include "Test.h"

#include "SessionBuilds.h"

namespace
{
	using SessionBuilds::Verdict;

	constexpr std::uint32_t ADA = 0x0100FF12;
	constexpr std::uint32_t HOST = 1;
	constexpr std::uint32_t FRIEND = 2;
	const std::vector<Protocol::ItemMod> SAWS{ { 0x0100A102, 0, 0 }, { 0x0100A201, 0, 0 } };
	const std::vector<Protocol::ItemMod> STOCK{ { 0x0100A101, 0, 0 }, { 0x0100A201, 0, 0 } };

	Protocol::NpcEquipment Report(const std::vector<Protocol::ItemMod>& a_mods, bool a_changed = false)
	{
		Protocol::NpcEquipment report{ 0, ADA, {}, true, a_mods, 0 };
		report.modsChanged = a_changed;
		return report;
	}
}

TEST("session builds: the same robot is the same mods at the same attach indexes, in any order")
{
	CHECK(SessionBuilds::Same(SAWS, std::vector<Protocol::ItemMod>{ SAWS[1], SAWS[0] }));
	CHECK(!SessionBuilds::Same(SAWS, STOCK));
	CHECK(!SessionBuilds::Same(std::vector<Protocol::ItemMod>{ { 0x0100A401, 1, 0 } }, std::vector<Protocol::ItemMod>{ { 0x0100A401, 2, 0 } }));
	CHECK(SessionBuilds::Same(std::vector<Protocol::ItemMod>{ { 0x0100A401, 1, 0 }, { 0x0100A401, 1, 0 } }, std::vector<Protocol::ItemMod>{ { 0x0100A401, 1, 0 } }));
}

TEST("session builds: a hand-off never puts the new runner's unfinished robot into the first runner's game")
{
	SessionBuilds::Book book;
	auto                host = Report(SAWS);
	CHECK(book.Judge(HOST, false, host) == Verdict::kKept);
	// The friend's game takes Ada over with its copy still stock (it was fighting): the host's saws go on, not the stock.
	auto handedOff = Report(STOCK);
	CHECK(book.Judge(FRIEND, false, handedOff) == Verdict::kRefused);
	CHECK(handedOff.modsRead);
	CHECK(SessionBuilds::Same(handedOff.actorMods, SAWS));
	// Its game says nothing of a copy half way through the swap: the session's go on all the same.
	Protocol::NpcEquipment unsaid{ 0, ADA, {}, false, {}, 0 };
	CHECK(book.Judge(FRIEND, false, unsaid) == Verdict::kFilled);
	CHECK(unsaid.modsRead);
	CHECK(SessionBuilds::Same(unsaid.actorMods, SAWS));
	// Back in the host's game: the same build.
	auto back = Report(SAWS);
	CHECK(book.Judge(HOST, false, back) == Verdict::kSame);
	REQUIRE(book.Find(ADA));
	CHECK(book.Find(ADA)->playerId == HOST);
	// A newcomer is told the session's, whatever the runner said last.
	Protocol::NpcEquipment told{ FRIEND, ADA, {}, true, STOCK, 0 };
	book.Tell(told);
	CHECK(SessionBuilds::Same(told.actorMods, SAWS));
}

TEST("session builds: replaced by its keeper, its companion owner, or a change made where it runs")
{
	SessionBuilds::Book book;
	auto first = Report(SAWS);
	book.Judge(HOST, false, first);
	// The keeper's game changes it (the host at the robot workbench, or a load of an older save).
	auto again = Report(STOCK);
	CHECK(book.Judge(HOST, false, again) == Verdict::kReplaced);
	CHECK(SessionBuilds::Same(book.Find(ADA)->mods, STOCK));
	// The friend at the workbench while their game runs it: changed there, so it is the session's, and theirs to change.
	auto edited = Report(SAWS, true);
	CHECK(book.Judge(FRIEND, false, edited) == Verdict::kReplaced);
	CHECK(edited.modsChanged);
	CHECK(book.Find(ADA)->playerId == FRIEND);
	// The host's game, not its keeper now, runs it with another build and no change of its own: the friend's stands.
	auto stale = Report(STOCK);
	CHECK(book.Judge(HOST, false, stale) == Verdict::kRefused);
	CHECK(SessionBuilds::Same(stale.actorMods, SAWS));
	CHECK(!stale.modsChanged);
	// Unless Ada is the host's companion: their robot, as their player built it.
	auto companion = Report(STOCK);
	CHECK(book.Judge(HOST, true, companion) == Verdict::kReplaced);
	CHECK(book.Find(ADA)->playerId == HOST);
}

TEST("session builds: an NPC with no parts of its own is passed on as it came, and a death forgets the build")
{
	SessionBuilds::Book book;
	Protocol::NpcEquipment raider{ 0, ADA, {}, true, {}, 0 };
	CHECK(book.Judge(HOST, false, raider) == Verdict::kNone);
	Protocol::NpcEquipment unsaid{ 0, ADA, {}, false, {}, 0 };
	CHECK(book.Judge(HOST, false, unsaid) == Verdict::kNone);
	CHECK(!unsaid.modsRead);
	CHECK(book.Size() == 0u);
	auto built = Report(SAWS);
	book.Judge(HOST, false, built);
	CHECK(book.Size() == 1u);
	book.Forget(ADA);
	CHECK(book.Find(ADA) == nullptr);
	auto next = Report(STOCK);
	CHECK(book.Judge(FRIEND, false, next) == Verdict::kKept);
}
