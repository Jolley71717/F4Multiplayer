#include "Test.h"

#include "game/StageRules.h"

#include <chrono>
#include <iterator>
#include <tuple>

namespace
{
	using StageRules::File;
	using StageRules::Verdict;
	using namespace std::chrono_literals;

	const StageRules::Rule* Find(std::uint32_t a_quest, std::uint16_t a_stage, File a_file = File::kFallout4)
	{
		return StageRules::Find(a_file, a_quest, a_stage);
	}

	std::uint8_t FlagsOf(std::uint32_t a_quest, std::uint16_t a_stage, File a_file = File::kFallout4)
	{
		const auto rule = Find(a_quest, a_stage, a_file);
		return rule ? rule->flags : 0;
	}
}

TEST("stage rules: the table is sorted, once per stage")
{
	for (auto it = std::next(std::begin(StageRules::RULES)); it != std::end(StageRules::RULES); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } < std::tuple{ it->file, it->quest, it->stage }));
	}
	CHECK(StageRules::FileOf("Fallout4.esm") == File::kFallout4);
	CHECK(StageRules::FileOf("DLCCOAST.ESM") == File::kCoast);
	CHECK(!StageRules::FileOf("DLCRobot.esm"));
}

TEST("stage rules: what the fragments do")
{
	using namespace StageRules;
	CHECK(FlagsOf(0x0229E6, 177) == kPays);  // MQ105: the 250 caps bribe for Geneva
	CHECK(FlagsOf(0x1764DF, 40) == kPays);   // FFDiamondCity10: a Stimpak
	CHECK(FlagsOf(0x0229FD, 37) == kMoves);  // MS07b: MoveTo the dig site, faded out
	CHECK(FlagsOf(0x0B1752, 1000) == (kMoves | kBridge));  // MQ206: through the teleporter door, the only way into the Institute
	CHECK(FlagsOf(0x0B1752, 500) == (kMoves | kBridge));   // MQ206: controls off, and starts MQ207
	CHECK(FlagsOf(0x0B1752, 1050) == 0);                   // MQ206: Wait 2, then 1200
	CHECK(!Find(0x0B1752, 1200));                          // MQ206: controls back, the quest stops
	CHECK(FindLock(File::kFallout4, 0x0B1752, 500) && FindLock(File::kFallout4, 0x0B1752, 500)->releaseStage == 1200);
	CHECK(FindLock(File::kFallout4, 0x0B26BC, 1400));  // MQ206RR sets MQ206 500
	CHECK(!FindLock(File::kFallout4, 0x0B1752, 1000));
	CHECK(FlagsOf(0x05DEE4, 220) == kStory);   // Min01: Preston's offer, The First Step
	CHECK(FlagsOf(0x05DEE4, 2000) == kStory);  // Min01: the Minutemen
	CHECK(FlagsOf(0x05DEE4, 2100) == kStory);  // Min01: Stop(), which would end a Sanctuary left at 1410 by the skipped 1500
	CHECK(FlagsOf(0x05DEE4, 1900) == 0);       // Min01: Preston's greeting, nothing of the story
	CHECK(FlagsOf(0x108EC0, 100) == kStory);   // FFGoodneighbor01: MQ103 30
	CHECK(FlagsOf(0x01F25E, 210) == 0);        // MQ104 210: starts MQ105, nothing to hold back
	CHECK(!(FlagsOf(0x001B42, 10, File::kCoast) & kStory));  // Far Harbor's own story is left alone

	CHECK(Hold(nullptr) == StageRules::Clock::duration::zero());
	const auto door = Find(0x0B1752, 1000);
	CHECK(door && Hold(door) == 5s);  // Wait 4, and a second more
}

TEST("stage rules: a friend's stage is set, skipped, or waits")
{
	using namespace StageRules;
	const Rule plain{ 0, 0x1, 10, 0, 0 };
	const Rule pays{ 0, 0x1, 10, kPays, 0 };
	const Rule story{ 0, 0x1, 10, kStory, 0 };
	const Rule moves{ 0, 0x1, 10, kMoves, 0 };
	const Rule bridge{ 0, 0x1, 10, kMoves | kBridge, 0 };

	// Most stages: set; caught-up ones wait while the player fights, live ones don't.
	CHECK(Judge(nullptr, { .live = false }) == Verdict::kSet);
	CHECK(Judge(&plain, { .live = true, .held = true }) == Verdict::kSet);
	CHECK(Judge(&plain, { .live = false, .held = true }) == Verdict::kLater);

	// Never a friend's payment.
	CHECK(Judge(&pays, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);

	// A side quest's stage that moves the story on: only with a shared story.
	CHECK(Judge(&story, { .sharedStory = false, .live = true }) == Verdict::kSkip);
	CHECK(Judge(&story, { .sharedStory = true, .live = true }) == Verdict::kSet);

	// A stage that moves the player: along with the friend who did it, else not at all.
	CHECK(Judge(&moves, { .live = true, .nearFriend = true }) == Verdict::kSet);
	CHECK(Judge(&moves, { .live = true, .nearFriend = false }) == Verdict::kSkip);
	CHECK(Judge(&moves, { .live = false, .nearFriend = true }) == Verdict::kSkip);

	// ... unless nothing else does what it does: then after a warning, and not in a fight.
	CHECK(Judge(&bridge, { .live = false }) == Verdict::kWarn);
	CHECK(Judge(&bridge, { .live = false, .warned = 5s }) == Verdict::kLater);
	CHECK(Judge(&bridge, { .live = false, .warned = WARNING }) == Verdict::kSet);
	CHECK(Judge(&bridge, { .live = false, .held = true, .warned = WARNING }) == Verdict::kLater);
	CHECK(Judge(&bridge, { .live = true, .nearFriend = true }) == Verdict::kSet);

	// Controls taken away only once the stage that gives them back is coming; until then only its
	// own quest waits (a release that never comes mustn't hold up every other quest), whatever else
	// is the case.
	CHECK(Judge(&bridge, { .live = true, .nearFriend = true, .releaseComing = false }) == Verdict::kQuestWaits);
	CHECK(Judge(&bridge, { .live = false, .held = true, .releaseComing = false, .warned = WARNING }) == Verdict::kQuestWaits);
	CHECK(Judge(&bridge, { .live = false, .releaseComing = true, .warned = WARNING }) == Verdict::kSet);
	// The stages after a lock stage go on as warned, and in a fight (the player can't fight anyway).
	CHECK(Judge(&bridge, { .live = false, .held = false, .warned = WARNING }) == Verdict::kSet);
}

TEST("stage rules: starting a stuck main story's next quest (MQ105, whose start moves the player) warns first and waits out a fight")
{
	// QuestSync::RepairStory judges MQ105's stage 0 as a stage caught up from the session.
	const auto rule = Find(0x0229E6, 0);
	REQUIRE(rule);
	StageRules::Situation now;
	now.sharedStory = true;
	CHECK(StageRules::Judge(rule, now) == Verdict::kWarn);
	now.warned = 4s;
	CHECK(StageRules::Judge(rule, now) == Verdict::kLater);
	now.warned = StageRules::WARNING;
	now.held = true;  // the player is fighting
	CHECK(StageRules::Judge(rule, now) == Verdict::kLater);
	now.held = false;
	CHECK(StageRules::Judge(rule, now) == Verdict::kSet);
}
