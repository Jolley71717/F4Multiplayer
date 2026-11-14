#include "Test.h"

#include "game/QuestFilter.h"
#include "game/SharedRoll.h"
#include "game/StageRules.h"

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
	CHECK(StageRules::FileOf("DLCRobot.esm") == File::kRobot);
	CHECK(StageRules::FileOf("dlcnukaworld.esm") == File::kNukaWorld);
	CHECK(!StageRules::FileOf("DLCworkshop01.esm"));  // the workshop add-ons have no shared quest stages
	CHECK(!StageRules::FileOf("Unofficial Fallout 4 Patch.esp"));
}

TEST("stage rules: Automatron's and Nuka-World's stages that move the player or take from them (tools/questscan.ps1 -Rules)")
{
	using namespace StageRules;
	// DLC04MQ01 1000 sets DLC04MQ01Gauntlet 2500, which walks the player through the Nuka-World station's stairwell door.
	CHECK(FlagsOf(0x000801, 1000, File::kNukaWorld) == kMoves);
	// DLC04MS02 500 unequips the space suits and helmets and takes them; the raids' 90 take the settlement's caps.
	CHECK(FlagsOf(0x000807, 500, File::kNukaWorld) == kPays);
	CHECK(FlagsOf(0x014352, 90, File::kNukaWorld) == kPays);  // DLC04RaidChaseOff
	CHECK(FlagsOf(0x019088, 90, File::kNukaWorld) == kPays);  // DLC04RaidCoerce
	// Automatron's quickstarts (MoveTo Codsworth's marker, Fort Hagen's start): DLC01MQ04's, which set its quests' stages,
	// are never shared (kDebug). DLC01MQ05 has its 1 and 2 in its fragment script too, but its record binds those functions to
	// no stage (tools/questscan.cs takes a stage's code from the VMAD's fragment table): setting them runs nothing, no row.
	CHECK(FlagsOf(0x002833, 2, File::kRobot) == (kMoves | kDebug));  // DLC01MQ04
	CHECK(!Find(0x0010F5, 1, File::kRobot));                          // DLC01MQ05
	CHECK(!Find(0x0010F5, 2, File::kRobot));
	// Quest items, read from an alias, are left alone: the robot's head (DLC01MQ04 200), Dry Rock Gulch's drink (218).
	CHECK(!Find(0x002833, 200, File::kRobot));
	CHECK(!Find(0x0431AB, 218, File::kNukaWorld));
	// The same form IDs in another file are another quest's.
	CHECK(!Find(0x000801, 1000, File::kRobot));
	CHECK(!(FlagsOf(0x000807, 500, File::kFallout4) & kPays));

	// Never set from a friend's game: a payment at all, a move unless live beside the friend who did it.
	for (const auto& [quest, stage] : { std::pair{ 0x000807u, 500 }, std::pair{ 0x014352u, 90 }, std::pair{ 0x019088u, 90 } }) {
		CHECK(Judge(Find(quest, static_cast<std::uint16_t>(stage), File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	}
	const auto door = Find(0x000801, 1000, File::kNukaWorld);
	CHECK(Judge(door, { .live = true, .nearFriend = false }) == Verdict::kSkip);  // a friend far away
	CHECK(Judge(door, { .live = false }) == Verdict::kSkip);                      // caught up from the session
	CHECK(Judge(door, { .live = true, .nearFriend = true }) == Verdict::kSet);    // through the door with them

	// Their stage 0s that move the player are the developers' quickstarts, none its quest's start-up stage, so a friend's
	// stage that starts the quest here isn't judged by one (StageRules::Starting): DLC04MQ01's ("[QS] Nuka-Station") sets
	// shared stages and is a kDebug row, which StartRule never gives; the others are left out.
	CHECK(IsDebug(File::kNukaWorld, 0x000801, 0));  // DLC04MQ01
	CHECK(!StartRule(File::kNukaWorld, 0x000801, 0));
	CHECK(!Find(0x000807, 0, File::kNukaWorld));  // DLC04MS02
	CHECK(!Find(0x000806, 0, File::kRobot));      // DLC01MQ01
	CHECK(!StageRules::Starting(StartRule(File::kRobot, 0x000806, 0), Find(0x000806, 100, File::kRobot)));
	CHECK(StageRules::Starting(StartRule(File::kNukaWorld, 0x000801, 0), door)->flags == kMoves);

	// Theirs that set another shared quest's stage (-SetsOther): DLC04MQ00's 520 starts DLC04MQ01, DLC01MQ01's 3000 DLC01MQ02.
	CHECK(StageRules::SetsAnother(File::kNukaWorld, 0x000800, 520));
	CHECK(StageRules::SetsAnother(File::kNukaWorld, 0x000801, 1000));  // DLC04MQ02 1
	CHECK(StageRules::SetsAnother(File::kRobot, 0x000806, 3000));
	CHECK(!StageRules::SetsAnother(File::kRobot, 0x000800, 520));
}

TEST("stage rules: a stage's code is the fragment its quest record binds to it, five-digit stages too (tools/questscan.cs, the VMAD's fragment table)")
{
	using namespace StageRules;
	// The scan once took a stage from its fragment function's name, four digits of it: RR302's Fragment_Stage_10700 and
	// 10900 read as 1070 and 1090 (stages RR302 doesn't have), RR303's 10200 as its 1020.
	CHECK(SetsAnother(File::kFallout4, 0x02C8CB, 10700));  // RR302
	CHECK(SetsAnother(File::kFallout4, 0x02C8CB, 10900));
	CHECK(!SetsAnother(File::kFallout4, 0x02C8CB, 1070));
	CHECK(!SetsAnother(File::kFallout4, 0x02C8CB, 1090));
	CHECK(SetsAnother(File::kFallout4, 0x043275, 10200));  // RR303 10200 sets RR201 1100
	// RR303's 1020 turns the Brotherhood on the player (by its 1000); its 10200 the Brotherhood's attackers (by its 100),
	// and so does Rockets' Red Glare's 200, which sets it.
	const auto rr303 = HostileOf(File::kFallout4, 0x043275, 1020);
	REQUIRE(rr303.size() == 1);
	CHECK(rr303[0].faction == 0x05DE41);
	const auto attackers = HostileOf(File::kFallout4, 0x043275, 10200);
	REQUIRE(attackers.size() == 1);
	CHECK(attackers[0].faction == 0x19E9A6);
	REQUIRE(HostileOf(File::kFallout4, 0x142FD3, 200).size() == 1);
	CHECK(HostileOf(File::kFallout4, 0x142FD3, 200)[0].faction == 0x19E9A6);
	// A function of the fragment script that the record binds to no stage never runs: DLC01MQ05's 1 and 2 (above), V81_03's
	// 1, 200 and 300.
	CHECK(!SetsAnother(File::kRobot, 0x0010F5, 1));
	CHECK(!SetsAnother(File::kRobot, 0x0010F5, 2));
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

TEST("stage rules: a dialogue quest's payment or sale (shared with the story) is never made from a friend's game")
{
	using namespace StageRules;
	// Shared since the dialogue quests are (QuestFilter::IsDialogue), scanned from their fragments (tools/questscan.ps1).
	CHECK(FlagsOf(0x003648, 70) == kPays);   // DialogueDiamondCity: the World Series bat sold for caps
	CHECK(FlagsOf(0x019955, 210) == kPays);  // DialogueBunkerHill: 10 caps
	CHECK(FlagsOf(0x03F244, 5) == kPays);    // DialogueVault81: 5 caps
	CHECK(FlagsOf(0x03F244, 645) == kPays);  // DialogueVault81: 25 fertilizer sold
	CHECK(FlagsOf(0x005BD7, 90, File::kCoast) == kPays);  // DLC03DialogueFarHarbor: the charge card sold
	// A friend's sale or payment there: skipped, live and beside them too.
	for (const auto& [quest, stage, file] : { std::tuple{ 0x003648u, 70, File::kFallout4 }, std::tuple{ 0x019955u, 210, File::kFallout4 },
			 std::tuple{ 0x03F244u, 5, File::kFallout4 }, std::tuple{ 0x03F244u, 645, File::kFallout4 }, std::tuple{ 0x005BD7u, 90, File::kCoast } }) {
		CHECK(Judge(Find(quest, static_cast<std::uint16_t>(stage), file), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	}
	// The scenes around them are not held back.
	CHECK(!Find(0x03F244, 642));  // DialogueVault81: Priscilla hands the player a mutfruit
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

TEST("stage rules: a friend far away when their stage moves them is offered to go with them, and it is set once beside them; not taken up, it goes as before")
{
	using namespace StageRules;
	const Rule plain{ 0, 0x1, 10, 0, 0 };
	const Rule moves{ 0, 0x1, 10, kMoves, 0 };
	const Rule bridge{ 0, 0x1, 10, kMoves | kBridge, 0 };
	const Rule fight{ 0, 0x1, 10, kMoves | kHostile, 0 };
	const Rule paysToo{ 0, 0x1, 10, kMoves | kPays, 0 };

	// Live, the friend who did it far and known here: offered (the HUD line, the teleport key aimed at them).
	CHECK(Judge(&moves, { .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(Judge(&fight, { .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(Judge(&moves, { .live = true, .held = true, .followable = true }) == Verdict::kOffer);  // fighting: offered all the same
	// Nobody to follow (a stage the server named no player for): as before, skipped.
	CHECK(Judge(&moves, { .live = true }) == Verdict::kSkip);
	// Beside them already: set at once, no offer.
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .followable = true }) == Verdict::kSet);

	// Offered: it waits, past the live window too, until OFFER_FOR has gone by.
	CHECK(OFFER_FOR > LIVE_FOR);
	CHECK(Judge(&moves, { .live = true, .followable = true, .offered = 1s }) == Verdict::kOffer);
	CHECK(Judge(&moves, { .live = false, .followable = true, .offered = 60s }) == Verdict::kOffer);
	CHECK(Judge(&moves, { .live = false, .followable = true, .offered = OFFER_FOR - 1s }) == Verdict::kOffer);
	// ... and is set the moment the player is beside them (the teleport key's arrival, or on foot).
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .followable = true, .offered = 5s }) == Verdict::kSet);
	CHECK(Judge(&moves, { .live = false, .nearFriend = true, .followable = true, .offered = 60s }) == Verdict::kSet);
	CHECK(Judge(&moves, { .live = false, .nearFriend = true, .held = true, .followable = true, .offered = 60s }) == Verdict::kSet);

	// Not taken up: skipped, as a far friend's stage was before; a bridge warned and set as ever.
	CHECK(Judge(&moves, { .live = false, .followable = true, .offered = OFFER_FOR }) == Verdict::kSkip);
	CHECK(Judge(&moves, { .live = false, .nearFriend = true, .followable = true, .offered = OFFER_FOR }) == Verdict::kSkip);
	CHECK(Judge(&bridge, { .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(Judge(&bridge, { .live = false, .followable = true, .offered = OFFER_FOR }) == Verdict::kWarn);
	CHECK(Judge(&bridge, { .live = false, .warned = 5s, .followable = true, .offered = OFFER_FOR + 5s }) == Verdict::kLater);
	CHECK(Judge(&bridge, { .live = false, .warned = WARNING, .followable = true, .offered = OFFER_FOR + 10s }) == Verdict::kSet);
	// The friend gone from the session meanwhile: nobody to follow, as before.
	CHECK(Judge(&moves, { .live = false, .followable = false, .offered = 30s }) == Verdict::kSkip);

	// Never for a stage caught up from the session (a joiner, a load): skipped, or a bridge warned.
	CHECK(Judge(&moves, { .live = false, .followable = true }) == Verdict::kSkip);
	CHECK(Judge(&bridge, { .live = false, .followable = true }) == Verdict::kWarn);
	// Nor for one that doesn't move the player, a payment, a side quest's step of an unshared story, the opening, or a
	// lock whose release isn't coming.
	CHECK(Judge(&plain, { .live = true, .followable = true }) == Verdict::kSet);
	CHECK(Judge(&paysToo, { .live = true, .followable = true }) == Verdict::kSkip);
	CHECK(Judge(&moves, { .live = true, .opening = true, .followable = true }) == Verdict::kOpeningWaits);
	CHECK(Judge(&bridge, { .live = true, .releaseComing = false, .followable = true }) == Verdict::kQuestWaits);
	CHECK(Judge(&bridge, { .live = false, .releaseComing = false, .followable = true, .offered = 10s }) == Verdict::kQuestWaits);

	// The rows: Kellogg's memory (MQ203 10) and DN070's cave (5) are offered; Kiddie Kingdom's quickstart 6 is a move
	// never shared (kDebug: it sets the park's stages), and its 740 (Oswald's fight) no RULES row at all: a faction turned
	// on the player is HOSTILE's (tests/HostileHoldTests.cpp).
	CHECK(Judge(Find(0x0229EA, 10), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(FlagsOf(0x0E9914, 5) == kMoves);  // DN070: MoveTo DN070_TreasuresEntry (the rig's step_follow; a quickstart left unmarked for it)
	CHECK(Judge(Find(0x0E9914, 5), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(FlagsOf(0x03684F, 6, File::kNukaWorld) == (kMoves | kDebug));
	CHECK(Judge(Find(0x03684F, 6, File::kNukaWorld), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kSkip);
	CHECK(!Find(0x03684F, 740, File::kNukaWorld));
	CHECK(!Find(0x08FCED, 500));  // DN036 500, Phyllis: the rig's step_follow lands it far from the host, none of her faction near
	CHECK(!HostileOf(File::kFallout4, 0x08FCED, 500).empty());
}

TEST("stage rules: starting a stuck main story's next quest at MQ105 10 (which moves nobody) needs no warning but waits out a fight; MQ105 0 is a console quickstart")
{
	// QuestSync::RepairStory sets MQ105 10, as MQ104 210 does, judged by 10's own rule as a stage caught up from the session:
	// MQ105 has no start-up stage, so 10 runs nothing else.
	const auto rule = Find(0x0229E6, 10);
	CHECK(!rule);
	StageRules::Situation now;
	now.sharedStory = true;
	CHECK(StageRules::Judge(rule, now) == Verdict::kSet);
	now.held = true;  // the player is fighting
	CHECK(StageRules::Judge(rule, now) == Verdict::kLater);
	// Its 0 ("TESTING ONLY": MoveTo MQ105PlayerStartMarker, and the story set up to there) only a console sets: never shared,
	// nor the start a friend's 10 is judged by (what it sets: the test of QUICKSTART_SETS for it, below).
	CHECK(FlagsOf(0x0229E6, 0) == (StageRules::kMoves | StageRules::kDebug));
	CHECK(StageRules::IsDebug(File::kFallout4, 0x0229E6, 0));
	CHECK(!StageRules::StartRule(File::kFallout4, 0x0229E6, 0));
	CHECK(StageRules::Judge(Find(0x0229E6, 0), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(StageRules::Judge(Find(0x0229E6, 0), { .sharedStory = true }) == Verdict::kSkip);
}

TEST("stage rules: a friend's stage that starts its quest here is judged and held for the quest's start-up stage too")
{
	using StageRules::Starting;
	CHECK(!Starting(nullptr, nullptr));
	// A stage merged with a start-up stage that moves the player (MQ201's 0, as a stage that started it would be judged if
	// its 0 were one) moves them too.
	const auto clue = Starting(Find(0x0229E8, 0), Find(0x0229E8, 10));
	REQUIRE(clue);
	CHECK(clue->flags == (StageRules::kMoves | StageRules::kBridge));
	CHECK(StageRules::Hold(&*clue) == StageRules::Hold(Find(0x0229E8, 0)));
	StageRules::Situation now;
	now.sharedStory = true;
	CHECK(StageRules::Judge(&*clue, now) == Verdict::kWarn);
	// MS01's 1050 waits 5 s; with a start-up 0 that moves the player (no bridge) and waits 2 s: from the session it isn't
	// set, but it is with the friend who just did it.
	const auto ms01 = Starting(StageRules::StartRule(StageRules::File::kFallout4, 0x0229F6, 0), Find(0x0229F6, 1050));
	REQUIRE(ms01);
	CHECK(ms01->flags == StageRules::kMoves);
	CHECK(ms01->wait == 50);
	CHECK(StageRules::Judge(&*ms01, {}) == Verdict::kSkip);
	CHECK(StageRules::Judge(&*ms01, { .live = true, .nearFriend = true }) == Verdict::kSet);
	// MS07a's 0 moved the player too, and was judged so with its 17 once; it is a developer's quickstart ("DEBUG -
	// Companion Interjection Quickstart", no start-up stage), which starting the quest never runs: its 17 is judged alone.
	CHECK(StageRules::IsDebug(StageRules::File::kFallout4, 0x01CB51, 0));
	CHECK(!StageRules::StartRule(StageRules::File::kFallout4, 0x01CB51, 0));
	const auto ms07a = Starting(StageRules::StartRule(StageRules::File::kFallout4, 0x01CB51, 0), Find(0x01CB51, 17));
	REQUIRE(ms07a);
	CHECK(ms07a->flags == 0);
	CHECK(StageRules::Judge(&*ms07a, {}) == Verdict::kSet);
	// A quest whose start-up stage does nothing: the stage's own rule.
	const auto vault = Starting(nullptr, Find(0x0B8464, 400));
	REQUIRE(vault);
	CHECK(StageRules::Judge(&*vault, { .live = true }) == Verdict::kSkip);  // Vault 81's 400 takes the player's fusion cores
	CHECK((FlagsOf(0x0B8464, 400) & StageRules::kPays) != 0);
}

TEST("stage rules: a developer's quickstart stage 0 (kDebug) is never set from a friend's game, nor judged as its quest's start")
{
	using namespace StageRules;
	const Rule debug{ 0, 0x1, 0, kDebug, 0 };
	// Never, live or caught up, with the friend near or a warning given.
	CHECK(Judge(&debug, {}) == Verdict::kSkip);
	CHECK(Judge(&debug, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(Judge(&debug, { .sharedStory = true, .live = true, .warned = WARNING }) == Verdict::kSkip);
	// A quest whose 0 is kDebug: the game doesn't run that 0 as the quest starts, so another stage starting it is judged
	// by its own rule alone, and one with no rule is set.
	const Rule waits{ 0, 0x1, 50, 0, 10 };
	const auto alone = Starting(&debug, &waits);
	REQUIRE(alone);
	CHECK(alone->flags == 0);
	CHECK(alone->wait == 10);
	CHECK(!Starting(&debug, nullptr));
	// ... and the 0 itself, starting it, is still skipped.
	const auto itself = Starting(&debug, &debug);
	REQUIRE(itself);
	CHECK(Judge(&*itself, { .live = true }) == Verdict::kSkip);

	// The rows from SafeScan.QuickStart (tools/questscan.ps1 -Safe -Rules): the SAFE and SILENT quests whose 0 moves the
	// player to a marker, is no start-up stage, and is set by nothing in the game's files. Each is a 0, and the only flag.
	// (The scan's DEBUG list, by hand, marks the other developers' quickstarts, of any stage: the tests below.)
	for (const auto& [quest, file] : { std::pair{ 0x0229FCu, 0 }, std::pair{ 0x145722u, 0 }, std::pair{ 0x0254B7u, 0 }, std::pair{ 0x16854Fu, 0 } }) {
		const auto rule = Find(static_cast<File>(file), quest, 0);
		REQUIRE(rule);
		CHECK(rule->flags == kDebug);
		CHECK(rule->wait == 0);
		CHECK((QuestFilter::IsSafe(rule->file, rule->quest) || QuestFilter::IsSilent(rule->file, rule->quest)));
	}
	CHECK(FlagsOf(0x0229FC, 0) == kDebug);  // MS07, Detective Case Files: MoveTo TestMQ201PlayerMarker (its 0 changes companions)
	CHECK(FlagsOf(0x145722, 0) == kDebug);  // BoSM04Postquest: MoveTo BoS301IngramGantryMarker (its 0 sets the repeatable DN053)
	CHECK(FlagsOf(0x0254B7, 0) == kDebug);  // DN154: MoveTo DN154_QuickstartMarker
	CHECK(FlagsOf(0x16854F, 0) == kDebug);  // MS05BTapes (SILENT): MoveTo pPlayerTestMarker
	// MS07's later stages start it with their own rules: its 50 waits 1 s, judged alone (no move).
	const auto ms07 = Starting(Find(0x0229FC, 0), Find(0x0229FC, 50));
	REQUIRE(ms07);
	CHECK(ms07->flags == 0);
	CHECK(Judge(&*ms07, {}) == Verdict::kSet);
	// BoSM04Postquest's 2 moves the player to the gantry: only with the friend who did it.
	CHECK(FlagsOf(0x145722, 2) == kMoves);
	CHECK(Judge(Find(0x145722, 2), { .live = true, .nearFriend = true }) == Verdict::kSet);
	CHECK(Judge(Find(0x145722, 2), {}) == Verdict::kSkip);
}

TEST("stage rules: a stage that moves the player through another quest it sets (Dangerous Minds' lounger into Kellogg's memory) is not caught up")
{
	using namespace StageRules;
	// tools/questscan.ps1 -Moves: MQ202 300 sets MQ203 10, whose MQ203Script.StartQuest fades out and moves the player
	// into the memory; MQ203 1000's FinishQuest moves them back to the lounger. A joiner catching up a friend's story
	// from the session was sent into the memory (rig, 2026-11-01).
	CHECK(FlagsOf(0x0229E9, 160) == kMoves);  // MQ202: the surgery, controls taken (was already so)
	CHECK(FlagsOf(0x0229E9, 300) == kMoves);
	CHECK(FlagsOf(0x0229EA, 10) == kMoves);
	CHECK(FlagsOf(0x0229EA, 1000) == kMoves);
	for (const auto& [quest, stage] : { std::pair{ 0x0229E9u, 300 }, std::pair{ 0x0229EAu, 10 }, std::pair{ 0x0229EAu, 1000 } }) {
		const auto rule = Find(quest, static_cast<std::uint16_t>(stage));
		REQUIRE(rule);
		CHECK(Judge(rule, { .sharedStory = true }) == Verdict::kSkip);                                  // caught up from the session
		CHECK(Judge(rule, { .sharedStory = true, .live = true }) == Verdict::kSkip);                    // live, the friend far away
		CHECK(Judge(rule, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSet);  // with the friend at the lounger
		CHECK(Judge(rule, { .sharedStory = true, .opening = true }) == Verdict::kOpeningWaits);
	}
	// The rest of the memory quest and the story after it are set: MQ203 20 (which starts MQ203 here once 10 was
	// skipped) and MQ204's first stage are judged alone. MQ203's 0 and MQ204's 0 are debug stages, quickstarts that set
	// the story's stages (kDebug), which StartRule never gives as their quest's start, so The Glowing Sea is caught up.
	CHECK(IsDebug(File::kFallout4, 0x0229EA, 0));
	CHECK(IsDebug(File::kFallout4, 0x06B500, 0));
	CHECK(!StartRule(File::kFallout4, 0x0229EA, 0));
	CHECK(!StartRule(File::kFallout4, 0x06B500, 0));
	CHECK(!Starting(StartRule(File::kFallout4, 0x0229EA, 0), Find(0x0229EA, 20)));
	CHECK(!Starting(StartRule(File::kFallout4, 0x06B500, 0), Find(0x06B500, 10)));
	CHECK(Judge(nullptr, { .sharedStory = true }) == Verdict::kSet);
	// The lounger's script waits 10 s (MQ203Script.StartQuest): set beside the friend, what comes after waits it out.
	CHECK(Hold(Find(0x0229E9, 300)) == 11s);

	// The other memory and teleport sequences the scan found behind a script or another quest's stage.
	CHECK(FlagsOf(0x108EC0, 40) == kMoves);               // FFGoodneighbor01: the Memory Den's memory (FFGoodneighbor01MemorySequence 10)
	CHECK(FlagsOf(0x01CBFA, 1, File::kCoast) == kMoves);   // DLC03MQ04_01: into DiMA's memories
	CHECK(FlagsOf(0x01E3FE, 350, File::kCoast) == kMoves); // DLC03MQ04_05: out of them
	CHECK(FlagsOf(0x036763, 548, File::kCoast) == kMoves); // DLC03_V118_Quest: the date
	CHECK(FlagsOf(0x0229F7, 17) == kMoves);                // MS02: controls frozen
	CHECK(FlagsOf(0x022A0A, 60) == kMoves);                // MS19: controls frozen
	// MQ101TVStation: would start MQ101, the pre-war opening, again, whose start-up stage also empties the inventory
	CHECK(FlagsOf(0x0BA4F2, 150) == (kMoves | kPays));
	CHECK(FlagsOf(0x0C3309, 500) == (kMoves | kPays));     // MQ101SanctuaryHills: the same
	// A branch on the argument is followed one way: MQ302 800 calls ControlToggle(1), which gives the controls back.
	CHECK(FlagsOf(0x0229EE, 800) == 0);
}

TEST("stage rules: the opening's stages (Vault 111) are every player's own, never shared")
{
	using StageRules::IsOpening;
	// MQ102 until the player first steps out of the vault (its 10, set by the trigger at the top of the lift).
	for (std::uint16_t stage = 0; stage < 10; ++stage) {
		CHECK(IsOpening(File::kFallout4, 0x01CC2A, stage));
	}
	CHECK(!IsOpening(File::kFallout4, 0x01CC2A, 10));
	CHECK(!IsOpening(File::kFallout4, 0x01CC2A, 65));  // MQ102 goes on outside, with Codsworth and Concord
	// The vault's dialogue quest, all of it: the cryopod (40), the vault door (95), the mirror (98), the lift (100),
	// outside (110). Its stages aren't in order, so each game has a different set; a friend's would be set here.
	for (const std::uint16_t stage : { 40, 55, 56, 57, 58, 60, 65, 70, 72, 74, 95, 97, 98, 100, 110 }) {
		CHECK(IsOpening(File::kFallout4, 0x01ED62, stage));
	}
	// Only Fallout4.esm's: Far Harbor's form IDs are another file's.
	CHECK(!IsOpening(File::kCoast, 0x01CC2A, 0));
	CHECK(!IsOpening(File::kCoast, 0x01ED62, 40));
	CHECK(!IsOpening(File::kFallout4, 0x01F25E, 0));  // MQ104
}

TEST("stage rules: a start-up stage that is a developer's quickstart is not what starting its quest runs: a friend's real stage of that quest is judged alone")
{
	using namespace StageRules;
	// The quests whose 0 is a quickstart ("Debug Start-Up" and the like: play starts them at a later stage, which never
	// runs their 0): the airport battle, BoS303, MQ302, Min301, MinDestBoS, MQ206, and the other quickstarts' quests:
	// MS07a, BoSM02, BoS201B, BoS302, BoS304, MQ203, InstM02, Nuka-World's DLC04MQ01 among them.
	for (const std::uint32_t quest : { 0x174029u, 0x0FDC8Cu, 0x0229EEu, 0x0DDFDBu, 0x0DFB3Cu, 0x0B1752u, 0x01CB51u, 0x04402Cu, 0x0537FCu, 0x0B9F9Du, 0x173ED9u, 0x0229EAu, 0x0A8257u }) {
		CHECK(IsDebug(File::kFallout4, quest, 0));
		CHECK(!StartRule(File::kFallout4, quest, 0));
	}
	CHECK(!StartRule(File::kNukaWorld, 0x000801, 0));
	// Any other stage's row is the start rule for it; a quest with no such row has none. (Whether a stage is a start-up
	// stage at all is the quest record's: QuestStageLayout::StartUps, see the test below.)
	CHECK(!StartRule(File::kFallout4, 0x0229E6, 0));                       // MQ105: its 0 is "TESTING ONLY", a quickstart, and no start-up stage
	CHECK(StartRule(File::kFallout4, 0x0229F6, 0) == Find(0x0229F6, 0));  // MS01
	CHECK(StartRule(File::kFallout4, 0x0C8675, 0) == Find(0x0C8675, 0));  // BoSR03
	CHECK(!StartRule(File::kFallout4, 0x06D646, 0));                      // DN084: its quickstarts are its 1 to 9
	CHECK(!StartRule(File::kCoast, 0x174029, 0));

	// The airport battle's 10, which play sets to start it (Inst307's 30), and its 15: no row, so set like any stage.
	const Situation live{ .sharedStory = true, .live = true };
	CHECK(!Find(0x174029, 10));
	CHECK(!Starting(StartRule(File::kFallout4, 0x174029, 0), Find(0x174029, 10)));
	CHECK(Judge(nullptr, live) == Verdict::kSet);
	// Judged with the quickstart's row, as it was once, it was skipped: no friend's game ever started the battle. Starting
	// itself doesn't take a kDebug 0 for the start-up stage either.
	const auto quickstart = Find(0x174029, 0);
	REQUIRE(quickstart);
	Rule asQuickstart = *quickstart;
	asQuickstart.stage = 10;
	CHECK(Judge(&asQuickstart, live) == Verdict::kSkip);
	CHECK(!Starting(quickstart, Find(0x174029, 10)));
	// BoS303's 40, which has a row of its own: judged by that row alone.
	const auto brotherhood = Starting(StartRule(File::kFallout4, 0x0FDC8C, 0), Find(0x0FDC8C, 40));
	REQUIRE(brotherhood);
	CHECK(!(brotherhood->flags & kDebug));
	CHECK(brotherhood->flags == FlagsOf(0x0FDC8C, 40));
	CHECK(Judge(&*brotherhood, live) == Verdict::kSet);
}

TEST("stage rules: a stage that starts its quest is judged with the quest's own start-up stages, as its record flags them, not its 0")
{
	using namespace StageRules;
	// The start-up stages as Fallout4.esm's quest records flag them (INDX, QUEST_STAGE_DATA's kStartUp; the game hands
	// them to QuestSync as QuestStageLayout::StartUps): MQ101's is its 0, Inst305's and Inst307ChangeLocation's their 10
	// (as MQ205's), and MQ105, MS01 and BoS201 have none.
	constexpr std::array<std::uint16_t, 1> AT_0{ 0 };
	constexpr std::array<std::uint16_t, 1> AT_10{ 10 };
	constexpr std::span<const std::uint16_t> NONE{};
	// Inst305: its start-up 10 moves the player, so a friend's 20 that starts the quest here moves them too. Judged by its
	// 0 (a quickstart, kDebug), it was set plain.
	CHECK(FlagsOf(0x0A274B, 10) == kMoves);
	CHECK(IsDebug(File::kFallout4, 0x0A274B, 0));
	const auto inst305 = Starting(File::kFallout4, 0x0A274B, 20, AT_10);
	REQUIRE(inst305);
	CHECK(inst305->flags == kMoves);
	CHECK(inst305->stage == 10);  // no row of its own: the start-up stage's
	CHECK(!Starting(File::kFallout4, 0x0A274B, 20, NONE));  // running here already: its own row (none)
	// Inst307ChangeLocation: its 10 is the start-up stage and the stage itself; judged once by its row.
	const auto change = Starting(File::kFallout4, 0x17CA39, 10, AT_10);
	REQUIRE(change);
	CHECK(change->flags == FlagsOf(0x17CA39, 10));
	// MQ105 has no start-up stage: its 10 (set by MQ104 210 to start it) is judged alone, not with its 0's move.
	CHECK(!Starting(File::kFallout4, 0x0229E6, 10, NONE));
	// MS01's 1050 alone: no start-up stage, its 0 never runs as the quest starts.
	const auto ms01 = Starting(File::kFallout4, 0x0229F6, 1050, NONE);
	REQUIRE(ms01);
	CHECK(ms01->flags == 0);
	CHECK(Judge(&*ms01, {}) == Verdict::kSet);
	// A quest whose start-up stage is its 0 (as the model's in tests/sim): merged with it as before.
	const auto ms01At0 = Starting(File::kFallout4, 0x0229F6, 1050, AT_0);
	REQUIRE(ms01At0);
	CHECK(ms01At0->flags == kMoves);
	CHECK(ms01At0->wait == 50);
	// A start-up stage that is a quickstart (kDebug) is never merged.
	CHECK(!Starting(File::kFallout4, 0x174029, 10, AT_0));  // Inst307Fight
	// The HOSTILE rows go the same way: InstKickOut's start-up 0 for its 100 only when it starts the quest.
	CHECK(HostileRows(File::kFallout4, 0x16D036, 100, AT_0).size() == 2);
	CHECK(HostileRows(File::kFallout4, 0x16D036, 100, AT_10).size() == 1);
	CHECK(HostileRows(File::kFallout4, 0x16D036, 100, NONE).size() == 1);
}

TEST("stage rules: a developer's quickstart (Mass Fusion's DN084 1 to 9 and BoS303 0 to 3, the airport battle's 0 to 8 and the others on the way to a faction's kick-out) is never shared, whatever else its row says")
{
	using namespace StageRules;
	for (std::uint16_t stage = 1; stage <= 9; ++stage) {
		CHECK(IsDebug(File::kFallout4, 0x06D646, stage));  // DN084
		CHECK(FlagsOf(0x06D646, stage) == (kMoves | kDebug));
	}
	for (std::uint16_t stage = 0; stage <= 8; ++stage) {  // its 0 and 1 are the ones the others go through; play starts the battle at its 10
		CHECK(IsDebug(File::kFallout4, 0x174029, stage));  // Inst307Fight
		CHECK(FlagsOf(0x174029, stage) == (kMoves | kDebug));
	}
	for (std::uint16_t stage = 0; stage <= 3; ++stage) {  // "Debug Start-Up", "Debug StartUp Roof", "... Interior", "... Flight"
		CHECK(IsDebug(File::kFallout4, 0x0FDC8C, stage));  // BoS303, the Brotherhood's side of Mass Fusion
		CHECK(FlagsOf(0x0FDC8C, stage) == (kMoves | kDebug));
	}
	// The other quests on the way to a kick-out, each with a designer's note that says what it is and no script, scene,
	// dialogue or terminal that sets it (tools/questscan.ps1 -NeverSet).
	struct Marked
	{
		std::uint32_t quest;
		std::uint16_t stage;
		std::uint8_t  flags;
	};
	const Marked marked[] = {
		{ 0x0229EE, 0, kDebug }, { 0x0229EE, 1, kDebug }, { 0x0229EE, 2, kDebug }, { 0x0229EE, 3, kDebug },                    // MQ302
		{ 0x043275, 1, kMoves | kDebug }, { 0x043275, 2, kDebug }, { 0x043275, 3, kDebug }, { 0x043275, 4, kDebug },           // RR303
		{ 0x0B1752, 0, kDebug },                                                                                              // MQ206
		{ 0x0C9B0B, 10, kDebug }, { 0x0C9B0B, 30, kDebug }, { 0x0C9B0B, 40, kDebug },                                          // MQ00: "Simulate the player getting kicked out of ..."
		{ 0x0C9B0B, 50, kMoves | kDebug }, { 0x0C9B0B, 60, kMoves | kDebug },                                                  // ... and its endings' slides
		{ 0x0DDFDB, 0, kDebug }, { 0x0DDFDB, 1, kDebug },                                                                      // Min301
		{ 0x0DFB3C, 0, kDebug },                                                                                              // MinDestBoS
		{ 0x10C64C, 10, kDebug },                                                                                             // MQ302RR
		{ 0x142FD3, 1, kMoves | kDebug }, { 0x142FD3, 2, kDebug }, { 0x142FD3, 20, kMoves | kDebug },                          // RR304: play starts it at its 100
		{ 0x142FD3, 25, kMoves | kDebug }, { 0x142FD3, 26, kDebug }, { 0x142FD3, 27, kDebug }, { 0x142FD3, 30, kMoves | kDebug },
	};
	for (const auto& row : marked) {
		CHECK(IsDebug(File::kFallout4, row.quest, row.stage));
		CHECK(FlagsOf(row.quest, row.stage) == row.flags);
	}
	// What play sets of these quests is not: Mass Fusion's 0 and from its 10 on, the airport battle from its 10 on (the
	// story's own stage, Inst307 30, starts it there), BoS303 from its 10 on, RR304 from its 100 on.
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 0));
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 10));
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 225));
	CHECK(!IsDebug(File::kFallout4, 0x174029, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0FDC8C, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0FDC8C, 40));
	CHECK(!IsDebug(File::kFallout4, 0x0FDC8C, 200));
	CHECK(!IsDebug(File::kFallout4, 0x142FD3, 100));
	CHECK(!IsDebug(File::kFallout4, 0x0229EE, 10));
	CHECK(!IsDebug(File::kFallout4, 0x043275, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0C9B0B, 20));
	// The other quests' quickstarts that set a stage a friend's game would be sent, each with a designer's note naming it
	// a debug, test or quick start (the scan's DEBUG list): the Brotherhood's own quests', the story's, the Railroad's,
	// the add-ons'.
	const Marked others[] = {
		{ 0x02BF21, 1, kMoves | kDebug }, { 0x02BF21, 4, kMoves | kDebug }, { 0x02BF21, 35, kMoves | kDebug },  // BoS201: "Quest PreFlight QuickStart" ... "Debug - Flight Avoidance Override"
		{ 0x04402C, 0, kMoves | kDebug }, { 0x04402C, 1, kDebug }, { 0x04402C, 9, kMoves | kDebug },          // BoSM02: "Debug: Start at Kells", "Debug Setup", "Debug: Kells"
		{ 0x0ADCE5, 1, kDebug }, { 0x0ADCE5, 4, kMoves | kDebug },                                            // BoSM04
		{ 0x173ED9, 2, kDebug },                                                                              // BoS304: "Prime Debug Startup 2"
		{ 0x0229EA, 0, kMoves | kDebug }, { 0x0229EA, 8, kMoves | kDebug },                                    // MQ203: "quickstart main quest", "TESTING - start memory H"
		{ 0x06B500, 0, kMoves | kDebug },                                                                     // MQ204: "TESTING ONLY - QUICKSTART"
		{ 0x0B2D48, 5, kDebug }, { 0x0B2D48, 25, kDebug },                                                    // RR201: "Debug: Initial", "DEBUG: Back to Institute"
		{ 0x07CC61, 10, kDebug },                                                                             // RR301: "DEBUG: Quest available"
		{ 0x0A274B, 0, kDebug },                                                                              // Inst305: "test start phase"
	};
	for (const auto& row : others) {
		CHECK(IsDebug(File::kFallout4, row.quest, row.stage));
		CHECK(FlagsOf(row.quest, row.stage) == row.flags);
	}
	CHECK(IsDebug(File::kCoast, 0x001B44, 2));       // DLC03MQ06: "Debug stage - Start wind farm path only"
	CHECK(IsDebug(File::kRobot, 0x002833, 3));       // DLC01MQ04: "Build Jezebel Debug Stage"
	CHECK(IsDebug(File::kNukaWorld, 0x000804, 4));   // DLC04MQ04: "DEBUG START - Player needs to build final settlement ..."
	CHECK(IsDebug(File::kNukaWorld, 0x000801, 3));   // DLC04MQ01: "[QS] Arena Post Fight"
	// DLC04Grandchester 0 ("Quickstart"): a stage 0 that is no start-up stage, moves the player and sets nothing else.
	CHECK(IsDebug(File::kNukaWorld, 0x000808, 0));
	CHECK(FlagsOf(0x000808, 0, File::kNukaWorld) == (kMoves | kDebug));
	CHECK(!IsDebug(File::kNukaWorld, 0x000808, 1));
	// Their quests' real stages are not, nor the one left unmarked on purpose: DN070 5 (the rig's follow step), nor a stage
	// the designer's note calls no debug one.
	CHECK(!IsDebug(File::kFallout4, 0x02BF21, 10));
	CHECK(!IsDebug(File::kFallout4, 0x04402C, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0229EA, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0E9914, 5));
	CHECK(!IsDebug(File::kFallout4, 0x0229E6, 10));  // MQ105 10, its start in play (MQ104 210); its 0 is a quickstart
	CHECK(!IsDebug(File::kFallout4, 0x0229FE, 200));
	CHECK(!IsDebug(File::kCoast, 0x04E77A, 160));    // DLC03FarHarborM02: "[MQ02] Far Harbor section skipped"
	CHECK(!IsDebug(File::kCoast, 0x043822, 0));      // DLC03MQ02Fight 0 ("Quickstart: Initial"): its fight's counter sets a stage it is passed
	// No other file's quest of the same form ID, no stage without a row.
	CHECK(!IsDebug(File::kCoast, 0x06D646, 2));
	CHECK(!IsDebug(File::kFallout4, 0x01F25E, 0));  // MQ104
	// Skipped like a payment, next to the friend who set it or far away, live or caught up, the story shared or not: never
	// set, never the faction alone (kHostileOnly), never an offer to go along.
	const auto quickstart = Find(0x06D646, 2);
	REQUIRE(quickstart);
	CHECK(Judge(quickstart, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(Judge(quickstart, { .sharedStory = true, .live = true, .nearFriend = false, .followable = true }) == Verdict::kSkip);
	CHECK(Judge(quickstart, { .sharedStory = true, .live = false }) == Verdict::kSkip);
	CHECK(Judge(quickstart, { .sharedStory = false, .live = true, .nearFriend = true }) == Verdict::kSkip);
	// A row of the flag alone is skipped too: the flag decides, not the move.
	const StageRules::Rule debug{ 0, 0x000001, 1, kDebug, 0 };
	CHECK(Judge(&debug, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
}

namespace
{
	using Pairs = std::initializer_list<std::pair<std::uint32_t, std::uint16_t>>;

	// StageRules::SetByQuickstart in a game where these stages of Fallout4.esm's quests are done.
	bool SetByQuickstartWith(std::uint32_t a_quest, std::uint16_t a_stage, Pairs a_done, File a_file = File::kFallout4)
	{
		return StageRules::SetByQuickstart(a_file, a_quest, a_stage, [&](File a_by, std::uint32_t a_byQuest, std::uint16_t a_byStage) {
			return a_by == File::kFallout4 && std::ranges::find(a_done, std::pair{ a_byQuest, a_byStage }) != a_done.end();
		});
	}
}

TEST("stage rules: the stages the quickstarts' scripts set (QUICKSTART_SETS) are sorted for the lookup, name only quickstarts, hold no quickstart, and every quickstart whose script sets a stage has some")
{
	using namespace StageRules;
	const auto key = [](const QuickstartSet& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage, a_row.byFile, a_row.byQuest, a_row.byStage }; };
	CHECK(std::ranges::is_sorted(QUICKSTART_SETS, {}, key));
	CHECK(std::ranges::adjacent_find(QUICKSTART_SETS, {}, key) == std::end(QUICKSTART_SETS));  // one row for a stage and a quickstart
	for (const auto& row : QUICKSTART_SETS) {
		CHECK(IsDebug(static_cast<File>(row.byFile), row.byQuest, row.byStage));
		CHECK(!IsDebug(static_cast<File>(row.file), row.quest, row.stage));  // a quickstart is never reported as it is (IsDebug)
	}
	std::size_t quickstarts = 0;
	for (const auto& rule : RULES) {
		if (rule.flags & kDebug) {
			++quickstarts;
			const bool sets = std::ranges::any_of(QUICKSTART_SETS, [&](const QuickstartSet& a_row) {
				return a_row.byFile == rule.file && a_row.byQuest == rule.quest && a_row.byStage == rule.stage;
			});
			// All but MQ00's three that only turn a faction ("Simulate the player getting kicked out of ..."), and the ones that
			// only move the player: DLC04Grandchester 0, the Combat Zone's 1 and MS05BTapes 0. No stage set.
			const bool setsNothing = (rule.file == 0 && rule.quest == 0x0C9B0B && (rule.stage == 10 || rule.stage == 30 || rule.stage == 40)) ||
			                         (rule.file == 3 && rule.quest == 0x000808 && rule.stage == 0) || (rule.file == 0 && rule.quest == 0x02074B && rule.stage == 1) ||
			                         (rule.file == 0 && rule.quest == 0x16854F && rule.stage == 0);
			CHECK(sets == !setsNothing);
		}
	}
	// Mass Fusion's DN084 1 to 9 and BoS303 0 to 3, the airport battle's 0 to 8, and the 25 others on the way to a kick-out
	// (MQ302 0 to 3, RR303 1 to 4, MQ206 0, MQ00 10 to 60, Min301 0 and 1, MinDestBoS 0, MQ302RR 10, RR304's seven): 47;
	// and the 114 other quests' quickstarts that set a stage a friend's game would be sent (MS07c 190, MS16 89, MS05B 252
	// and 253 and DLC04MS01 550 among them): 161; DLC04Grandchester 0, a move alone: 162; the quests shared since: the
	// Combat Zone's 1, RRAct3Pickup 1 and 2, DLC03MQ00 10, DLC03CoA_DialogueNucleusEntrance 3, DLC03AtomM01_CombatStop 1,
	// DLC01Lair 3 to 9 and DLC04MQ05 0 to 3: 179; MQ105 0 ("TESTING ONLY"), left out
	// once for QuestSync's RepairStory: 180 in the scan's DEBUG list; and SafeScan.QuickStart's four stage 0s: 184.
	CHECK(quickstarts == 184);
}

TEST("stage rules: what the scan's proven stage 0 quickstarts set (MS07 0, BoSM04Postquest 0, DN154 0) is their own game's while they are done there, as DEBUG's are")
{
	using namespace StageRules;
	constexpr std::uint32_t MS07 = 0x0229FC;
	constexpr std::uint32_t BOSM04POSTQUEST = 0x145722;
	constexpr std::uint32_t DN154 = 0x0254B7;
	constexpr std::uint32_t DIALOGUE_DIAMOND_CITY = 0x003648;
	constexpr std::uint32_t MQ103 = 0x0229E5;
	constexpr std::uint32_t MQ104 = 0x01F25E;
	constexpr std::uint32_t BOS301 = 0x0AE51C;
	CHECK(IsDebug(File::kFallout4, MS07, 0));
	CHECK(IsDebug(File::kFallout4, BOSM04POSTQUEST, 0));
	// MS07 0 ("QUICKSTART") sets DialogueDiamondCity 170, whose script sets 600 and through it MQ103's end and MQ104's start.
	CHECK(SetByQuickstartWith(DIALOGUE_DIAMOND_CITY, 170, { { MS07, 0 } }));
	CHECK(SetByQuickstartWith(DIALOGUE_DIAMOND_CITY, 600, { { MS07, 0 } }));
	CHECK(SetByQuickstartWith(MQ103, 100, { { MS07, 0 } }));
	CHECK(SetByQuickstartWith(MQ104, 10, { { MS07, 0 } }));
	// BoSM04Postquest 0 ("Debug Start") sets its own 1 ("Initial Setup"), which sets the Brotherhood's questline on.
	CHECK(SetByQuickstartWith(BOSM04POSTQUEST, 1, { { BOSM04POSTQUEST, 0 } }));
	CHECK(SetByQuickstartWith(BOS301, 255, { { BOSM04POSTQUEST, 0 } }));
	CHECK(SetByQuickstartWith(DN154, 10, { { DN154, 0 } }));
	// Reached in play (the talk with Ellie, the Brotherhood's own way), they are reported.
	CHECK(!SetByQuickstartWith(DIALOGUE_DIAMOND_CITY, 170, {}));
	CHECK(!SetByQuickstartWith(DIALOGUE_DIAMOND_CITY, 170, { { MS07, 50 } }));
	CHECK(!SetByQuickstartWith(BOSM04POSTQUEST, 1, { { BOSM04POSTQUEST, 10 } }));
	CHECK(!SetByQuickstartWith(MQ104, 10, { { MQ103, 30 } }));
}

TEST("stage rules: the shutdown stages a quickstart's script runs by stopping a quest (MS07 0: MQ103 1000, MQ104 1000) are its own game's while it is done there, and reported when play set them")
{
	using namespace StageRules;
	constexpr std::uint32_t MS07 = 0x0229FC;
	constexpr std::uint32_t MS07C = 0x0229FE;
	constexpr std::uint32_t MQ103 = 0x0229E5;
	constexpr std::uint32_t MQ104 = 0x01F25E;
	// No script sets either 1000: each is its quest's shutdown stage, which the game runs when the quest stops. MS07 0
	// stops MQ104 (its pMQ04 property holds MQ104) and sets MQ103 100, which stops MQ103.
	CHECK(SetByQuickstartWith(MQ103, 1000, { { MS07, 0 } }));
	CHECK(SetByQuickstartWith(MQ104, 1000, { { MS07, 0 } }));
	// Nick's own quickstarts go the same way.
	for (const auto stage : std::initializer_list<std::uint16_t>{ 0, 1, 190 }) {
		CHECK(SetByQuickstartWith(MQ103, 1000, { { MS07C, stage } }));
		CHECK(SetByQuickstartWith(MQ104, 1000, { { MS07C, stage } }));
	}
	// Played to their end, they are reported.
	CHECK(!SetByQuickstartWith(MQ103, 1000, {}));
	CHECK(!SetByQuickstartWith(MQ104, 1000, {}));
	CHECK(!SetByQuickstartWith(MQ104, 1000, { { MQ103, 100 } }));
	CHECK(!IsDebug(File::kFallout4, MQ103, 1000));
	CHECK(!IsDebug(File::kFallout4, MQ104, 1000));
}

TEST("stage rules: a stuck main story's MQ105 is started where MQ104 went past 210 without it, not where a quickstart typed there stopped MQ104 at 1000")
{
	using namespace StageRules;
	constexpr std::uint32_t MS07 = 0x0229FC;
	constexpr std::uint32_t MQ103 = 0x0229E5;
	const auto repairs = [](std::uint16_t a_stage, bool a_at210, bool a_mq105Started, Pairs a_done) {
		return RepairsStory(a_stage, a_at210, a_mq105Started, [&](File a_by, std::uint32_t a_byQuest, std::uint16_t a_byStage) {
			return a_by == File::kFallout4 && std::ranges::find(a_done, std::pair{ a_byQuest, a_byStage }) != a_done.end();
		});
	};
	// An older version's loss: MQ104 straight to 1000, no 210, MQ105 never started.
	CHECK(repairs(1000, false, false, {}));
	CHECK(repairs(1000, false, false, { { MQ103, 30 } }));  // a stage of the story is no quickstart
	// MS07 0 at this game's console stopped MQ104: its 1000 is the quickstart's, and the story is not stuck.
	CHECK(!repairs(1000, false, false, { { MS07, 0 } }));
	// Nothing to repair: 210 ran, MQ105 is under way, or MQ104 is not past 210.
	CHECK(!repairs(1000, true, false, {}));
	CHECK(!repairs(1000, false, true, {}));
	CHECK(!repairs(210, false, false, {}));
	CHECK(!repairs(120, false, false, {}));
}

TEST("stage rules: what MQ105 0 (a console quickstart) sets is its own game's while it is done there, and reported when play set it")
{
	constexpr std::uint32_t MQ105 = 0x0229E6;
	// It sets the story up to Getting a Clue: Diamond City's entrance and the mayor's speech, MQ103 and MQ104's end.
	CHECK(SetByQuickstartWith(0x0190BC, 55, { { MQ105, 0 } }));   // DialogueDiamondCityEntrance 55
	CHECK(SetByQuickstartWith(0x0229E5, 30, { { MQ105, 0 } }));   // MQ103 30
	CHECK(SetByQuickstartWith(0x01F25E, 120, { { MQ105, 0 } }));  // MQ104 120
	CHECK(!SetByQuickstartWith(0x0229E5, 30, { { MQ105, 10 } }));
	CHECK(!SetByQuickstartWith(0x0229E5, 30, {}));
}

TEST("stage rules: the quickstarts of the quests shared since the DEBUG list was made are never shared, nor what they set (CZ 1, RRAct3Pickup 1-2, DLC01Lair 3-9, DLC04MQ05 0-3, Far Harbor's DLC03MQ00 10, NucleusEntrance 3, CombatStop 1)")
{
	using namespace StageRules;
	CHECK(FlagsOf(0x02074B, 1) == (kMoves | kDebug));  // CZ: MoveTo CZDebugMarker
	CHECK(FlagsOf(0x185E58, 1) == (kMoves | kDebug));  // RRAct3Pickup: "Debug: Supported MM"
	CHECK(FlagsOf(0x185E58, 2) == (kMoves | kDebug));  // "Debug: Supported BoS"
	for (std::uint16_t stage = 3; stage <= 9; ++stage) {
		CHECK(FlagsOf(0x0008BB, stage, File::kRobot) == (kMoves | kDebug));  // DLC01Lair: "Quickstart - ..."
	}
	for (std::uint16_t stage = 0; stage <= 3; ++stage) {
		CHECK(FlagsOf(0x000805, stage, File::kNukaWorld) == (kMoves | kDebug));  // DLC04MQ05: "Debug Start Quest - ..."
	}
	CHECK(FlagsOf(0x001B3E, 10, File::kCoast) == kDebug);  // DLC03MQ00: "DEBUG - Quickstart the DLC03 Main Quest"
	CHECK(FlagsOf(0x004C8F, 3, File::kCoast) == kDebug);   // DLC03CoA_DialogueNucleusEntrance: "[DEBUG] Bypass entrance quest"
	CHECK(FlagsOf(0x05D37B, 1, File::kCoast) == kDebug);   // DLC03AtomM01_CombatStop: "DEBUG - Set Children friendly to player"
	// What they set: RRAct3Pickup 1 the Institute's way in (MQ206Min 1500), DLC01Lair 8 the lair's 1350 (its robots the
	// player's enemy), DLC04MQ05 1 its 8 (a gang the player's enemy), DLC03MQ00 10 the main quest's first stage.
	CHECK(SetByQuickstartWith(0x0B26BE, 1500, { { 0x185E58, 1 } }));
	const auto with = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_by, std::uint16_t a_byStage) {
		return SetByQuickstart(a_file, a_quest, a_stage, [&](File a_doneFile, std::uint32_t a_doneQuest, std::uint16_t a_doneStage) {
			return a_doneFile == a_file && a_doneQuest == a_by && a_doneStage == a_byStage;
		});
	};
	CHECK(with(File::kRobot, 0x0008BB, 1350, 0x0008BB, 8));
	CHECK(!with(File::kRobot, 0x0008BB, 1350, 0x0008BB, 1000));
	CHECK(with(File::kNukaWorld, 0x000805, 8, 0x000805, 1));
	CHECK(!with(File::kNukaWorld, 0x000805, 8, 0x000805, 5));
	// Far Harbor's main-quest quickstart sets the base game's story up to there (MQ106 10), a stage of another file's quest.
	CHECK(SetByQuickstart(File::kFallout4, 0x0229E7, 10, [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage) {
		return a_file == File::kCoast && a_quest == 0x001B3E && a_stage == 10;
	}));
	CHECK(!SetByQuickstartWith(0x0229E7, 10, { { 0x0229E6, 10 } }));
	// The quests' stages that play sets are not: the Combat Zone's arena, the lair's battle, Power Play's choice.
	CHECK(!IsDebug(File::kFallout4, 0x02074B, 250));
	CHECK(!IsDebug(File::kRobot, 0x0008BB, 1350));
	CHECK(!IsDebug(File::kNukaWorld, 0x000805, 5));
	CHECK(!IsDebug(File::kCoast, 0x001B3E, 100));
}

TEST("stage rules: a quickstart past stage 63 is one too (MS07c 190, MS16 89, MS05B 252 and 253, DLC04MS01 550): never shared, and what it sets is its own game's while it is done there")
{
	using namespace StageRules;
	constexpr std::uint32_t MS07C = 0x0229FE;
	constexpr std::uint32_t MS16 = 0x022A07;
	constexpr std::uint32_t MS05B = 0x14B717;
	constexpr std::uint32_t DLC04MS01 = 0x000806;
	// The designers' notes: "DEBUG - Give player all tapes, teleport him and Nick to Andrew station", "Shortcut - Start
	// Subway Dig", "DEBUG - Teleport to Delivery location", "DEBUG - Teleport to Lynn Woods", "DEBUG: Move player & Sierra".
	CHECK(IsDebug(File::kFallout4, MS07C, 190));
	CHECK(IsDebug(File::kFallout4, MS16, 89));
	CHECK(IsDebug(File::kFallout4, MS05B, 252));
	CHECK(IsDebug(File::kFallout4, MS05B, 253));
	CHECK(IsDebug(File::kNukaWorld, DLC04MS01, 550));
	// What each sets is left out while it is done in that game, by its own stage, not a bit of one below 64.
	CHECK(SetByQuickstartWith(MS07C, 200, { { MS07C, 190 } }));
	CHECK(SetByQuickstartWith(MS16, 95, { { MS16, 89 } }));
	CHECK(SetByQuickstartWith(MS05B, 254, { { MS05B, 252 } }));
	CHECK(SetByQuickstartWith(MS05B, 254, { { MS05B, 253 } }));
	const auto nukaWorld = [](std::uint16_t a_done) {
		return SetByQuickstart(File::kNukaWorld, DLC04MS01, 500, [&](File a_by, std::uint32_t a_byQuest, std::uint16_t a_byStage) {
			return a_by == File::kNukaWorld && a_byQuest == DLC04MS01 && a_byStage == a_done;
		});
	};
	CHECK(nukaWorld(550));
	// Reached in play, with another stage of the quest done or none, it is reported.
	CHECK(!SetByQuickstartWith(MS07C, 200, {}));
	CHECK(!SetByQuickstartWith(MS07C, 200, { { MS07C, 62 } }));
	CHECK(!SetByQuickstartWith(MS16, 95, { { MS16, 25 } }));
	CHECK(!SetByQuickstartWith(MS05B, 254, { { MS05B, 250 } }));
	CHECK(!nukaWorld(500));
	CHECK(!nukaWorld(38));
}

TEST("stage rules: a stage is a quickstart's while a quickstart that sets it is done in that game (the kick-outs of Mass Fusion's 2 and 1 and of the airport battle's), and not when play reached it")
{
	constexpr std::uint32_t DN084 = 0x06D646;
	constexpr std::uint32_t INST307FIGHT = 0x174029;
	constexpr std::uint32_t INSTMASSFUSION = 0x15BD39;
	constexpr std::uint32_t BOSKICKOUT = 0x16FF04;
	constexpr std::uint32_t BOS303 = 0x0FDC8C;
	constexpr std::uint32_t INSTKICKOUT = 0x16D036;
	// Mass Fusion's 2 (the Institute's side): InstMassFusion 30, the Brotherhood's kick-out (it sets BoSKickOut 20), and BoSKickOut 30.
	CHECK(SetByQuickstartWith(INSTMASSFUSION, 30, { { DN084, 2 } }));
	CHECK(SetByQuickstartWith(BOSKICKOUT, 20, { { DN084, 2 } }));
	CHECK(SetByQuickstartWith(BOSKICKOUT, 30, { { DN084, 2 } }));
	// Mass Fusion's 1 (the Brotherhood's side): BoS303 30, and the Institute's kick-out that starts for a player who is in the Institute.
	CHECK(SetByQuickstartWith(BOS303, 30, { { DN084, 1 } }));
	CHECK(SetByQuickstartWith(INSTKICKOUT, 0, { { DN084, 1 } }));
	// The airport battle's 2 to 8 set its 1, which sets BoSKickOut 20; its 0 and its 1 typed themselves do too.
	for (std::uint16_t stage = 0; stage <= 8; ++stage) {
		CHECK(SetByQuickstartWith(BOSKICKOUT, 20, { { INST307FIGHT, stage } }));
	}
	// The same stages reached in play, no quickstart done in that game: reported.
	CHECK(!SetByQuickstartWith(INSTMASSFUSION, 30, {}));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, {}));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 30, {}));
	CHECK(!SetByQuickstartWith(BOS303, 30, {}));
	CHECK(!SetByQuickstartWith(INSTKICKOUT, 0, {}));
	// The two quests' stages that are no quickstarts don't count, nor does the stage that sets the kick-out in play.
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { DN084, 10 }, { DN084, 50 }, { INST307FIGHT, 10 }, { INSTMASSFUSION, 30 } }));
	// Only a quickstart whose script sets that stage: Mass Fusion's 1 runs no kick-out of the Brotherhood's, its 4 goes past
	// InstMassFusion 30 (and still sets BoSKickOut 30), its 2 and the airport battle's set nothing of BoS303's side.
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { DN084, 1 } }));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 30, { { DN084, 1 } }));
	CHECK(!SetByQuickstartWith(INSTMASSFUSION, 30, { { DN084, 4 } }));
	CHECK(SetByQuickstartWith(BOSKICKOUT, 30, { { DN084, 4 } }));
	CHECK(!SetByQuickstartWith(BOS303, 30, { { DN084, 2 } }));
	CHECK(!SetByQuickstartWith(BOS303, 30, { { INST307FIGHT, 2 } }));
	// Any one of the done quickstarts is enough.
	CHECK(SetByQuickstartWith(BOS303, 30, { { DN084, 2 }, { INST307FIGHT, 5 }, { DN084, 3 } }));
	// What the quickstart's script doesn't set is reported after it: the stages played on from there.
	CHECK(!SetByQuickstartWith(INSTMASSFUSION, 50, { { DN084, 2 } }));
	CHECK(!SetByQuickstartWith(DN084, 50, { { DN084, 1 }, { DN084, 2 }, { DN084, 9 } }));
	CHECK(!SetByQuickstartWith(INST307FIGHT, 395, { { INST307FIGHT, 0 }, { INST307FIGHT, 8 } }));
	// Both sides of a branch in the script count, whichever the game took: Mass Fusion's 3 has the Institute's quest's 50
	// beside the Brotherhood's.
	CHECK(SetByQuickstartWith(INSTMASSFUSION, 50, { { DN084, 3 } }));
	CHECK(SetByQuickstartWith(BOS303, 50, { { DN084, 3 } }));
	// A quickstart itself has no row (IsDebug is what keeps it home), and only Fallout4.esm's quests have any.
	CHECK(!SetByQuickstartWith(INST307FIGHT, 1, { { INST307FIGHT, 2 } }));
	CHECK(!SetByQuickstartWith(DN084, 3, { { BOS303, 2 } }));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { DN084, 2 } }, File::kCoast));

	// The Brotherhood's side's own quickstarts (BoS303's 1 "Roof" and 2 "Interior"): its 30, and the Institute's kick-out.
	for (const std::uint16_t stage : { std::uint16_t{ 1 }, std::uint16_t{ 2 } }) {
		CHECK(SetByQuickstartWith(BOS303, 30, { { BOS303, stage } }));
		CHECK(SetByQuickstartWith(INSTKICKOUT, 0, { { BOS303, stage } }));
		CHECK(SetByQuickstartWith(INSTKICKOUT, 10, { { BOS303, stage } }));
		CHECK(SetByQuickstartWith(INSTKICKOUT, 20, { { BOS303, stage } }));
		CHECK(SetByQuickstartWith(DN084, 10, { { BOS303, stage } }));
	}
	// Its 2 goes on through Mass Fusion's 3: what that sets is its own too.
	CHECK(SetByQuickstartWith(INSTMASSFUSION, 50, { { BOS303, 2 } }));
	CHECK(SetByQuickstartWith(DN084, 20, { { BOS303, 2 } }));
	CHECK(!SetByQuickstartWith(INSTMASSFUSION, 50, { { BOS303, 1 } }));
	// Its 0 and its 3 ("Flight") run no kick-out; its real stages name nothing; its end is no quickstart's.
	CHECK(!SetByQuickstartWith(BOS303, 30, { { BOS303, 0 } }));
	CHECK(!SetByQuickstartWith(BOS303, 30, { { BOS303, 3 } }));
	CHECK(!SetByQuickstartWith(INSTKICKOUT, 0, { { BOS303, 3 } }));
	CHECK(!SetByQuickstartWith(INSTKICKOUT, 0, { { BOS303, 30 }, { BOS303, 40 } }));
	CHECK(!SetByQuickstartWith(BOS303, 200, { { BOS303, 0 }, { BOS303, 1 }, { BOS303, 2 }, { BOS303, 3 }, { DN084, 3 } }));
	// The Railroad's quickstarts run the Brotherhood's kick-out: RR303's 1, RR304's 1, 20, 25 and 30 (stages past 15 are
	// named the same way).
	CHECK(SetByQuickstartWith(BOSKICKOUT, 20, { { 0x043275, 1 } }));
	for (const std::uint16_t stage : { std::uint16_t{ 1 }, std::uint16_t{ 20 }, std::uint16_t{ 25 }, std::uint16_t{ 30 } }) {
		CHECK(SetByQuickstartWith(BOSKICKOUT, 20, { { 0x142FD3, stage } }));
	}
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { 0x142FD3, 2 } }));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { 0x142FD3, 26 } }));
	CHECK(!SetByQuickstartWith(BOSKICKOUT, 20, { { 0x142FD3, 100 } }));  // where play starts RR304: no quickstart
	// MQ00's endings ("Simulate endgame binks"): the last quest's stages they set.
	CHECK(SetByQuickstartWith(0x0229EE, 10, { { 0x0C9B0B, 50 } }));
	CHECK(SetByQuickstartWith(0x0229EE, 10, { { 0x0C9B0B, 60 } }));
	CHECK(!SetByQuickstartWith(0x0229EE, 10, { { 0x0C9B0B, 40 } }));
}

TEST("stage rules: the fades and the endings' slides move the player")
{
	using namespace StageRules;
	CHECK(FlagsOf(0x01ED62, 40) == kMoves);  // DialogueVault111: CryoPod.Activate(player), into the vault's pod
	CHECK(FlagsOf(0x01CC2A, 4) == kMoves);   // MQ102: the Pip-Boy's image-space effect
	// The endings: they fade to black and play the slides, needed all the same (they finish the story).
	CHECK(FlagsOf(0x0229EE, 850) == (kMoves | kBridge));  // MQ302: FadetoBlackImod
	CHECK(FlagsOf(0x0229EE, 855) == (kMoves | kBridge));  // MQ302: EndGameBinks
	// MQ00's 50 and 60 play them too, but nothing in play sets them (the developers' "Simulate endgame binks"): quickstarts.
	CHECK(FlagsOf(0x0C9B0B, 50) == (kMoves | kDebug));
	CHECK(FlagsOf(0x0C9B0B, 60) == (kMoves | kDebug));
	CHECK(FlagsOf(0x0BAD00, 30) == (kMoves | kBridge));   // Inst308
	// The waits were kept.
	CHECK(Hold(Find(0x0229EE, 850)) == 26s);
}

TEST("stage rules: nothing that moves the player is set while this game is in the opening; a friend in it holds nothing here")
{
	using namespace StageRules;
	const Rule plain{ 0, 0x1, 10, 0, 0 };
	const Rule pays{ 0, 0x1, 10, kPays, 0 };
	const Rule moves{ 0, 0x1, 10, kMoves, 0 };
	const Rule bridge{ 0, 0x1, 10, kMoves | kBridge, 0 };

	// Live and beside the friend who did it, warned, in no fight: it waits, its quest only.
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .opening = true }) == Verdict::kOpeningWaits);
	CHECK(Judge(&bridge, { .live = true, .nearFriend = true, .opening = true }) == Verdict::kOpeningWaits);
	CHECK(Judge(&bridge, { .live = false, .opening = true, .warned = WARNING }) == Verdict::kOpeningWaits);
	CHECK(Judge(&bridge, { .live = false, .opening = true }) == Verdict::kOpeningWaits);  // no warning while it can't come
	// ... and once the opening is over, as before.
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .opening = false }) == Verdict::kSet);
	CHECK(Judge(&bridge, { .live = false, .opening = false }) == Verdict::kWarn);
	// A friend still in the vault (three or more in the session): the others, side by side, go on as ever; their own game
	// holds what moves them.
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .friendInOpening = true }) == Verdict::kSet);
	CHECK(Judge(&moves, { .live = false, .friendInOpening = true }) == Verdict::kSkip);
	CHECK(Judge(&bridge, { .live = false, .friendInOpening = true }) == Verdict::kWarn);
	CHECK(Judge(&moves, { .live = true, .nearFriend = true, .opening = true, .friendInOpening = true }) == Verdict::kOpeningWaits);
	// What doesn't move the player goes on; a payment is never made.
	CHECK(Judge(&plain, { .live = true, .opening = true }) == Verdict::kSet);
	CHECK(Judge(nullptr, { .live = false, .opening = true }) == Verdict::kSet);
	CHECK(Judge(&pays, { .live = true, .nearFriend = true, .opening = true }) == Verdict::kSkip);
	// A stage that moves the player with a warning waits too: MQ201's start.
	CHECK(Judge(Find(0x0229E8, 0), { .sharedStory = true, .opening = true }) == Verdict::kOpeningWaits);
}

TEST("stage rules: the stages that set another quest's are sorted, once each, and are the fragments' (Codsworth's talk and search)")
{
	for (auto it = std::next(std::begin(StageRules::SETS_ANOTHER)); it != std::end(StageRules::SETS_ANOTHER); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } < std::tuple{ it->file, it->quest, it->stage }));
	}
	CHECK(StageRules::SetsAnother(File::kFallout4, 0x01E64E, 50));    // DialogueCodsworthPostWar: the talk sets MQ102's 30
	CHECK(StageRules::SetsAnother(File::kFallout4, 0x01E64E, 200));   // ... back with him: MQ102's 50
	CHECK(StageRules::SetsAnother(File::kFallout4, 0x01CC2A, 40));    // MQ102: the search over stops his (DialogueCodsworthPostWar 100)
	CHECK(StageRules::SetsAnother(File::kFallout4, 0x01CC2A, 38));    // ... through the 40 it sets
	CHECK(StageRules::SetsAnother(File::kFallout4, 0x01CC2A, 150));   // MQ102 starts MQ103 at 10
	CHECK(!StageRules::SetsAnother(File::kFallout4, 0x01CC2A, 30));   // objectives only
	CHECK(!StageRules::SetsAnother(File::kFallout4, 0x01E64E, 100));  // stops his search scene, sets nothing
	CHECK(!StageRules::SetsAnother(File::kCoast, 0x01E64E, 50));
}

TEST("stage rules: the stages of the quests sQuests = safe shares that move the player or take from them (tools/questscan.ps1 -Safe -Rules)")
{
	using namespace StageRules;
	// Nuka-World's main line under no type: DLC04MQ03's flag poles, MoveTo each: its quickstarts ("Debug Galactic Zone
	// Cleared" ...), which set the park's stages, so never shared (kDebug).
	for (std::uint16_t stage = 1; stage <= 5; ++stage) {
		CHECK(FlagsOf(0x000803, stage, File::kNukaWorld) == (kMoves | kDebug));
	}
	// DLC04MQ05 is SAFE's again (shared for its outcome, its gang globals carried: CARRIES), so its quickstarts have rows: kDebug.
	CHECK(FlagsOf(0x000805, 1, File::kNukaWorld) == (kMoves | kDebug));
	CHECK(FlagsOf(0x000805, 3, File::kNukaWorld) == (kMoves | kDebug));
	// DLC04MS01: a Nuka-Cola Quartz taken; its MoveTo (550, "DEBUG: Move player & Sierra") is a quickstart.
	CHECK(FlagsOf(0x000806, 75, File::kNukaWorld) == kPays);
	CHECK(FlagsOf(0x000806, 550, File::kNukaWorld) == (kMoves | kDebug));
	// Fallout4.esm's: Diamond City's caps, the treasure's cave, the Prydwen's gantry, the Combat Zone's quickstart.
	CHECK(FlagsOf(0x02074B, 1) == (kMoves | kDebug));  // CZ: MoveTo CZDebugMarker
	CHECK(FlagsOf(0x14191C, 10) == kFree);   // FFDiamondCity02: Home Plate, its price waived in a friend's game (PRICES)
	CHECK(FlagsOf(0x0E9914, 5) == kMoves);   // DN070
	CHECK(FlagsOf(0x145722, 2) == kMoves);   // BoSM04Postquest: SAFE's again, its 0 a developer's quickstart
	// Far Harbor's treasure dig fades the screen out.
	CHECK(FlagsOf(0x04D2F5, 200, File::kCoast) == kMoves);  // DLC03PostQuestTreasureDig
	// A quest's own item taken is left alone (only its script's wait kept): the fog condenser module.
	const auto module = Find(0x023ACA, 210, File::kCoast);  // DLC03FarHarborFF02
	REQUIRE(module);
	CHECK(module->flags == 0);
	CHECK(module->wait == 50);
	CHECK(!Find(0x10CB94, 100));                   // FFGoodneighbor03: its own item, no wait
	CHECK(!Find(0x02039A, 300, File::kCoast));     // DLC03FarHarborFF03c
	// Their stage 0s that move the player are the developers' quickstarts, none the quest's start-up stage: kDebug (never
	// its start: StartRule) where the scan's DEBUG list names it (DLC04MQ03's, which sets the park's stages; DLC04MQ05's, which sets its 8) or it proves
	// nothing in the game's files sets it (DN154's, BoSM04Postquest's: SafeScan.QuickStart); the others are left out.
	CHECK(IsDebug(File::kNukaWorld, 0x000803, 0));  // DLC04MQ03
	CHECK(!StartRule(File::kNukaWorld, 0x000803, 0));
	CHECK(FlagsOf(0x000805, 0, File::kNukaWorld) == (kMoves | kDebug));  // DLC04MQ05: the DEBUG list names it
	CHECK(FlagsOf(0x0254B7, 0) == kDebug);        // DN154
	CHECK(FlagsOf(0x145722, 0) == kDebug);        // BoSM04Postquest

	// A far friend's move is skipped, a quickstart's always; a payment is never made from a friend's game.
	const auto move = Find(0x0E9914, 5);  // DN070: MoveTo DN070_TreasuresEntry
	CHECK(Judge(move, { .sharedStory = true, .live = true, .nearFriend = false }) == Verdict::kSkip);
	CHECK(Judge(move, { .sharedStory = true, .live = false }) == Verdict::kSkip);
	CHECK(Judge(move, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSet);
	CHECK(Judge(Find(0x000803, 1, File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(Judge(Find(0x000806, 75, File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);

	// The ones that set another shared quest's stage (-Safe -SetsOther): DLC04MQ03 655, DLC04MS01's, DLC04GZMainQuest 1000.
	CHECK(SetsAnother(File::kNukaWorld, 0x000803, 655));
	CHECK(SetsAnother(File::kNukaWorld, 0x000806, 75));
	CHECK(SetsAnother(File::kNukaWorld, 0x01E34A, 1000));
	CHECK(SetsAnother(File::kCoast, 0x023ACA, 400));  // DLC03FarHarborFF02
	CHECK(SetsAnother(File::kFallout4, 0x02074B, 500));  // CZ 500 sets CZPostQuest 10
}

TEST("stage rules: the stages of the objective-less quests sQuests = safe shares (QuestFilter::SILENT; tools/questscan.ps1 -Safe -Rules)")
{
	using namespace StageRules;
	// DLC03MQ02Fight's quickstart moves the player and sets the fight's stages, never shared (kDebug); its 0 (another)
	// is no start-up stage: left out.
	CHECK(FlagsOf(0x043822, 5, File::kCoast) == (kMoves | kDebug));
	CHECK(!Find(0x043822, 0, File::kCoast));
	// Grandchester's pre-war money and its ticket are taken: never from a friend's stage.
	CHECK(FlagsOf(0x000808, 100, File::kNukaWorld) == kPays);
	CHECK(FlagsOf(0x000808, 120, File::kNukaWorld) == kPays);
	CHECK(FlagsOf(0x000808, 0, File::kNukaWorld) == (kMoves | kDebug));  // its 0, a quickstart that moves the player alone
	CHECK(Judge(Find(0x000808, 100, File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	// The Children of Atom's pump regulators are the quest's own (as the fog condenser modules): no row.
	CHECK(!Find(0x02B4FF, 20, File::kCoast));  // DLC03CoA_FFNucleus03_PumpHandler
	CHECK(!Find(0x02B4FF, 30, File::kCoast));
	// Waits: the Castle's waves, the Prydwen's arrival (BoSEnable 10 and 20; its 15 is ScriptDoors').
	REQUIRE(Find(0x0ABF55, 500));  // DN029FirstWave
	CHECK(Find(0x0ABF55, 500)->wait == 30);
	CHECK(FlagsOf(0x1851A0, 10) == 0);
	CHECK(!Find(0x1851A0, 15));
	// Their stages that set another quest's (-Safe -SetsOther), and the shared quests' that set one of theirs.
	CHECK(SetsAnother(File::kFallout4, 0x022A02, 170));  // MS11 starts DN029FirstWave
	CHECK(SetsAnother(File::kFallout4, 0x0ABF55, 500));  // DN029FirstWave sets MS11 180
	CHECK(SetsAnother(File::kFallout4, 0x0229E7, 900));  // MQ106 sets BoSEnable 10
	CHECK(SetsAnother(File::kFallout4, 0x1851A0, 10));   // BoSEnable sets DN165Exterior 255
	CHECK(SetsAnother(File::kCoast, 0x043822, 100));     // DLC03MQ02Fight sets DLC03MQ02 300
	// DLC04MQ00TransitCenter is no longer SILENT's (its 30 sets the repeatable DLC04TransitCenterAnnouncements): its 20,
	// which sets DLC04MQ00 500, is no row.
	CHECK(!SetsAnother(File::kNukaWorld, 0x046A18, 20));
	// The base game's kick-outs and faction outcomes (SILENT's since the hostile hold): Mass Fusion's quickstarts (its 2
	// among them: it sets InstMassFusion 30) move the player and are never shared (kDebug), its fragments' waits; the
	// airport battle's 0 to 8 the same (its 0 moves the player to the airport, its 1 through Inst307 35; play enters at
	// its 10). The kick-outs themselves have no RULES row: what they do is HOSTILE's and DISMISSES'.
	CHECK(FlagsOf(0x06D646, 1) == (kMoves | kDebug));  // DN084
	CHECK(FlagsOf(0x06D646, 2) == (kMoves | kDebug));
	REQUIRE(Find(0x06D646, 225));
	CHECK(Find(0x06D646, 225)->wait == 45);
	CHECK(FlagsOf(0x174029, 1) == (kMoves | kDebug));  // Inst307Fight
	CHECK(FlagsOf(0x174029, 8) == (kMoves | kDebug));
	CHECK(FlagsOf(0x174029, 0) == (kMoves | kDebug));  // its 0 is no start-up stage: a quickstart
	CHECK(!Find(0x16FF04, 20));  // BoSKickOut
	CHECK(!Find(0x16C207, 100));  // RRKickOut
	CHECK(!Find(0x082615, 10));   // DN117
	// Their stages that set another quest's: Inst307Fight 1 sets BoSKickOut 20 and Inst307's, DN084 2 sets BoSKickOut 30, RRKickOut
	// 300 fails the Railroad's quests, the final battles' 200 set DN084 450; and the shared quests' that set one of theirs: BoS302B
	// 30 sets RRKickOut 100, InstMassFusion 30 sets BoSKickOut 20.
	CHECK(SetsAnother(File::kFallout4, 0x174029, 1));
	CHECK(SetsAnother(File::kFallout4, 0x06D646, 2));
	CHECK(SetsAnother(File::kFallout4, 0x16C207, 300));
	CHECK(SetsAnother(File::kFallout4, 0x0F8D39, 200));
	CHECK(SetsAnother(File::kFallout4, 0x0F8D3A, 200));
	CHECK(SetsAnother(File::kFallout4, 0x107A1C, 30));
	CHECK(SetsAnother(File::kFallout4, 0x15BD39, 30));
}

TEST("stage rules: a stage of a quest shared for its outcome that turns a faction on the player is HOSTILE's, no RULES row: it waits until none of that faction is near (tools/questscan.ps1 -Hostile)")
{
	using namespace StageRules;
	// Each one the scan found (SafeScan.Hostile), in the quests tools/questscan.cs ALLOW_OUTCOMES lets into SAFE.
	const std::tuple<File, std::uint32_t, std::uint16_t> hostile[] = {
		{ File::kFallout4, 0x08FCED, 500 },  // DN036: Phyllis
		{ File::kFallout4, 0x04542B, 120 },  // DN049: the champion
		{ File::kFallout4, 0x04542B, 149 },  // DN049: each shop's robots
		{ File::kFallout4, 0x04542B, 221 },
		{ File::kFallout4, 0x04542B, 249 },
		{ File::kFallout4, 0x04542B, 349 },
		{ File::kFallout4, 0x04542B, 449 },
		{ File::kFallout4, 0x04542B, 549 },
		{ File::kFallout4, 0x04542B, 649 },
		{ File::kFallout4, 0x04542B, 749 },
		{ File::kFallout4, 0x04542B, 849 },
		{ File::kCoast, 0x004C8E, 70 },       // DLC03CoA_DialogueNucleus: sets DLC03CoA_Kickout 700
		{ File::kCoast, 0x00AEC3, 210 },      // DLC03FarHarborS02: Malcolm
		{ File::kCoast, 0x013F3A, 200 },      // DLC03Acadia_Institute: sets DLC03AcadiaDefense 100
		{ File::kCoast, 0x01AC2B, 50 },       // DLC03MQ06_WindFarmHandling: the turbines stopped (sets DLC03MQ00 100)
		{ File::kCoast, 0x0323DE, 700 },      // DLC03CoA_Kickout: the Children of Atom
		{ File::kCoast, 0x032C24, 100 },      // DLC03AcadiaDefense: Acadia
		{ File::kNukaWorld, 0x03684F, 600 },  // DLC04_KiddieKingdomMain: its script's one-time Init
		{ File::kNukaWorld, 0x03684F, 700 },
		{ File::kNukaWorld, 0x03684F, 740 },  // Oswald's fight
		{ File::kNukaWorld, 0x027704, 20 },   // DLC04RaiderKickout: the gangs, and Gage when he was a companion
	};
	for (const auto& [file, quest, stage] : hostile) {
		// No RULES row (nothing else these stages do: no move, no payment, no wait); a HOSTILE row, one faction each.
		CHECK(!Find(quest, stage, file));
		const auto rows = HostileOf(file, quest, stage);
		REQUIRE(rows.size() == 1);
		CHECK(rows.front().faction != 0);
		// QuestSync judges it with kHostile: with the friend who did it near, at once, fighting or not; otherwise it waits
		// while any of the faction is near (or the player fights), live or caught up, and is set once none is; never
		// offered, never skipped, and the opening doesn't hold it.
		const Rule rule{ static_cast<std::uint8_t>(file), quest, stage, kHostile, 0 };
		CHECK(Judge(&rule, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
		CHECK(Judge(&rule, { .live = true, .nearFriend = true, .factionNear = true, .held = true }) == Verdict::kSet);
		CHECK(Judge(&rule, { .live = true, .nearFriend = false, .factionNear = true, .followable = true }) == Verdict::kHostileWaits);
		CHECK(Judge(&rule, { .live = true, .nearFriend = false, .factionNear = false, .followable = true }) == Verdict::kSet);
		CHECK(Judge(&rule, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
		CHECK(Judge(&rule, { .live = false, .factionNear = false }) == Verdict::kSet);
		CHECK(Judge(&rule, { .live = false, .nearFriend = true, .factionNear = true }) == Verdict::kHostileWaits);  // beside the friend, but not live: the fight is long over
		CHECK(Judge(&rule, { .factionNear = false, .opening = true }) == Verdict::kSet);
	}
	// The Nucleus nuked (Far Harbor's ending): HOSTILE's through the kick-out it sets, and a RULES row for the wait the
	// scan read alone (no flag: QuestSync adds kHostile from the HOSTILE row); the blast (150) no row, but a trap (TRAPS:
	// held while the player is inside). The wind farm's code dug up (5) moves the player: kMoves alone.
	REQUIRE(Find(0x015366, 100, File::kCoast) != nullptr);
	CHECK(FlagsOf(0x015366, 100, File::kCoast) == 0);
	CHECK(Find(0x015366, 100, File::kCoast)->wait == 35);
	REQUIRE(HostileOf(File::kCoast, 0x015366, 100).size() == 1);
	CHECK(HostileOf(File::kCoast, 0x015366, 100).front().faction == 0x004462);  // the Children of Atom
	Rule nuke = *Find(0x015366, 100, File::kCoast);
	nuke.flags |= kHostile;
	CHECK(Judge(&nuke, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
	CHECK(Judge(&nuke, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&nuke, { .live = false, .factionNear = false }) == Verdict::kSet);  // never skipped
	CHECK(!Find(0x015366, 150, File::kCoast));
	// Power Play's 8 (DLC04GangSetup) turns the snubbed gang on the player, picked as its script runs from the gang the
	// stage carries (CARRIES): no RULES row, and a HOSTILE row for each of the three gangs (by hand in the scan); it waits
	// for the one the carried value names (HOSTILE_WHEN, its own test below).
	CHECK(!Find(0x000805, 8, File::kNukaWorld));
	const auto gangs = HostileOf(File::kNukaWorld, 0x000805, 8);
	REQUIRE(gangs.size() == 3);
	CHECK(gangs[0].faction == 0x00F438);  // the Disciples
	CHECK(gangs[1].faction == 0x00F439);  // the Operators
	CHECK(gangs[2].faction == 0x00F43A);  // the Pack
	for (const auto& gang : gangs) {
		CHECK(gang.factionFile == static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK(gang.call == kSetPlayerEnemy);
	}
	CHECK(FlagsOf(0x01AC2B, 5, File::kCoast) == kMoves);
	CHECK(SetsAnother(File::kCoast, 0x015366, 100));  // DLC03CoA_Kickout 700
	// The peace a stage makes is no row: DN049's x50 (SetEnemy(PlayerFaction, True, False), then StopCombat) and its 55.
	CHECK(HostileOf(File::kFallout4, 0x04542B, 50).empty());
	CHECK(HostileOf(File::kFallout4, 0x04542B, 150).empty());
	CHECK(HostileOf(File::kFallout4, 0x04542B, 155).empty());
	// Nor a settlement given (DN036's endings, Far Harbor's S02 425 and FF01 220), nor two factions set on each other
	// (DLC04DialogueSettlement 700), nor an ally made (DLC03_ADV017 100).
	CHECK(HostileOf(File::kFallout4, 0x08FCED, 550).empty());
	CHECK(HostileOf(File::kFallout4, 0x08FCED, 700).empty());
	CHECK(HostileOf(File::kCoast, 0x00AEC3, 425).empty());
	CHECK(HostileOf(File::kCoast, 0x023ACB, 220).empty());
	CHECK(HostileOf(File::kNukaWorld, 0x01AABF, 700).empty());
	CHECK(HostileOf(File::kCoast, 0x02C687, 100).empty());
	// One that moves the player is kMoves the same way: Kiddie Kingdom's quickstart 6, which sets its 600 (so kDebug,
	// never shared). And the Nucleus's quickstart.
	CHECK(FlagsOf(0x03684F, 6, File::kNukaWorld) == (kMoves | kDebug));
	CHECK(Judge(Find(0x03684F, 6, File::kNukaWorld), { .live = false }) == Verdict::kSkip);
	CHECK(FlagsOf(0x004C8E, 1, File::kCoast) == kMoves);
	// Power Play's quickstarts 0-3 (a rank set to 3, a MoveTo, then its 5 and 8): kDebug, never shared, and what they set
	// not reported from the game that typed them (QUICKSTART_SETS). None is HOSTILE's (left out by hand in the scan): which
	// gang its 8 would turn on the player is not known here, and no faction is turned on the player in its place.
	for (const auto stage : { 0, 1, 2, 3 }) {
		CHECK(FlagsOf(0x000805, static_cast<std::uint16_t>(stage), File::kNukaWorld) == (kMoves | kDebug));
		CHECK(HostileOf(File::kNukaWorld, 0x000805, static_cast<std::uint16_t>(stage)).empty());
		CHECK(Judge(Find(0x000805, static_cast<std::uint16_t>(stage), File::kNukaWorld), { .live = false }) == Verdict::kSkip);
	}
	CHECK(Judge(Find(0x000805, 1, File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(!Find(0x000805, 5, File::kNukaWorld));  // DLC04MakeEnemy: nothing a rule holds back, the ranks carried instead
	// Their payments: DN049's pre-war money, the bloodworm meat Nuka-Town's trader takes.
	CHECK(FlagsOf(0x04542B, 711) == kPays);
	CHECK(FlagsOf(0x01AABF, 110, File::kNukaWorld) == kPays);
	CHECK(Judge(Find(0x04542B, 711), { .live = true, .nearFriend = true }) == Verdict::kSkip);
	// Their stages that set another shared quest's (-Safe -SetsOther): Far Harbor's settlements move its main quest on.
	CHECK(SetsAnother(File::kCoast, 0x00A991, 500));  // DLC03FarHarborS01 sets DLC03FarHarborM03 775
	CHECK(SetsAnother(File::kCoast, 0x00AEC3, 400));  // DLC03FarHarborS02 sets DLC03FarHarborM03 875
	CHECK(SetsAnother(File::kCoast, 0x013F3A, 200));  // DLC03Acadia_Institute sets DLC03MQ05 8990
	CHECK(SetsAnother(File::kNukaWorld, 0x000804, 10));  // DLC04MQ04 sets DLC04DialogueSettlement 140
	CHECK(!SetsAnother(File::kNukaWorld, 0x00CBD0, 200));  // DLC04DialogueHubologists 200 sets nothing
}

TEST("stage rules: the add-ons' other faction outcomes, Combat Zone's and The Big Dig's end (tools/questscan.ps1 -Safe -Rules, -Hostile)")
{
	using namespace StageRules;
	// The Hubologists: their donations take caps (kPays) and their other stages but 200 are each player's own sessions
	// (kOwn: the rads in the extraction chair, the objective, the rank's global and bonus spells): a friend's are never
	// set here, live or caught up, near or far.
	CHECK(kOwn == kPays);
	for (const std::uint16_t stage : { 20, 21, 22, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34, 35, 36, 37, 40, 41, 50, 51, 52, 53, 54, 55, 56, 57, 100 }) {
		CHECK(FlagsOf(0x00CBD0, stage, File::kNukaWorld) == kOwn);
		CHECK(Judge(Find(0x00CBD0, stage, File::kNukaWorld), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
		CHECK(Judge(Find(0x00CBD0, stage, File::kNukaWorld), { .sharedStory = true, .live = false }) == Verdict::kSkip);
	}
	// Their 200, the Hubologists turned on the player, is no row: HOSTILE's, held while any of them is near.
	CHECK(!Find(0x00CBD0, 200, File::kNukaWorld));
	CHECK(HostileOf(File::kNukaWorld, 0x00CBD0, 200).size() == 1);
	// Harvey's 200 the same; his 1000 (the end of his roadside encounter) has no row.
	CHECK(HostileOf(File::kNukaWorld, 0x0243A5, 200).size() == 1);
	CHECK(!Find(0x0243A5, 1000, File::kNukaWorld));
	// Sierra's attack puts her in PlayerEnemyFaction (Fallout4.esm's): no call made without the stage.
	const auto sierra = HostileOf(File::kNukaWorld, 0x035790, 10);
	REQUIRE(sierra.size() == 1);
	CHECK(sierra.front().factionFile == 0);
	CHECK(sierra.front().faction == 0x106C2F);
	CHECK(sierra.front().call == kAddToFaction);
	// Combat Zone: the arena's raiders at 250 and the stages that reach it; 100 (Tommy's fighters on the player's side) and
	// 500 (Cait made available) are no row. Its 1 is the developers' quickstart.
	CHECK(HostileOf(File::kFallout4, 0x02074B, 250).size() == 1);
	CHECK(HostileOf(File::kFallout4, 0x02074B, 200).size() == 1);
	CHECK(HostileOf(File::kFallout4, 0x02074B, 100).empty());
	CHECK(HostileOf(File::kFallout4, 0x02074B, 500).empty());
	CHECK(!Find(0x02074B, 500));
	CHECK(FlagsOf(0x02074B, 1) == (kMoves | kDebug));
	// The end of The Big Dig: Bobbi at 30; 80 (Hancock made available) is no row.
	CHECK(HostileOf(File::kFallout4, 0x1323AB, 30).size() == 1);
	CHECK(HostileOf(File::kFallout4, 0x1323AB, 80).empty());
	// Far Harbor's kick-out (DLC03MQ00 100), the Nucleus's door (50, and 150 through the Children of Atom's kick-out).
	CHECK(HostileOf(File::kCoast, 0x001B3E, 100).size() == 1);
	CHECK(HostileOf(File::kCoast, 0x001B3E, 0).empty());  // its start-up: the island's factions allied, in every game
	CHECK(HostileOf(File::kCoast, 0x004C8F, 50).size() == 1);
	CHECK(HostileOf(File::kCoast, 0x004C8F, 150).front().faction == 0x004462);
	// The Mechanist's lair: 1400 and 1350, which sets it; its quickstarts 3-9 move the player (8 reaches 1350), never shared.
	CHECK(HostileOf(File::kRobot, 0x0008BB, 1400).size() == 1);
	CHECK(HostileOf(File::kRobot, 0x0008BB, 1350).size() == 1);
	CHECK(HostileOf(File::kRobot, 0x0008BB, 2000).empty());  // the lair's workshop given
	for (std::uint16_t stage = 3; stage <= 9; ++stage) {
		CHECK(FlagsOf(0x0008BB, stage, File::kRobot) == (kMoves | kDebug));
	}
	// Automatron's master quest: its alliances are no row of either table.
	CHECK(HostileOf(File::kRobot, 0x0008BA, 10).empty());
	CHECK(!Find(0x0008BA, 10, File::kRobot));
	// The stages of theirs that set another shared quest's (-Safe -SetsOther).
	CHECK(SetsAnother(File::kFallout4, 0x1323AB, 90));    // FFGoodneighbor07 sets MS16 500
	CHECK(SetsAnother(File::kCoast, 0x001B3E, 100));      // DLC03MQ00's kick-out sets DLC03FarHarborFF01 500 and FF02 500
	CHECK(SetsAnother(File::kRobot, 0x0008BB, 1400));     // DLC01Lair sets DLC01MQ05 890
	CHECK(SetsAnother(File::kFallout4, 0x022A07, 260));   // MS16 260 sets FFGoodneighbor07 3 and 10
}

TEST("stage rules: the Nucleus's blast doors and blast wait while the player is inside the Nucleus, whatever else holds")
{
	using namespace StageRules;
	constexpr std::uint32_t NUKE = 0x015366;  // DLC03MQ06_NukeHandling
	constexpr std::uint32_t NUCLEUS = 0x003A6F;
	constexpr std::uint32_t COMMAND_CENTER = 0x003A99;  // the script's DLC03NucleusMemoryBanks01
	constexpr std::uint32_t SUB_INTERIOR = 0x040F16;
	constexpr std::uint32_t NUCLEUS_EXT = 0x000CF8;  // NucleusExt: outside its doors
	// Its two stages, in each of the three cells 150 checks; the launch (100), the hatch (130) and the end (500) none.
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 140, 150 }) {
		CHECK(FindTrap(File::kCoast, NUKE, stage) != nullptr);
		for (const auto cell : { NUCLEUS, COMMAND_CENTER, SUB_INTERIOR }) {
			CHECK(Trapped(File::kCoast, NUKE, stage, File::kCoast, cell));
		}
		CHECK(!Trapped(File::kCoast, NUKE, stage, File::kCoast, NUCLEUS_EXT));
		CHECK(!Trapped(File::kCoast, NUKE, stage, File::kCoast, 0));          // no cell (a loading screen)
		CHECK(!Trapped(File::kCoast, NUKE, stage, std::nullopt, NUCLEUS));     // a cell of a file the rules don't know
		CHECK(!Trapped(File::kCoast, NUKE, stage, File::kFallout4, NUCLEUS));  // Fallout4.esm's form of that ID
	}
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 0, 100, 130, 500 }) {
		CHECK(!FindTrap(File::kCoast, NUKE, stage));
		CHECK(!Trapped(File::kCoast, NUKE, stage, File::kCoast, NUCLEUS));
	}
	CHECK(!Trapped(File::kFallout4, NUKE, 150, File::kCoast, NUCLEUS));  // the same IDs in another file
	// Trapped: the stage waits, its quest with it, live or caught up, the friend near or far, warned or not; out of the
	// Nucleus, it is set as before (no row: set live or caught up).
	const auto blast = Find(NUKE, 150, File::kCoast);
	CHECK(!blast);
	CHECK(Judge(blast, { .live = true, .nearFriend = true, .trapped = true }) == Verdict::kTrapWaits);
	CHECK(Judge(blast, { .live = false, .trapped = true }) == Verdict::kTrapWaits);
	CHECK(Judge(blast, { .held = true, .trapped = true }) == Verdict::kTrapWaits);
	CHECK(Judge(blast, { .warned = WARNING, .trapped = true }) == Verdict::kTrapWaits);
	CHECK(Judge(blast, { .live = true }) == Verdict::kSet);
	CHECK(Judge(blast, { .live = false }) == Verdict::kSet);
	CHECK(Judge(Find(NUKE, 140, File::kCoast), { .live = false, .trapped = true }) == Verdict::kTrapWaits);
	CHECK(Judge(Find(NUKE, 140, File::kCoast), { .live = false }) == Verdict::kSet);
	// The launch is no trap. It is HOSTILE's (the Children of Atom turned on the player through the kick-out it sets), so
	// a friend's waits while any of them is near, as they are inside the Nucleus, and is set once none is: never skipped.
	CHECK(Judge(Find(NUKE, 100, File::kCoast), { .live = false }) == Verdict::kSet);
	Rule launch = *Find(NUKE, 100, File::kCoast);
	launch.flags |= kHostile;
	CHECK(Judge(&launch, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
	// What is never a player's to have stays skipped, trapped or not.
	const Rule pays{ 1, NUKE, 150, kPays, 0 };
	CHECK(Judge(&pays, { .live = true, .nearFriend = true, .trapped = true }) == Verdict::kSkip);
	// The table: every trap in a file the rules know, with a cell.
	for (const auto& trap : TRAPS) {
		CHECK(trap.file <= static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK(trap.cells[0] != 0);
	}
}

TEST("stage rules: no shared stage a friend's game sets pays through a stage it sets (tools/questscan.ps1 -Safe)")
{
	using namespace StageRules;
	// The scan's last line, SHARED_REACHES_PAYS: the stages of every quest a friend's stage is set on (the story's,
	// the dialogue quests', SAFE's, SILENT's, the radiants') that are no kPays or kFree row and reach, through their
	// fragments and the script functions they call, in their own quest or any other, a stage with a kPays or kFree row,
	// caps taken or the inventory emptied, or any item taken by a stage of a quest no friend's stage is set on (its
	// takes were never read for kPays rows), not read by hand.
	CHECK(SHARED_REACHES_PAYS == 0);
	// What it found: buying Home Plate from Geneva (DialogueDiamondCity 64) or the mayor (300) sets FFDiamondCity02 10,
	// which takes the price and gives the key and the house. A friend's purchase is set here with its price at 0: every
	// player has the key and the house, only the buyer pays. Set like any stage that doesn't move the player.
	for (const int stage : { 64, 300 }) {
		CHECK(FlagsOf(0x003648, static_cast<std::uint16_t>(stage)) == kFree);
		CHECK(Judge(Find(0x003648, static_cast<std::uint16_t>(stage)), { .sharedStory = true, .live = true, .nearFriend = false }) == Verdict::kSet);
		CHECK(Judge(Find(0x003648, static_cast<std::uint16_t>(stage)), { .sharedStory = true, .live = false }) == Verdict::kSet);
		const auto price = FindPrice(File::kFallout4, 0x003648, static_cast<std::uint16_t>(stage));
		REQUIRE(price);
		CHECK(price->global == 0x0D35FA);       // ServiceCostBuyHouse
		CHECK(price->boughtQuest == 0x14191C);  // FFDiamondCity02 10
		CHECK(price->boughtStage == 10);
		CHECK(price->given == 0x141946);        // DmndPlayerHouseKey
	}
	CHECK(FlagsOf(0x14191C, 10) == kFree);  // FFDiamondCity02 itself, shared with sQuests = safe
	CHECK(FindPrice(File::kFallout4, 0x14191C, 10));
	CHECK(!FindPrice(File::kFallout4, 0x003648, 70));  // the bat's sale is no purchase: never made here
	CHECK(!FindPrice(File::kCoast, 0x003648, 64));
	CHECK(SetsAnother(File::kFallout4, 0x003648, 64));   // what they set is another quest's stage
	CHECK(SetsAnother(File::kFallout4, 0x003648, 300));
	CHECK(!Find(0x003648, 65));  // the rest of Geneva's talk is not held back
	// Every kFree row has its price, and every price is a kFree row's.
	for (const auto& rule : RULES) {
		if (rule.flags & kFree) {
			CHECK(FindPrice(static_cast<File>(rule.file), rule.quest, rule.stage));
			CHECK(!(rule.flags & kPays));
		}
	}
	for (const auto& price : PRICES) {
		CHECK(FlagsOf(price.quest, price.stage, static_cast<File>(price.file)) == kFree);
	}
	// The price stays 0 until the key is given, or a few minutes of play if it never is (a lagging Papyrus queue runs the script late).
	CHECK(!PriceBack(false, 0s));
	CHECK(!PriceBack(false, 59s));  // not a minute: the script can be queued longer
	CHECK(!PriceBack(false, PRICE_TIMEOUT - 1s));
	CHECK(PRICE_TIMEOUT >= 3min);
	CHECK(PriceBack(true, 0s));
	CHECK(PriceBack(false, PRICE_TIMEOUT));
	// InstM01_OLD, an old cut of InstM01 never started in a game played through, takes 500 caps at its 35 as InstM01 does.
	CHECK(FlagsOf(0x0498B5, 35) == kPays);
	CHECK(FlagsOf(0x0986C7, 35) == kPays);
	// The pre-war stages that set MQ101's (its start-up stage empties the inventory) are never set from a friend's.
	for (const auto& [quest, stage] : { std::pair{ 0x0BA4F2u, 150 }, std::pair{ 0x0BA4F2u, 200 }, std::pair{ 0x0C3309u, 91 }, std::pair{ 0x0C3309u, 500 } }) {
		CHECK(Judge(Find(quest, static_cast<std::uint16_t>(stage)), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	}
	// The one it lists and has read by hand: MS13MarowskiRevenge 380 (SAFE) sets its own 400, which takes the caps unless 380
	// is done, and is set from 380.
	CHECK(QuestFilter::IsSafe(0, 0x084E40));
	CHECK(FlagsOf(0x084E40, 400) == kPays);
	CHECK(!Find(0x084E40, 380));
}

namespace
{
	using Waiver = StageRules::PriceWaiver;
	using Do = Waiver::Do;
	using Why = Waiver::Why;

	constexpr std::uint32_t PRICE = 0x0D35FA;
	constexpr float         OWN_VALUE = 2000.0f;
	const StageRules::Clock::time_point T0{};

	Waiver::Waived Purchase(std::uint32_t a_global = PRICE, float a_value = OWN_VALUE, std::uint32_t a_mainMenus = 1)
	{
		Waiver::Waived waived;
		waived.global = a_global;
		waived.value = a_value;
		waived.given = 0x141946;
		waived.givenBefore = 0;
		waived.quest = 0x003648;
		waived.stage = 64;
		waived.mainMenus = a_mainMenus;
		return waived;
	}
}

TEST("price waiver: held at 0 until what was bought is given, then its own value back, once")
{
	Waiver waiver;
	CHECK(!waiver.On());
	CHECK(waiver.Poll(T0, false, false, 1).what == Do::kNothing);
	CHECK(!waiver.BeforeSave());
	CHECK(!waiver.End());

	REQUIRE(waiver.Begin(Purchase(), T0));
	CHECK(waiver.On());
	const auto held = waiver.Poll(T0 + 1s, false, false, 1);
	CHECK(held.what == Do::kHold);
	CHECK(held.waived.global == PRICE);
	CHECK(waiver.On());

	const auto given = waiver.Poll(T0 + 2s, false, true, 1);
	CHECK(given.what == Do::kRestore);
	CHECK(given.why == Why::kGiven);
	CHECK(given.waived.global == PRICE);
	CHECK(given.waived.value == OWN_VALUE);
	CHECK(!waiver.On());
	CHECK(waiver.Poll(T0 + 3s, false, true, 1).what == Do::kNothing);  // over: nothing is zeroed or restored twice
	CHECK(!waiver.End());
}

TEST("price waiver: a script that never gives goes back after the timeout of play, not of paused or loading time")
{
	Waiver waiver;
	REQUIRE(waiver.Begin(Purchase(), T0));
	auto now = T0;
	// A long pause (a menu, a loading screen) is no play.
	now += 20min;
	CHECK(waiver.Poll(now, true, false, 1).what == Do::kHold);
	// Play up to just under the timeout holds.
	now += StageRules::PRICE_TIMEOUT - 1s;
	CHECK(waiver.Poll(now, false, false, 1).what == Do::kHold);
	CHECK(waiver.On());
	now += 1s;
	const auto late = waiver.Poll(now, false, false, 1);
	CHECK(late.what == Do::kRestore);
	CHECK(late.why == Why::kTimeout);
	CHECK(late.waived.value == OWN_VALUE);
	CHECK(!waiver.On());
}

TEST("price waiver: another stage of the same purchase starts the window over and keeps the first value; another price is refused")
{
	Waiver waiver;
	REQUIRE(waiver.Begin(Purchase(), T0));
	CHECK(waiver.Poll(T0 + StageRules::PRICE_TIMEOUT - 1s, false, false, 1).what == Do::kHold);
	// The second stage reads the global as the waiver left it, 0: the first value is the one kept.
	REQUIRE(waiver.Begin(Purchase(PRICE, 0.0f), T0 + StageRules::PRICE_TIMEOUT - 1s));
	CHECK(waiver.Poll(T0 + StageRules::PRICE_TIMEOUT + 2s, false, false, 1).what == Do::kHold);
	const auto end = waiver.Poll(T0 + StageRules::PRICE_TIMEOUT * 2, false, false, 1);
	CHECK(end.what == Do::kRestore);
	CHECK(end.waived.value == OWN_VALUE);

	// A purchase with another global while one is waived would be zeroed and never put back: refused, the first untouched.
	REQUIRE(waiver.Begin(Purchase(), T0));
	CHECK(!waiver.Begin(Purchase(0x0D35FB, 50.0f), T0));
	const auto kept = waiver.End();
	REQUIRE(kept);
	CHECK(kept->global == PRICE);
	CHECK(kept->value == OWN_VALUE);
}

TEST("price waiver: a save gets the value back and the waiver goes on")
{
	Waiver waiver;
	REQUIRE(waiver.Begin(Purchase(), T0));
	const auto saved = waiver.BeforeSave();
	REQUIRE(saved);
	CHECK(saved->global == PRICE);
	CHECK(saved->value == OWN_VALUE);
	CHECK(waiver.On());
	// The next poll holds it at 0 again (the caller sets it), and it ends as usual.
	CHECK(waiver.Poll(T0 + 1s, false, false, 1).what == Do::kHold);
	CHECK(waiver.Poll(T0 + 2s, false, true, 1).what == Do::kRestore);
	CHECK(!waiver.BeforeSave());  // nothing to put back for a save once it is over
}

TEST("price waiver: a load begins, the value is back and no later poll zeroes it (a load longer than the hold lets frames run)")
{
	Waiver waiver;
	REQUIRE(waiver.Begin(Purchase(), T0));
	const auto ended = waiver.End();
	REQUIRE(ended);
	CHECK(ended->global == PRICE);
	CHECK(ended->value == OWN_VALUE);
	CHECK(!waiver.On());
	// The frames of the long load: nothing sets the price to 0.
	for (int i = 1; i <= 100; ++i) {
		CHECK(waiver.Poll(T0 + std::chrono::seconds(i), true, false, 1).what == Do::kNothing);
	}
	// And the world loaded finds nothing left to drop.
	CHECK(!waiver.End());
}

TEST("price waiver: the main menu opening ends it with the value back; so does the world loaded or begun after it")
{
	Waiver waiver;
	REQUIRE(waiver.Begin(Purchase(PRICE, OWN_VALUE, 1), T0));
	CHECK(waiver.Poll(T0 + 1s, true, false, 1).what == Do::kHold);  // loading screens and the like are no main menu
	const auto quit = waiver.Poll(T0 + 2s, true, false, 2);
	CHECK(quit.what == Do::kRestore);
	CHECK(quit.why == Why::kMainMenu);
	CHECK(quit.waived.value == OWN_VALUE);
	CHECK(!waiver.On());
	CHECK(waiver.Poll(T0 + 3s, true, false, 2).what == Do::kNothing);  // the menu's polls don't zero it again

	// Should the menu have gone unseen (a poll missed it), the new game or load's reset restores it all the same.
	REQUIRE(waiver.Begin(Purchase(PRICE, OWN_VALUE, 2), T0));
	const auto reset = waiver.End();
	REQUIRE(reset);
	CHECK(reset->value == OWN_VALUE);
	CHECK(!waiver.On());
}

TEST("price waiver: every way it ends hands back the value it began with, whichever order the events come in")
{
	enum class Event { kSave, kPoll, kGiven, kLoad, kMenu, kNewWorld, kTimeout };
	constexpr Event events[] = { Event::kSave, Event::kPoll, Event::kGiven, Event::kLoad, Event::kMenu, Event::kNewWorld, Event::kTimeout };
	// Every pair of events after a purchase: the waiver, if it ended, ended with the value, and a global the caller keeps
	// (the model of the game's) is never left at 0 once it is over.
	for (const auto first : events) {
		for (const auto second : events) {
			Waiver waiver;
			float  global = OWN_VALUE;
			auto   now = T0;
			REQUIRE(waiver.Begin(Purchase(), now));
			global = 0.0f;
			const auto run = [&](Event a_event) {
				now += 1s;
				switch (a_event) {
				case Event::kSave:
					if (const auto saved = waiver.BeforeSave()) {
						global = saved->value;
					}
					break;
				case Event::kLoad:
				case Event::kNewWorld:
					if (const auto ended = waiver.End()) {
						global = ended->value;
					}
					break;
				default: {
					const bool given = a_event == Event::kGiven;
					const auto mainMenus = a_event == Event::kMenu ? 2u : 1u;
					if (a_event == Event::kTimeout) {
						now += StageRules::PRICE_TIMEOUT;
					}
					const auto polled = waiver.Poll(now, false, given, mainMenus);
					if (polled.what == Do::kHold) {
						global = 0.0f;
					} else if (polled.what == Do::kRestore) {
						global = polled.waived.value;
					}
					break;
				}
				}
			};
			run(first);
			run(second);
			// Whatever is left on is held at 0 by the next poll; whatever ended has the value. Ending what is left on, as a
			// load does, leaves the value in every case.
			if (const auto ended = waiver.End()) {
				global = ended->value;
			}
			CHECK(global == OWN_VALUE);
			CHECK(!waiver.On());
		}
	}
}

TEST("stage rules: a companion's quest's stages that are its owner's (kEach) are set in a friend's game by their EACH acts alone")
{
	using namespace StageRules;
	constexpr std::uint32_t CURIE = 0x16454E;     // COMCurieQuest, Emergent Behavior
	constexpr std::uint32_t MACCREADY = 0x027411;  // COMMacCreadyQuest, Long Road Ahead
	// The flag's own bit: 7 (6 is kDebug's), shared with no other flag.
	static_assert(kEach == 1 << 7);
	static_assert((kEach & (kMoves | kBridge | kPays | kStory | kFree | kHostile | (1 << 6))) == 0);
	// The table is sorted (a stage's acts kept in their order), every row's stage is kEach, and only the companions'
	// quests have kEach stages: those shared as the companions' (COMPANION_QUESTS), Nick's and Cait's shared by their
	// type, and Nick's misc quest after the Memory Den shared as SAFE's.
	const auto companions = [](std::uint8_t a_file, std::uint32_t a_quest) {
		return QuestFilter::IsCompanionQuest(a_file, a_quest) ||
		       (a_file == 0 && (a_quest == 0x0229FE || a_quest == 0x1666C5 || a_quest == 0x0FAC86));  // MS07c, COMCaitQuest, MQ202NickPostSurgery
	};
	for (auto it = std::next(std::begin(EACH)); it != std::end(EACH); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } <= std::tuple{ it->file, it->quest, it->stage }));
	}
	for (const auto& each : EACH) {
		CHECK(IsEach(static_cast<File>(each.file), each.quest, each.stage));
		CHECK(companions(each.file, each.quest));
	}
	for (const auto& rule : RULES) {
		if (rule.flags & kEach) {
			CHECK(companions(rule.file, rule.quest));
			CHECK(rule.wait == 0);  // no script runs here to wait for
			CHECK(!(rule.flags & (kMoves | kPays | kStory)));  // nothing of its script runs in a mirror: no move, no payment, no story step
		}
	}
	CHECK(QuestFilter::IsSafe(0, 0x0FAC86));  // Nick's quest after the Memory Den: shared as SAFE's
	// No shared stage's script sets a kEach stage of a quest with a driver (tools/questscan.ps1 -Safe walks every shared
	// stage that isn't kEach, through what it calls and sets, and prints the count): such a stage would run the owner's
	// stage with its script in a friend's game, and the fresh-stage rule would take that game for the driver. MS07c 305 sets
	// MS07cPostQuest 5 that way, which is why that quest stays unshared; MQ204 200 sets MQ202NickPostSurgery 95, so its 95 and
	// 100 are plain.
	CHECK(SHARED_SETS_EACH == 0);
	CHECK(!QuestFilter::IsSafe(0, 0x18817F) && !QuestFilter::IsSilent(0, 0x18817F));  // MS07cPostQuest
	for (const auto& rule : RULES) {
		CHECK((rule.file != 0 || rule.quest != 0x18817F));
	}

	// Curie: the owner's recruit, affinity and perks, kEach; her new body the world's half here.
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 1, 5, 200, 300, 400, 500, 600 }) {
		CHECK(IsEach(File::kFallout4, CURIE, stage));
	}
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 0, 100, 125, 150, 175, 310, 320, 610, 700, 9000 }) {
		CHECK(!IsEach(File::kFallout4, CURIE, stage));  // set as any shared stage: their script runs in both games
	}
	CHECK(FlagsOf(CURIE, 620) == kStory);  // the Railroad's RR101 110
	CHECK(SetsAnother(File::kFallout4, CURIE, 620));
	CHECK(EachOf(File::kFallout4, CURIE, 1).empty());  // a quickstart: only marked done
	CHECK(EachOf(File::kFallout4, CURIE, 5).empty());
	CHECK(EachOf(File::kFallout4, CURIE, 300).size() == 8);
	CHECK(EachOf(File::kFallout4, CURIE, 999).empty());

	// MacCready: the hire is a payment (never a friend's), his affinity and the cure his owner's.
	CHECK(FlagsOf(MACCREADY, 0) == kEach);  // the quickstart: MoveTo Goodneighbor, then 30 (the hire's caps): neither in a friend's game
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 30, 40, 50 }) {
		CHECK(FlagsOf(MACCREADY, stage) == kPays);
		CHECK(SetsAnother(File::kFallout4, MACCREADY, stage));  // FFGoodneighbor06 100
		CHECK(Judge(Find(MACCREADY, stage), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	}
	CHECK(IsEach(File::kFallout4, MACCREADY, 120));
	CHECK(IsEach(File::kFallout4, MACCREADY, 255));
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 60, 80, 90, 100, 110, 130, 140, 150, 160, 165, 170, 180, 190, 200, 210, 355 }) {
		CHECK(!Find(MACCREADY, stage));
	}
	// None of his stages that are set in a game played through moves the player: no follow offer, a far friend's set.
	CHECK(Judge(Find(MACCREADY, 140), { .live = true, .nearFriend = false, .followable = true }) == Verdict::kSet);

	// A kEach stage is judged as a plain one: set live or caught up, waiting out a fight only when caught up.
	const auto body = Find(CURIE, 300);
	CHECK(Judge(body, { .live = true, .nearFriend = false, .followable = true }) == Verdict::kSet);
	CHECK(Judge(body, { .live = false }) == Verdict::kSet);
	CHECK(Judge(body, { .live = false, .held = true }) == Verdict::kLater);
}


TEST("stage rules: a companion given a new body is one the engine rolls by her quest's globals, never a session's stored roll")
{
	using namespace StageRules;
	std::size_t bodies = 0;
	for (const auto& each : EACH) {
		if (each.act != Act::kNewBody) {
			continue;
		}
		++bodies;
		const auto driver = DriverOf(static_cast<File>(each.file), each.quest);
		CHECK(driver != nullptr);
		// The quest's companion is in the act's alias (0, Companion) and Fallout4.esm's: her runtime id is her own.
		CHECK(driver && each.file == static_cast<std::uint8_t>(File::kFallout4) && SharedRoll::RolledByGlobals(driver->companion));
	}
	CHECK(bodies == 1);
}

TEST("stage rules: Curie's new body in a friend's game: the globals her base reads, then her items moved and her actor reset, then dressed with her affinity and factions as they were")
{
	using namespace StageRules;
	const auto acts = EachOf(File::kFallout4, 0x16454E, 300);
	std::vector<Act> order;
	for (const auto& each : acts) {
		order.push_back(each.act);
	}
	// The globals before the reset (her levelled base picks the body from them).
	const auto at = [&](Act a_act) { return std::ranges::find(order, a_act) - order.begin(); };
	CHECK(at(Act::kGlobal) < at(Act::kNewBody));
	CHECK(std::ranges::count(order, Act::kGlobal) == 2);
	const auto synth = std::ranges::find_if(acts, [](const Each& a_each) { return a_each.act == Act::kGlobal && a_each.form == 0x10BD4C; });
	const auto robot = std::ranges::find_if(acts, [](const Each& a_each) { return a_each.act == Act::kGlobal && a_each.form == 0x1846CD; });
	CHECK(synth != acts.end() && synth->value == 100 && synth->body);
	CHECK(robot != acts.end() && robot->value == 0 && robot->body);
	const auto body = std::ranges::find_if(acts, [](const Each& a_each) { return a_each.act == Act::kNewBody; });
	// Curie, her items to FakeCurieRobot, to Doctor Amari, in CurieHumanOutfit.
	CHECK(body != acts.end() && body->alias == 0 && body->items == 7 && body->value == 1 && body->form == 0x1E2F26 && body->body);
	// The world's half that isn't her body is done wherever: the lounger and its chair.
	for (const auto& each : acts) {
		if (each.act == Act::kEnable && each.alias == NO_ALIAS) {
			CHECK(!each.body);
		}
		if (each.act == Act::kEnable && each.alias == 7) {
			CHECK(each.body);  // the robot body she leaves comes only where her body changes
		}
	}
	const auto kill = EachOf(File::kFallout4, 0x16454E, 500);
	CHECK(std::ranges::any_of(kill, [](const Each& a_each) { return a_each.act == Act::kKill && a_each.alias == 7 && a_each.body; }));

	// Before the reset: her items into the robot body. Nothing without the robot body. Then the reset, moved to Amari,
	// on its own, once the removal has taken: her inventory reads empty, or RESET_WAIT has passed, never the same poll.
	CHECK(NewBodyRemove(0x00102249, 0x000865D1) == "00102249.removeallitems 000865D1 1");
	CHECK(NewBodyRemove(0x00102249, 0).empty());
	CHECK(NewBodyRemove(0, 0x000865D1).empty());
	CHECK(NewBodyReset(0x00102249, 0x0009A683) == "00102249.recycleactor 0009A683");
	CHECK(NewBodyReset(0x00102249, 0) == "00102249.recycleactor");
	CHECK(NewBodyReset(0, 0x0009A683).empty());
	CHECK(!ResetDue(true, 0s));
	CHECK(ResetDue(true, 500ms));
	CHECK(!ResetDue(false, 500ms));
	CHECK(!ResetDue(false, RESET_WAIT - 1ms));
	CHECK(ResetDue(false, RESET_WAIT));
	CHECK(RESET_WAIT > BODY_WAIT);  // an item left over holds the reset longer than the script's own wait, not for good
	// After it: the outfit, the affinity she had, and each faction change put back as it was.
	const std::vector<FactionRank> factions{ { 0x000A1B85, 0 }, { 0x0001C21C, 1 }, { 0x0006A1E0, -1 } };
	CHECK((NewBodyAfter(0x00102249, 0x001E2F26, 512.5f, factions) ==
		   std::vector<std::string>{ "00102249.setoutfit 001E2F26", "00102249.setav CA_Affinity 512.5", "00102249.addtofaction 000A1B85 0",
			   "00102249.addtofaction 0001C21C 1", "00102249.removefromfaction 0006A1E0" }));
	CHECK((NewBodyAfter(0x00102249, 0x001E2F26, std::nullopt, {}) == std::vector<std::string>{ "00102249.setoutfit 001E2F26" }));
	CHECK((NewBodyAfter(0x00102249, 0, 250.0f, {}) == std::vector<std::string>{ "00102249.setav CA_Affinity 250" }));
	CHECK(BODY_WAIT == std::chrono::seconds(1));  // the script's Utility.Wait(1) before SetOutfit

	// Whether her body changes in a game that mirrors her quest.
	CHECK(BodyOf({ .actor = true, .met = true, .follows = false }) == Body::kChange);
	CHECK(BodyOf({ .actor = true, .met = false, .follows = false }) == Body::kUnmet);    // still in her Vault 81 room there: left alone
	CHECK(BodyOf({ .actor = true, .met = true, .follows = true }) == Body::kFollows);    // the friend's companion now: not swapped under them
	CHECK(BodyOf({ .actor = true, .met = false, .follows = true }) == Body::kUnmet);
	CHECK(BodyOf({ .actor = false, .met = true, .follows = false }) == Body::kNoActor);
	const auto curie = DriverOf(File::kFallout4, 0x16454E);
	CHECK(curie && curie->metQuest == 0x022A0A && curie->metStage == 750);  // MS19 750: her room opened, CaptiveFaction left

	// Nothing of a kEach stage's acts gives XP but the quest's own reward.
	for (const auto& each : EACH) {
		if (each.act == Act::kXp) {
			CHECK(each.quest == 0x16454E && each.stage == 600 && each.form == 0x0AEF8B);
		}
	}
	// MacCready's 255 takes the cure and completes the quest, and doesn't stop it: a friend who never hired him keeps his
	// hire's greeting (its 30 not done, the quest running) for their own save.
	const auto end = EachOf(File::kFallout4, 0x027411, 255);
	CHECK(std::ranges::any_of(end, [](const Each& a_each) { return a_each.act == Act::kTake && a_each.alias == 8; }));
	CHECK(std::ranges::any_of(end, [](const Each& a_each) { return a_each.act == Act::kCompleteQuest; }));
}

TEST("stage rules: a kEach stage's acts as console lines")
{
	using namespace StageRules;
	constexpr std::uint32_t QUEST = 0x0016454E;
	const Each show{ 0, 0x16454E, 200, Act::kShow, NO_ALIAS, 0, 200 };
	const Each complete{ 0, 0x16454E, 200, Act::kComplete, NO_ALIAS, 0, 150 };
	CHECK(Commands(show, QUEST, 0, 0) == std::vector<std::string>{ "setobjectivedisplayed 0016454E 200 1" });
	CHECK(Commands(complete, QUEST, 0, 0) == std::vector<std::string>{ "setobjectivecompleted 0016454E 150 1" });

	const Each enable{ 0, 0x16454E, 300, Act::kEnable, 7 };
	CHECK(Commands(enable, QUEST, 0x000865D1, 0) == std::vector<std::string>{ "000865D1.enable" });
	CHECK(Commands(enable, QUEST, 0, 0).empty());  // the alias empty here: not done (counted)

	const Each kill{ 0, 0x16454E, 500, Act::kKill, 7 };
	CHECK((Commands(kill, QUEST, 0x000865D1, 0x001846CB) == std::vector<std::string>{ "setessential 001846CB 0", "000865D1.kill" }));
	CHECK(Commands(kill, QUEST, 0x000865D1, 0) == std::vector<std::string>{ "000865D1.kill" });

	const Each take{ 0, 0x027411, 255, Act::kTake, 8 };
	CHECK(Commands(take, 0x00027411, 0x00012345, 0x00054321) == std::vector<std::string>{ "player.removeitem 00054321 1" });
	CHECK(Commands(take, 0x00027411, 0, 0).empty());

	const Each xp{ 0, 0x16454E, 600, Act::kXp, NO_ALIAS, 0x0AEF8B };
	CHECK(Commands(xp, QUEST, 0, 0x000AEF8B, 300.0f) == std::vector<std::string>{ "player.modav experience 300" });
	CHECK(Commands(xp, QUEST, 0, 0x000AEF8B, 0.0f).empty());

	CHECK(Commands(Each{ 0, 0x16454E, 600, Act::kCompleteQuest }, QUEST, 0, 0) == std::vector<std::string>{ "completequest 0016454E" });
	CHECK(Commands(Each{ 0, 0x16454E, 300, Act::kGlobal, NO_ALIAS, 0x10BD4C, 100 }, QUEST, 0, 0x0010BD4C).empty());  // written, not a console line
	CHECK(Commands(Each{ 0, 0x16454E, 300, Act::kNewBody, 0, 0x1E2F26, 1, 7, true }, QUEST, 0x00102249, 0).empty());  // NewBodyRemove, NewBodyReset and NewBodyAfter
}

namespace
{
	// One game's companion quest as the drive rule sees it: its done stages, whether the companion is in the driver's
	// faction or alias, whether our Followers alias holds them, the quests driven here since the last load. Each stage
	// that arrives is set as QuestSync sets it: with its script in the driving game, or marked done in a mirroring one. A
	// stage reads done before its script has run (Papyrus runs it a moment later): with lazy set, what a script does waits
	// for RunScripts.
	struct CompanionGame
	{
		std::uint32_t              quest;
		std::vector<std::uint16_t> done;
		bool                       inFaction = false;
		bool                       ours = false;
		std::vector<std::uint16_t> mirrored;  // stages marked done without their script
		bool                       inAlias = false;  // the driver's alias holds the companion (Curie's NewCurieBody)
		bool                       driven = false;   // a kEach stage ran with its script here since the last load (QuestSync's drivenHere)
		bool                       lazy = false;
		std::vector<std::uint16_t> pending;  // scripts not run yet (lazy)

		bool Done(std::uint16_t a_stage) const { return std::ranges::find(done, a_stage) != done.end(); }

		StageRules::DriveFacts Facts() const
		{
			const auto driver = StageRules::DriverOf(StageRules::File::kFallout4, quest);
			const auto isDone = [&](std::uint16_t a_stage) { return Done(a_stage); };
			return { StageRules::Marked(*driver, isDone, { inFaction, inAlias }), driven, StageRules::MirroredSoFar(StageRules::File::kFallout4, quest, isDone), ours };
		}

		StageRules::Drive Drive() const { return StageRules::DriveOf(Facts()); }

		void Arrives(std::uint16_t a_stage)
		{
			if (Done(a_stage)) {
				return;  // StageLog never sets a done stage again
			}
			const bool each = StageRules::IsEach(StageRules::File::kFallout4, quest, a_stage);
			const bool pays = (FlagsOf(quest, a_stage) & StageRules::kPays) != 0;
			if (pays && !ours) {
				return;  // a payment is never a friend's (Judge: kSkip)
			}
			if (each && Drive() == StageRules::Drive::kMirror) {
				mirrored.push_back(a_stage);
				done.push_back(a_stage);
				return;
			}
			driven = driven || each;
			done.push_back(a_stage);
			Script(a_stage);
		}

		// A stage this game's own play sets (a timer, a dialogue): its script runs here.
		void OwnPlay(std::uint16_t a_stage)
		{
			driven = driven || StageRules::IsEach(StageRules::File::kFallout4, quest, a_stage);
			done.push_back(a_stage);
			Script(a_stage);
		}

		void Script(std::uint16_t a_stage)
		{
			pending.push_back(a_stage);
			if (!lazy) {
				RunScripts();
			}
		}

		void RunScripts()
		{
			for (const auto stage : std::exchange(pending, {})) {
				ScriptRuns(stage);
			}
		}

		// The save this game makes now, loaded: what its scripts did is kept, what was only known since the last load is not.
		CompanionGame Loaded() const
		{
			auto loaded = *this;
			loaded.driven = false;
			loaded.pending.clear();
			return loaded;
		}

		// What the stage's own script does to the driver's mark (Curie's 200, 300 and 600).
		void ScriptRuns(std::uint16_t a_stage)
		{
			if (quest == 0x16454E && a_stage == 200) {
				inFaction = true;  // DisallowCompanion
				ours = false;      // ClearCompanion
			}
			if (quest == 0x16454E && a_stage == 300) {
				inFaction = false;  // Reset(aTrueCurie): taken as clearing her faction changes (the worst case)
				inAlias = true;     // ForceRefTo(Alias_NewCurieBody, aTrueCurie)
			}
			if (quest == 0x16454E && a_stage == 600) {
				inFaction = false;  // RemoveFromFaction DisallowedCompanionFaction
				ours = true;        // COMCurie 50: SetCompanion
			}
			if (quest == 0x1666C5 && a_stage == 60) {
				inFaction = true;  // DisallowCompanion(Cait, True): the faction, and DismissCompanion if she followed
				ours = false;
			}
			if (quest == 0x1666C5 && a_stage == 255) {
				inFaction = false;  // AllowCompanion(Cait, True, False): out of the faction, and SetCompanion when none follows
				ours = true;
			}
			if (quest == 0x0FAC86 && a_stage == 90) {
				ours = true;  // AllowCompanion(Nick, True, True): ForceCompanion
			}
		}
	};
}

TEST("stage rules: Nick's, Cait's and Danse's quests: only a stage that would recruit or dismiss a friend's companion is the owner's (kEach); allowing and disallowing a friend's copy is left to run")
{
	using namespace StageRules;
	using namespace std::chrono_literals;
	constexpr std::uint32_t NICK = 0x0229FE;         // MS07c, Long Time Coming
	constexpr std::uint32_t CAIT = 0x1666C5;         // COMCaitQuest, Benign Intervention
	constexpr std::uint32_t DANSE = 0x0B9F9D;        // BoS302, Blind Betrayal
	constexpr std::uint32_t NICK_DEN = 0x0FAC86;     // MQ202NickPostSurgery
	constexpr std::uint32_t NICK_AFTER = 0x18817F;   // MS07cPostQuest (unshared)
	constexpr std::uint32_t SHEFFIELD = 0x0502AC;    // FFDiamondCity08
	constexpr std::uint32_t NICK_REF = 0x002F25;     // NickValentineREF
	constexpr std::uint32_t CAIT_REF = 0x079305;     // CaitRef
	constexpr std::uint32_t DISALLOWED = 0x075D56;   // DisallowedCompanionFaction

	// Nick's Long Time Coming: the quickstarts (SetCompanion with his affinity at 999, a MoveTo) are kEach, nothing of them
	// in a friend's game, and no driver: mirrored everywhere, a console quickstart never runs in another game. 0 and 1 are
	// the scan's DEBUG quickstarts too (kDebug: never shared at all), and so is 190. None is a start-up stage
	// (StartRule gives none), so a friend's first stage of it (10, no row) is set plain while the quest isn't running here.
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 0, 1, 190 }) {
		CHECK(FlagsOf(NICK, stage) == (kEach | kDebug));
		CHECK(EachOf(File::kFallout4, NICK, stage).empty());
	}
	CHECK(!DriverOf(File::kFallout4, NICK));
	CHECK(!StartRule(File::kFallout4, NICK, 0));
	CHECK(!Starting(StartRule(File::kFallout4, NICK, 0), Find(NICK, 10)));
	CHECK(!Starting(Find(NICK, 0), Find(NICK, 10)));
	CHECK(Judge(nullptr, { .live = true, .nearFriend = false, .followable = true }) == Verdict::kSet);
	// 210 (DisallowCompanion(Nick, True): Nick runs to Eddie's bunker, with its Eddie, triggers and raiders), 250 and 260 (his
	// factions for the fight, affinity events to the current companion) and 290-305 (AllowCompanion(Nick, False, False)) run
	// in both games: FollowersScript's DismissCompanion returns unless he is that game's companion, and AllowCompanion with
	// neither flag only takes him out of DisallowedCompanionFaction.
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 10, 200, 205, 210, 220, 230, 250, 260, 280, 290, 300, 305, 600 }) {
		CHECK(!IsEach(File::kFallout4, NICK, stage));
	}
	CHECK(FlagsOf(NICK, 220) == kMoves);  // as before: the bunker's controls

	// Cait's Benign Intervention: her chair (60, DisallowCompanion: would dismiss her from a friend she followed) and her
	// cure (255, AllowCompanion(Cait, True, False): would recruit her to a friend with no companion) are the owner's; the
	// rest (Vault 95's map marker, the scenes, the objectives) runs in both games.
	CHECK(FlagsOf(CAIT, 0) == (kMoves | kDebug));  // the quickstart moves the player (never shared: the scan's DEBUG); no companion call
	CHECK(FlagsOf(CAIT, 60) == kEach);
	CHECK(FlagsOf(CAIT, 255) == kEach);
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 10, 20, 30, 40, 50, 70, 80, 90, 100, 110, 355 }) {
		CHECK(!Find(CAIT, stage));
	}
	CHECK(EachOf(File::kFallout4, CAIT, 60).empty());  // her chair scene is on her alias: the owner's; no objective of its own
	const auto cure = EachOf(File::kFallout4, CAIT, 255);
	CHECK(cure.size() == 2);
	CHECK(cure[0].act == Act::kComplete && cure[0].value == 50);
	CHECK(cure[1].act == Act::kCompleteQuest);  // the stage has the complete flag, as MacCready's 255
	const auto& cait = *DriverOf(File::kFallout4, CAIT);
	CHECK(cait.companion == CAIT_REF && cait.faction == DISALLOWED && cait.from == 60 && cait.until == 255 && cait.moved == 0 && cait.metQuest == 0);
	const auto doneIn = [](std::vector<std::uint16_t> a_done) { return [a_done](std::uint16_t a_stage) { return std::ranges::find(a_done, a_stage) != a_done.end(); }; };
	CHECK(Marked(cait, doneIn({ 10, 60 }), { .inFaction = true }));
	CHECK(!Marked(cait, doneIn({ 10 }), { .inFaction = true }));  // disallowed by something else before her 60
	CHECK(!Marked(cait, doneIn({ 10, 60 }), {}));                  // a mirror's 60: not in the faction there
	CHECK(HeldByQuest(cait, doneIn({ 10, 60, 70, 110 }), { .inFaction = true }));  // in the chair: the book keeps her the owner's
	CHECK(!HeldByQuest(cait, doneIn({ 10, 60, 70, 110, 255 }), { .inFaction = true }));
	CHECK(!HeldByQuest(cait, doneIn({ 10, 60 }), {}));
	// Played through: the owner (Cait theirs) runs both with their scripts; the friend mirrors both and never had her
	// disallowed, so no AllowCompanion is owed to their copy of her.
	const std::vector<std::uint16_t> caitStages{ 10, 20, 60, 70, 80, 90, 100, 110, 255 };
	CompanionGame owner{ CAIT, {}, false, true };
	CompanionGame friendGame{ CAIT };
	for (const auto stage : caitStages) {
		owner.Arrives(stage);
		friendGame.Arrives(stage);
	}
	CHECK(owner.mirrored.empty());
	CHECK(!owner.inFaction && owner.ours);  // 255 ran: she followed them again
	CHECK((friendGame.mirrored == std::vector<std::uint16_t>{ 60, 255 }));
	CHECK(!friendGame.inFaction && !friendGame.ours);
	// The owner reloads a save made while she sat in the chair (60 done, her in the faction, following nobody): the catch-up's
	// 255 runs its script there, by the mark.
	CompanionGame chair{ CAIT, { 10, 20, 60, 70, 80 }, true, false };
	CHECK(chair.Drive() == Drive::kScripts);
	chair.Arrives(255);
	CHECK(chair.mirrored.empty() && chair.ours);
	// A friend who mirrored her 60 stays a mirror, as a friend of Curie's does.
	CompanionGame mirror{ CAIT, { 10, 20, 60 }, false, false };
	CHECK(mirror.Drive() == Drive::kMirror);

	// Danse's Blind Betrayal: no stage recruits or dismisses; its 255 (AllowCompanion(Danse, False, False), the Brotherhood's
	// ending, his X-01) runs in both games as before.
	CHECK(!DriverOf(File::kFallout4, DANSE));
	for (const auto& rule : RULES) {
		CHECK((rule.quest != DANSE || rule.file != 0 || !(rule.flags & kEach)));
	}

	// Nick after the Memory Den: 90 (AllowCompanion(Nick, True, True): ForceCompanion, Nick recruited to whoever runs it) is the
	// owner's, with no acts of its own; its 95 (the objective hidden) and 100 (allowed again with neither flag, his affinity
	// conditions, the objective, Stop) are plain: the story's MQ204 200 sets 95, and 100 through it, in every game.
	CHECK(FlagsOf(NICK_DEN, 90) == kEach);
	CHECK(EachOf(File::kFallout4, NICK_DEN, 90).empty());
	CHECK(!Find(NICK_DEN, 20) && !Find(NICK_DEN, 95) && !Find(NICK_DEN, 100));
	const auto& den = *DriverOf(File::kFallout4, NICK_DEN);
	CHECK(den.companion == NICK_REF && den.faction == 0 && den.ran[0] == 0 && den.moved == 0);  // no mark of its own: ours, driven or mirrored decides
	// The owner (Nick dismissed at the lounger by MQ202 12: not following) sets 90 by talking to him: driven here. A reload
	// of a save from before that, with 90 in the session, mirrors it (the known limit: nobody recruited; the plain 100 still
	// allows him there, and the owner recruits him by his dialogue).
	CompanionGame den1{ NICK_DEN, { 20 }, false, false };
	den1.OwnPlay(90);
	CHECK(den1.Drive() == Drive::kScripts && den1.ours);
	CompanionGame den2{ NICK_DEN, { 20 }, false, false };
	CHECK(den2.Drive() == Drive::kMirror);
	// A friend's game whose story (MQ204 200) ran 95 and 100 itself: nothing of 90's left to mirror or run (the quest stopped
	// there), and no act of the owner's touches it.
	CompanionGame denStory{ NICK_DEN, { 20, 95, 100 }, false, false };
	CHECK(denStory.Drive() == Drive::kMirror);

	// Nick after Long Time Coming (MS07cPostQuest) stays unshared: MS07c's 305, a plain shared stage, sets its 5 in every game
	// (SHARED_SETS_EACH, above), so a friend's game would drive it and the owner's 20 (COMNick 80: SetCompanion) would recruit
	// a second Nick there.
	CHECK(!DriverOf(File::kFallout4, NICK_AFTER));
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 5, 7, 10, 20, 50, 100 }) {
		CHECK(!Find(NICK_AFTER, stage));
	}
	CHECK(SetsAnother(File::kFallout4, NICK, 305));  // the setter, as -SetsOther lists it

	// Sheffield's Nuka-Cola (FFDiamondCity08 100): a payment with its affinity event, never a friend's; the quest is SAFE's.
	CHECK(FlagsOf(SHEFFIELD, 100) == kPays);
	CHECK(Judge(Find(SHEFFIELD, 100), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
	CHECK(QuestFilter::IsSafe(0, SHEFFIELD));

	// Left out: Dogmeat's quest (DENIED: the shared MQ105 410 runs his recruit in each game) and X6-88's own (ComX688: his
	// recruit, dismissal and affinity talks, each player's as every COM<Name> quest).
	for (const auto& rule : RULES) {
		CHECK((rule.file != 0 || (rule.quest != 0x06DFAF && rule.quest != 0x0BBD95)));
	}
	CHECK(!QuestFilter::IsSafe(0, 0x06DFAF) && !QuestFilter::IsSilent(0, 0x0BBD95) && !QuestFilter::IsSafe(0, 0x0BBD95));
}

TEST("stage rules: a companion's quest is driven by the game whose own scripts took it; its owner's stages are never marked done there without their script")
{
	using namespace StageRules;
	constexpr std::uint32_t CURIE = 0x16454E;
	constexpr std::uint32_t MACCREADY = 0x027411;
	CHECK(DriverOf(File::kFallout4, CURIE) && DriverOf(File::kFallout4, MACCREADY));
	CHECK(DriverOf(File::kFallout4, 0x1666C5));   // COMCaitQuest: Cait's, shared by its type, with kEach stages
	CHECK(!DriverOf(File::kFallout4, 0x0229FE));  // MS07c: its kEach stages are quickstarts, mirrored in every other game
	CHECK(!DriverOf(File::kFallout4, 0x0B9F9D));  // BoS302: no kEach stage
	for (const auto& driver : DRIVERS) {
		// A quest with owner's stages: one of the companions' own, or one with kEach rows of its own.
		CHECK((QuestFilter::IsCompanionQuest(driver.file, driver.quest) || std::ranges::any_of(RULES, [&](const Rule& a_rule) {
			return a_rule.file == driver.file && a_rule.quest == driver.quest && (a_rule.flags & kEach);
		})));
		CHECK(driver.file == static_cast<std::uint8_t>(File::kFallout4));  // QuestSync::HeldByQuests reads Fallout4.esm's ids as they are
		for (const auto stage : driver.ran) {
			if (stage) {
				CHECK(FlagsOf(driver.quest, stage) & kPays);  // a stage a friend's game never sets: its being done is the mark
			}
		}
		if (driver.faction) {
			CHECK(IsEach(File::kFallout4, driver.quest, driver.from));  // the owner's stage that puts them in it: never run in a mirror
			CHECK(IsEach(File::kFallout4, driver.quest, driver.until));
		}
		if (driver.moved) {
			CHECK(driver.alias != NO_ALIAS);
			CHECK(IsEach(File::kFallout4, driver.quest, driver.moved));  // the owner's stage whose script fills the alias: never run in a mirror
			CHECK((!driver.faction || (driver.from < driver.moved && driver.moved < driver.until)));  // one mark or the other the whole hold
		}
	}
	const auto& curieDriver = *DriverOf(File::kFallout4, CURIE);
	CHECK(curieDriver.moved == 300 && curieDriver.alias == 3);  // NewCurieBody: no fill of its own; 300's ForceRefTo(aTrueCurie) after her Reset
	// The new body's acts never touch that alias in a mirror (Curie's alias is 0, the robot body's 7).
	for (const auto& each : EACH) {
		CHECK((each.alias != curieDriver.alias || each.quest != CURIE));
		CHECK((each.items != curieDriver.alias || each.quest != CURIE));
	}

	// The rule itself.
	CHECK(DriveOf({ .marked = true }) == Drive::kScripts);
	CHECK(DriveOf({ .marked = true, .mirrored = true }) == Drive::kScripts);
	CHECK(DriveOf({ .marked = true, .ours = false }) == Drive::kScripts);
	CHECK(DriveOf({ .driven = true, .mirrored = true }) == Drive::kScripts);  // ran one of them with its script here: its mark may not be there yet
	CHECK(DriveOf({ .ours = true }) == Drive::kScripts);
	CHECK(DriveOf({}) == Drive::kMirror);
	CHECK(DriveOf({ .mirrored = true, .ours = true }) == Drive::kMirror);
	const auto doneIn = [](std::vector<std::uint16_t> a_done) { return [a_done](std::uint16_t a_stage) { return std::ranges::find(a_done, a_stage) != a_done.end(); }; };
	CHECK(Marked(curieDriver, doneIn({ 200 }), { .inFaction = true }));
	CHECK(!Marked(curieDriver, doneIn({ 100 }), { .inFaction = true }));    // in the faction before her 200: not the quest's
	CHECK(Marked(curieDriver, doneIn({ 200, 300 }), { .inAlias = true }));  // her reset took the faction; the alias holds
	CHECK(!Marked(curieDriver, doneIn({ 200, 300 }), {}));                  // a mirror: neither
	CHECK(!Marked(curieDriver, doneIn({ 200 }), { .inAlias = true }));      // the alias without its stage: not the mark
	CHECK(Marked(*DriverOf(File::kFallout4, MACCREADY), doneIn({ 40 }), {}));  // the hire

	// Curie, played through: the owner (Curie theirs) drives, the friend (never had her) mirrors.
	const std::vector<std::uint16_t> curieStages{ 100, 150, 200, 300, 400, 500, 600, 610, 700 };
	CompanionGame owner{ CURIE, {}, false, true };
	CompanionGame friendGame{ CURIE };
	for (const auto stage : curieStages) {
		owner.Arrives(stage);
		friendGame.Arrives(stage);
	}
	CHECK(owner.mirrored.empty());
	CHECK(!owner.inFaction && owner.ours);  // 600 ran: she is a companion again
	CHECK((friendGame.mirrored == std::vector<std::uint16_t>{ 200, 300, 400, 500, 600 }));
	CHECK(!friendGame.inFaction && !friendGame.inAlias);  // nothing of the owner's DisallowCompanion or new body's aliases here

	// The owner's game crashes after the 24-hour wait and they reload the autosave with 200 done (her in
	// DisallowedCompanionFaction, no alias holds her): the rejoin's catch-up re-sends every stage. Each runs its script here.
	CompanionGame reloaded{ CURIE, { 100, 150, 200 }, true, false };
	for (const auto stage : curieStages) {
		CHECK((reloaded.Done(stage) || !IsEach(File::kFallout4, CURIE, stage) || reloaded.Drive() == Drive::kScripts));
		reloaded.Arrives(stage);
	}
	CHECK(reloaded.mirrored.empty());
	CHECK(!reloaded.inFaction && reloaded.ours);  // 600's script ran: she can be recruited again, not lost for good

	// The same reload with the book already free of her (no alias, nobody's): the mark alone decides.
	CompanionGame reloadedAfterTimer{ CURIE, { 100, 150, 200, 300 }, true, false };
	CHECK(reloadedAfterTimer.Drive() == Drive::kScripts);

	// The owner's own timer runs her 300 (her reset takes her out of the faction), and they reload the autosave made on
	// entering the Memory Den for the operation, after it: the rejoin's catch-up re-sends 400, 500 and 600. The alias her
	// 300 filled is the mark now: each runs its script, 600's too.
	CompanionGame timer{ CURIE, {}, false, true };
	for (const auto stage : std::initializer_list<std::uint16_t>{ 100, 150, 200 }) {
		timer.Arrives(stage);
	}
	timer.OwnPlay(300);
	CHECK(!timer.inFaction && timer.inAlias);
	auto memoryDen = timer.Loaded();
	CHECK(memoryDen.Facts().marked && !memoryDen.Facts().driven && memoryDen.Facts().mirrored && !memoryDen.ours);
	CHECK(memoryDen.Drive() == Drive::kScripts);
	for (const auto stage : curieStages) {
		memoryDen.Arrives(stage);
	}
	CHECK(memoryDen.mirrored.empty());
	CHECK(!memoryDen.inFaction && memoryDen.ours);  // 600's RemoveFromFaction and COMCurie 50 ran: not lost for good
	// Held by her quest from 200 to 600 in that game, through her reset.
	CHECK(HeldByQuest(curieDriver, doneIn(timer.done), { timer.inFaction, timer.inAlias }));

	// The owner reloads a save from before her 200, Curie following (theirs), and the session has 200 and 300. 200 runs
	// with its script; it reads done at once, before its DisallowCompanion and ClearCompanion have run, and 300 is judged
	// then. It is driven here (200 ran with its script), not mirrored, and so are 400 to 600.
	CompanionGame early{ CURIE, { 100, 150 }, false, true };
	early.lazy = true;
	early.Arrives(200);
	CHECK(!early.inFaction);        // its script hasn't run yet: no mark
	CHECK(early.Facts().mirrored);  // 200 done here, as a mirror's would be
	CHECK(early.Drive() == Drive::kScripts);
	early.Arrives(300);
	early.RunScripts();
	for (const auto stage : curieStages) {
		early.Arrives(stage);
		early.RunScripts();
	}
	CHECK(early.mirrored.empty());
	CHECK(!early.inFaction && early.ours);
	// Without what was driven here (the same moment seen by the old rule) 300 would have been mirrored, and 600's
	// RemoveFromFaction would have found no NewCurieBody: Curie disallowed for good.
	CompanionGame racing{ CURIE, { 100, 150, 200 }, false, false };
	CHECK(racing.Drive() == Drive::kMirror);
	// This game's own play does the same: its own 200 (her dialogue) drives the quest at once.
	CompanionGame ownDialogue{ CURIE, { 100, 150 }, false, true };
	ownDialogue.lazy = true;
	ownDialogue.OwnPlay(200);
	CHECK(ownDialogue.Drive() == Drive::kScripts);
	// A load forgets what was driven: the save's marks decide (the scripts of a save made after them have run).
	ownDialogue.RunScripts();
	CHECK(ownDialogue.Loaded().Drive() == Drive::kScripts);

	// A friend whose own save ran her 200 earlier (her in the faction there): their game drives it too, and their Curie gets
	// her 600 with its script.
	CompanionGame ownSave{ CURIE, { 100, 150, 200 }, true, false };
	ownSave.Arrives(300);
	ownSave.Arrives(600);
	CHECK(ownSave.mirrored.empty());
	CHECK(!ownSave.inFaction);

	// A friend who mirrored 200 and recruited robot Curie once the owner's hold lapsed: still a mirror (the owner's 200 isn't
	// here for her 300's script to carry on from).
	CompanionGame recruitedLate{ CURIE };
	recruitedLate.Arrives(100);
	recruitedLate.Arrives(200);
	recruitedLate.ours = true;
	CHECK(recruitedLate.Drive() == Drive::kMirror);
	recruitedLate.Arrives(300);
	CHECK((recruitedLate.mirrored == std::vector<std::uint16_t>{ 200, 300 }));
	CHECK(recruitedLate.Loaded().Drive() == Drive::kMirror);

	// The driving game keeps her its player's from 200 to 600 (the book): held by her quest only with the mark.
	CHECK(!HeldByQuest(curieDriver, doneIn({ 100, 150 }), {}));
	CHECK(HeldByQuest(curieDriver, doneIn({ 100, 150, 200 }), { .inFaction = true }));
	CHECK(HeldByQuest(curieDriver, doneIn({ 100, 150, 200, 300, 400, 500 }), { .inFaction = true }));
	CHECK(HeldByQuest(curieDriver, doneIn({ 100, 150, 200, 300, 400, 500 }), { .inAlias = true }));  // her reset took the faction
	CHECK(!HeldByQuest(curieDriver, doneIn({ 100, 150, 200, 300, 400, 500, 600 }), { .inAlias = true }));
	CHECK(!HeldByQuest(curieDriver, doneIn({ 100, 150, 200, 300 }), {}));  // a mirror: 200 done, neither mark

	// MacCready: the owner hired him (30, a payment: never a friend's). Their game restarts without a save after its own 60
	// and 120, and the catch-up re-sends 120: its script runs (the caps given back, his affinity's condition).
	const std::vector<std::uint16_t> macStages{ 5, 30, 60, 80, 110, 120, 130, 140, 150, 160, 165, 170, 180, 190, 200, 255 };
	CompanionGame hirer{ MACCREADY, {}, false, true };
	CompanionGame macFriend{ MACCREADY };
	for (const auto stage : macStages) {
		hirer.Arrives(stage);
		macFriend.Arrives(stage);
	}
	CHECK(hirer.mirrored.empty());
	CHECK((macFriend.mirrored == std::vector<std::uint16_t>{ 120, 255 }));
	CHECK(!macFriend.Done(30));  // never paid here
	CompanionGame restarted{ MACCREADY, { 5, 30, 60 }, false, false };  // the save from before 120, him not following yet
	restarted.Arrives(120);
	restarted.Arrives(255);
	CHECK(restarted.mirrored.empty());
	// A friend whose own save hired him earlier: their game drives his quest.
	CompanionGame ownHire{ MACCREADY, { 5, 40 }, false, false };
	ownHire.Arrives(120);
	CHECK(ownHire.mirrored.empty());
}

TEST("stage rules: a companion quest that doesn't start here is sent startquest for a while, then given up on")
{
	using namespace StageRules;
	using namespace std::chrono_literals;
	CHECK(!GiveUpStart(0, 0s));
	CHECK(!GiveUpStart(EACH_START_TRIES - 1, 10min));  // tried too few times: a paused game's one poll is not a give-up
	CHECK(!GiveUpStart(100, EACH_START_FOR - 1s));     // too soon: the poll is twice a second
	CHECK(GiveUpStart(EACH_START_TRIES, EACH_START_FOR));
}

TEST("stage rules: the stages that carry globals (Nuka-World's gang choice) are sorted, once each, with the globals DLC04MakeEnemy, GangSwitch and DLC04GangSetup read (tools/questscan.ps1 -Globals)")
{
	using namespace StageRules;
	for (auto it = std::next(std::begin(CARRIES)); it != std::end(CARRIES); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } < std::tuple{ it->file, it->quest, it->stage }));
	}
	for (const auto& row : CARRIES) {
		const auto globals = GlobalsOf(row);
		CHECK(!globals.empty());
		CHECK(globals.size() <= MAX_CARRIED);
		CHECK(globals.size() <= Protocol::MAX_STAGE_GLOBALS);
		for (const auto global : globals) {
			CHECK(global != 0);
			CHECK(global <= 0x00FFFFFF);  // in the quest's file
		}
		for (std::size_t i = globals.size(); i < row.globals.size(); ++i) {
			CHECK(row.globals[i] == 0);  // the unused slots after the used ones
		}
		// Every row of Power Play is the one that carries a value a script of the stage reads (the ranks, the enemy gang
		// and the parks' owners): each a stage with a StageRules row too, or of the quest shared by its type.
		CHECK(row.file == static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK((row.needsQuest == 0) == (row.quest == 0x000804));  // MQ05's wait for MQ04 500; MQ04 500 itself needs nothing
	}
	constexpr std::uint32_t RANK_PACK = 0x00880C, RANK_DISCIPLES = 0x00880D, RANK_OPERATORS = 0x00880E, ENEMY_GANG = 0x00D370;
	// Home Sweet Home's end sets Power Play's 5, whose DLC04MakeEnemy reads the ranks: the ranks go with both.
	const auto home = FindCarried(File::kNukaWorld, 0x000804, 500);
	const auto make = FindCarried(File::kNukaWorld, 0x000805, 5);
	REQUIRE(home);
	REQUIRE(make);
	CHECK((GlobalsOf(*home).size() == 3 && GlobalsOf(*home)[0] == RANK_PACK && GlobalsOf(*home)[1] == RANK_DISCIPLES && GlobalsOf(*home)[2] == RANK_OPERATORS));
	CHECK(std::ranges::equal(GlobalsOf(*home), GlobalsOf(*make)));
	CHECK(make->needsQuest == 0x000804 && make->needsStage == 500);
	// 8: the enemy gang and the five parks' owners (GangSwitch, DLC04GangSetup, ZoneReset).
	const auto setup = FindCarried(File::kNukaWorld, 0x000805, 8);
	REQUIRE(setup);
	CHECK(GlobalsOf(*setup).size() == 6);
	CHECK(GlobalsOf(*setup)[0] == ENEMY_GANG);
	for (const auto claimed : { 0x03E418u, 0x03E419u, 0x03E41Au, 0x03E41Bu, 0x03E41Cu }) {
		CHECK(std::ranges::contains(GlobalsOf(*setup), claimed));
	}
	// 20, 120 and 255 read the enemy gang alone.
	for (const auto stage : { 20, 120, 255 }) {
		const auto row = FindCarried(File::kNukaWorld, 0x000805, static_cast<std::uint16_t>(stage));
		REQUIRE(row);
		CHECK(GlobalsOf(*row).size() == 1);
		CHECK(GlobalsOf(*row)[0] == ENEMY_GANG);
	}
	// The quickstarts (0-3) set a rank themselves and are kMoves | kDebug rows; 10 reads none; DLC04PostQuestFlags isn't shared.
	for (const auto stage : { 0, 1, 2, 3, 10, 70, 75 }) {
		CHECK(!FindCarried(File::kNukaWorld, 0x000805, static_cast<std::uint16_t>(stage)));
	}
	CHECK(!FindCarried(File::kNukaWorld, 0x04690A, 10));
	CHECK(!FindCarried(File::kFallout4, 0x000805, 8));  // the same form ID in another file is another quest's
}

TEST("stage rules: a friend's stage is set with its globals written first, in its row's order; waits for the stage its row needs; and is not set when one it reads didn't come")
{
	using namespace StageRules;

	constexpr std::uint32_t FILE = 0x01000000;  // DLCNukaWorld.esm loaded at 01
	const auto setup = FindCarried(File::kNukaWorld, 0x000805, 8);
	const auto home = FindCarried(File::kNukaWorld, 0x000804, 500);
	REQUIRE(setup && home);
	// What came with Power Play's 8, in any order, with one more no row names.
	const std::vector<GlobalValue> came{ { 0x0103E41C, 2.0f }, { 0x0100D370, 3.0f }, { 0x0103E418, 1.0f }, { 0x0103E41A, 2.0f }, { 0x0100F446, 9.0f }, { 0x0103E419, 3.0f },
		{ 0x0103E41B, 1.0f } };
	const auto plan = PlanCarry(setup, came, FILE | 0x000805, true);
	CHECK(plan.what == Carry::Do::kSet);
	CHECK(plan.dropped == 1);  // DLC04OpKills: not the row's
	// Written in the row's order, with the full form IDs.
	CHECK((plan.writes == std::vector<GlobalValue>{ { 0x0100D370, 3.0f }, { 0x0103E418, 1.0f }, { 0x0103E419, 3.0f }, { 0x0103E41A, 2.0f }, { 0x0103E41B, 1.0f }, { 0x0103E41C, 2.0f } }));

	// The order it happens in: every write, then the stage, nothing else.
	std::vector<std::string> did;
	DoCarry(
		plan, [&](std::uint32_t a_global, float a_value) { did.push_back(std::format("{:08X}={}", a_global, a_value)); }, [&]() { did.push_back("setstage"); });
	CHECK((did == std::vector<std::string>{ "0100D370=3", "0103E418=1", "0103E419=3", "0103E41A=2", "0103E41B=1", "0103E41C=2", "setstage" }));

	// Home Sweet Home's end isn't done here: Power Play's stages wait (its quest alone), judged again later; its own row
	// needs nothing. CarryWaits says so before the stage is judged (no follow offer for a stage that then waits), and
	// PlanCarry agrees.
	CHECK(CarryWaits(*setup, false));
	CHECK(!CarryWaits(*setup, true));
	CHECK(!CarryWaits(*home, false));
	CHECK(!CarryWaits(*home, true));
	for (const auto& row : CARRIES) {
		CHECK(CarryWaits(row, false) == (row.needsQuest != 0));
		CHECK(CarryWaits(row, false) == (PlanCarry(&row, {}, FILE | row.quest, false).what == Carry::Do::kWait));
	}
	const auto waits = PlanCarry(setup, came, FILE | 0x000805, false);
	CHECK(waits.what == Carry::Do::kWait);
	CHECK(waits.writes.empty());
	CHECK(waits.why != nullptr);
	did.clear();
	DoCarry(
		waits, [&](std::uint32_t, float) { did.push_back("write"); }, [&]() { did.push_back("setstage"); });
	CHECK(did.empty());
	const std::vector<GlobalValue> ranks{ { 0x0100880C, 1.0f }, { 0x0100880D, 3.0f }, { 0x0100880E, 2.0f } };
	CHECK(PlanCarry(home, ranks, FILE | 0x000804, false).what == Carry::Do::kSet);
	CHECK(PlanCarry(home, ranks, FILE | 0x000804, false).writes.size() == 3);

	// A value the row names didn't come (an older session file's stage, or none at all): not set, nothing written.
	std::vector<GlobalValue> short_ = came;
	std::erase_if(short_, [](const GlobalValue& a_value) { return a_value.global == 0x0103E419; });
	const auto skip = PlanCarry(setup, short_, FILE | 0x000805, true);
	CHECK(skip.what == Carry::Do::kSkip);
	CHECK(skip.writes.empty());
	CHECK(skip.why != nullptr);
	CHECK(PlanCarry(setup, {}, FILE | 0x000805, true).what == Carry::Do::kSkip);
	did.clear();
	DoCarry(
		skip, [&](std::uint32_t, float) { did.push_back("write"); }, [&]() { did.push_back("setstage"); });
	CHECK(did.empty());
	// A value of another file's global with the same local ID is not the row's.
	std::vector<GlobalValue> other = came;
	for (auto& value : other) {
		value.global = (value.global & 0x00FFFFFF) | 0x02000000;
	}
	CHECK(PlanCarry(setup, other, FILE | 0x000805, true).what == Carry::Do::kSkip);

	// No row: set as it is, whatever came is dropped (a sender's row this build lacks), and nothing is written.
	const auto none = PlanCarry(nullptr, came, FILE | 0x000805, false);
	CHECK(none.what == Carry::Do::kSet);
	CHECK(none.writes.empty());
	CHECK(none.dropped == came.size());
	const auto bare = PlanCarry(nullptr, {}, FILE | 0x000805, false);
	CHECK(bare.what == Carry::Do::kSet);
	CHECK(bare.dropped == 0);
	did.clear();
	DoCarry(
		none, [&](std::uint32_t, float) { did.push_back("write"); }, [&]() { did.push_back("setstage"); });
	CHECK((did == std::vector<std::string>{ "setstage" }));
}

TEST("stage rules: Power Play's 8 is held for, and its landing told by, the one gang the carried DLC04EnemyGang names (1 the Disciples, 2 the Operators, 3 the Pack); all three only when no such value came")
{
	using namespace StageRules;

	constexpr std::uint32_t FILE = 0x01000000;  // DLCNukaWorld.esm loaded at 01
	constexpr std::uint32_t QUEST = 0x000805, ENEMY_GANG = 0x00D370;
	constexpr std::uint32_t DISCIPLES = 0x00F438, OPERATORS = 0x00F439, PACK = 0x00F43A;
	const auto factions = [](const std::vector<Hostile>& a_rows) {
		std::vector<std::uint32_t> ids;
		for (const auto& row : a_rows) {
			ids.push_back(row.faction);
		}
		return ids;
	};
	// The stage's rows as QuestSync has them, with what came with it: the enemy gang and the parks' owners.
	const auto heldAt = [&](std::vector<GlobalValue> a_came, std::uint32_t a_file) { return factions(HostileWith(HostileRows(File::kNukaWorld, QUEST, 8, {}), a_came, a_file | QUEST)); };
	const auto held = [&](std::vector<GlobalValue> a_came) { return heldAt(std::move(a_came), FILE); };
	const auto with = [](float a_gang) {
		return std::vector<GlobalValue>{ { 0x0103E41C, 2.0f }, { 0x0100D370, a_gang }, { 0x0103E418, 1.0f }, { 0x0103E419, 3.0f }, { 0x0103E41A, 2.0f }, { 0x0103E41B, 1.0f } };
	};
	// The gang the chooser's game made the enemy, alone: the hold counts only its members, and the first row (the one the
	// log's landing note names when nothing was waited for) is that gang's.
	CHECK((held(with(1.0f)) == std::vector<std::uint32_t>{ DISCIPLES }));
	CHECK((held(with(2.0f)) == std::vector<std::uint32_t>{ OPERATORS }));
	CHECK((held(with(3.0f)) == std::vector<std::uint32_t>{ PACK }));
	// The value alone, as its 20's row carries it, picks the same way.
	CHECK((held({ { 0x0100D370, 3.0f } }) == std::vector<std::uint32_t>{ PACK }));
	// No value (nothing came with the stage: an older session file's), or one that names no gang (0 before DLC04MakeEnemy
	// ran, anything else): all three, as before.
	const std::vector<std::uint32_t> all{ DISCIPLES, OPERATORS, PACK };
	CHECK(held({}) == all);
	CHECK(held(with(0.0f)) == all);
	CHECK(held(with(4.0f)) == all);
	CHECK(held(with(-1.0f)) == all);
	CHECK(held(with(1.5f)) == all);
	std::vector<GlobalValue> parks = with(2.0f);
	std::erase_if(parks, [](const GlobalValue& a_value) { return a_value.global == 0x0100D370; });
	CHECK(held(parks) == all);  // the parks' owners without the gang
	// A global of another file with the same local ID is not the stage's; nor is the file the quest is loaded at another's.
	CHECK((held({ { 0x0200D370, 2.0f } }) == all));
	CHECK(heldAt(with(2.0f), 0x07000000) == all);
	CHECK((heldAt({ { 0x0700D370, 2.0f } }, 0x07000000) == std::vector<std::uint32_t>{ OPERATORS }));
	// Another global with a gang's number is not the enemy gang (a park's owner 1 with the enemy gang 3).
	CHECK((held({ { 0x0103E418, 1.0f }, { 0x0100D370, 3.0f } }) == std::vector<std::uint32_t>{ PACK }));

	// A stage no row here names keeps its rows whatever came with it; so do rows of other stages judged with one that is
	// named (a start-up stage's, merged in by HostileRows), and no rows stay none.
	const auto kiddie = HostileRows(File::kNukaWorld, 0x03684F, 740, {});
	REQUIRE(kiddie.size() == 1);
	CHECK(HostileWith(kiddie, with(2.0f), FILE | 0x03684F).size() == 1);
	auto mixed = HostileRows(File::kNukaWorld, QUEST, 8, {});
	mixed.insert(mixed.begin(), kiddie.front());
	CHECK((factions(HostileWith(mixed, with(2.0f), FILE | QUEST)) == std::vector<std::uint32_t>{ kiddie.front().faction, OPERATORS }));
	CHECK(HostileWith(mixed, {}, FILE | QUEST).size() == 4);
	CHECK(HostileWith({}, with(2.0f), FILE | QUEST).empty());

	// The table: each row is a HOSTILE row of its stage, its global one the stage carries (so the value is there before the
	// stage is judged), and no faction is picked by two values of a global or a value picks two factions (one gang turns).
	static_assert(std::size(HOSTILE_WHEN) == 3);
	for (const auto& when : HOSTILE_WHEN) {
		const auto rows = HostileOf(static_cast<File>(when.file), when.quest, when.stage);
		CHECK(std::ranges::any_of(rows, [&](const Hostile& a_row) { return a_row.factionFile == when.factionFile && a_row.faction == when.faction; }));
		const auto carried = FindCarried(static_cast<File>(when.file), when.quest, when.stage);
		REQUIRE(carried);
		CHECK(std::ranges::contains(GlobalsOf(*carried), when.global));
		CHECK(std::ranges::count_if(HOSTILE_WHEN, [&](const HostileWhen& a_other) {
			return a_other.file == when.file && a_other.quest == when.quest && a_other.stage == when.stage && a_other.global == when.global && a_other.value == when.value;
		}) == 1);
		CHECK(std::ranges::count_if(HOSTILE_WHEN, [&](const HostileWhen& a_other) {
			return a_other.file == when.file && a_other.quest == when.quest && a_other.stage == when.stage && a_other.factionFile == when.factionFile && a_other.faction == when.faction;
		}) == 1);
	}
	// Every row of Power Play's 8 is named: a gang the script can pick with no value that picks it would always be held for.
	for (const auto& row : HostileOf(File::kNukaWorld, QUEST, 8)) {
		CHECK(std::ranges::any_of(HOSTILE_WHEN, [&](const HostileWhen& a_when) { return a_when.stage == row.stage && a_when.quest == row.quest && a_when.faction == row.faction; }));
	}
	CHECK(HOSTILE_WHEN[0].global == ENEMY_GANG);
}

TEST("stage rules: the teleport quests shared with sQuests = safe move nobody without the follow offer, RRAct3Pickup's quickstarts nobody at all (InstituteTeleportEffects, DN136_Attack; MQ302BoS 300, MQ302RR 330; the charge planted: MQ302 600, MQ302BoS 600, MQ302Min 1600, MQ302RR 600)")
{
	using namespace StageRules;
	// By hand (the scan sees only the player's own moves): the Institute's teleporter reached through MQ206Min/BoS 1500, a
	// companion or Dogmeat teleported along, X6-88 moved to the relay.
	CHECK(FlagsOf(0x185E58, 1) == (kMoves | kDebug));  // RRAct3Pickup: a quickstart ("Debug: Supported MM") into the Institute by MQ206Min 1500
	CHECK(FlagsOf(0x185E58, 2) == (kMoves | kDebug));  // ... ("Debug: Supported BoS") by MQ206BoS 1500: never shared, nor offered
	CHECK(FlagsOf(0x1ED4FA, 10) == kMoves);   // InstituteTeleportEffects: X6-88 to the relay beside the player
	CHECK(FlagsOf(0x121D0C, 10) == kMoves);   // DN136_Attack: the companion and Dogmeat in with the Brotherhood
	CHECK(FlagsOf(0x10C64B, 300) == kMoves);  // MQ302BoS: sets DN136_Attack 10
	CHECK(FlagsOf(0x10C64C, 330) == kMoves);  // MQ302RR: MQ302RRScript.TeleportFollowers
	// The pulse charge planted (the scan doesn't follow a scene's Start into the scene's fragments): MQ302 600 sets the
	// faction's stage in every game, and each starts its charge-planted scene, whose end teleports the companion and
	// Dogmeat into the Institute and moves the player to the relay marker (MQ302BoS_Maxson_ChargePlanted 00112E46,
	// MQ302Min_Preston_ChargePlanted 0011386A, MQ302RR_Desdemona_ChargePlanted 00113870: Fragment_End).
	CHECK(FlagsOf(0x0229EE, 600) == kMoves);   // MQ302: sets MQ302BoS 600, MQ302Min 1600 or MQ302RR 600
	CHECK(FlagsOf(0x10C64B, 600) == kMoves);   // MQ302BoS: the scene
	CHECK(FlagsOf(0x10C64A, 1600) == kMoves);  // MQ302Min
	CHECK(FlagsOf(0x10C64C, 600) == kMoves);   // MQ302RR
	CHECK(SetsAnother(File::kFallout4, 0x0229EE, 600));  // the faction's stage waits its turn
	// None a bridge: what they set of the story (MQ206Min 1500, MQ302 350, the faction's 600) reaches a friend as the
	// story's own. So a far friend is offered to go with the chooser, and nobody is moved without taking it up.
	for (const auto& [quest, stage] : { std::pair{ 0x1ED4FAu, 10 }, std::pair{ 0x121D0Cu, 10 },
			 std::pair{ 0x10C64Bu, 300 }, std::pair{ 0x10C64Cu, 330 }, std::pair{ 0x0229EEu, 600 }, std::pair{ 0x10C64Bu, 600 }, std::pair{ 0x10C64Au, 1600 },
			 std::pair{ 0x10C64Cu, 600 } }) {
		const auto rule = Find(quest, static_cast<std::uint16_t>(stage));
		REQUIRE(rule);
		CHECK(!(rule->flags & kBridge));
		CHECK(Judge(rule, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSet);                                   // beside the friend who did it
		CHECK(Judge(rule, { .sharedStory = true, .live = true, .followable = true }) == Verdict::kOffer);                                 // far: the follow offer
		CHECK(Judge(rule, { .sharedStory = true, .live = true, .followable = true, .offered = 10s }) == Verdict::kOffer);                 // still open
		CHECK(Judge(rule, { .sharedStory = true, .live = false, .nearFriend = true, .followable = true, .offered = 60s }) == Verdict::kSet);  // taken up: beside them
		CHECK(Judge(rule, { .sharedStory = true, .live = false, .followable = true, .offered = OFFER_FOR }) == Verdict::kSkip);           // not taken up: skipped, never moved
		CHECK(Judge(rule, { .sharedStory = true, .live = true }) == Verdict::kSkip);                                                      // far, nobody known to follow
		CHECK(Judge(rule, { .sharedStory = true, .live = false }) == Verdict::kSkip);                                                     // caught up from the session
	}
	// The teleporter's script waits as MQ206 1000's does (Wait 4, and a second more).
	// RRAct3Pickup's two are the developers' quickstarts: skipped beside the friend too, never offered.
	for (const std::uint16_t stage : { std::uint16_t{ 1 }, std::uint16_t{ 2 } }) {
		CHECK(Judge(Find(0x185E58, stage), { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSkip);
		CHECK(Judge(Find(0x185E58, stage), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kSkip);
	}
	CHECK(Hold(Find(0x185E58, 1)) == 5s);
	CHECK(Hold(Find(0x185E58, 2)) == 5s);
	// The charge-planted scene's lines, its end's 5 s and the move: what comes after the faction's stage waits that long.
	CHECK(Hold(Find(0x10C64B, 600)) == 21s);
	CHECK(Hold(Find(0x10C64A, 1600)) == 21s);
	CHECK(Hold(Find(0x10C64C, 600)) == 21s);
	// DNPrime_BoS304 moves Prime and the synths, MQ302Post the faction leaders home: nothing of the player's (waits only,
	// or no row; MQ302Post 10 sets the ending's Institute kick-out, MQ302Min 2000, here: HOSTILE's, as that stage is).
	for (const auto& rule : RULES) {
		if (rule.file == 0 && (rule.quest == 0x176E24 || rule.quest == 0x215CC7)) {
			CHECK(rule.flags == 0);
		}
	}
	CHECK(Find(0x176E24, 25) && Find(0x176E24, 25)->wait == 25);
	CHECK(Find(0x176E24, 505) && Find(0x176E24, 505)->wait == 150);
	CHECK(!Find(0x215CC7, 10));
	REQUIRE(HostileOf(File::kFallout4, 0x215CC7, 10).size() == 1);
	CHECK(HostileOf(File::kFallout4, 0x215CC7, 10).front().faction == HostileOf(File::kFallout4, 0x10C64A, 2000).front().faction);  // the Institute
	CHECK(!Find(0x15DE65, 25));  // FFGoodneighbor08: its timer's stages are HOST_TIMERS', no rule
	// What they set of other quests waits its turn (SETS_ANOTHER, tools/questscan.ps1 -Safe -SetsOther).
	CHECK(SetsAnother(File::kFallout4, 0x185E58, 1));     // MQ206Min 1500
	CHECK(SetsAnother(File::kFallout4, 0x185E58, 200));   // RR201 100
	CHECK(!SetsAnother(File::kFallout4, 0x185E58, 300));  // Stop
	CHECK(SetsAnother(File::kFallout4, 0x121D0C, 350));   // MQ302 425
	CHECK(SetsAnother(File::kFallout4, 0x176E24, 500));   // BoS304 60
	CHECK(SetsAnother(File::kFallout4, 0x176E24, 1000));  // BoS304VB 255
	CHECK(SetsAnother(File::kFallout4, 0x215CC7, 10));    // MQ302Min 2000
}

TEST("stage rules: a timer's stages are the host's (HOST_TIMERS: FFGoodneighbor08's 25, 200 and 300, and its shutdown stage 1000, which every game takes)")
{
	using namespace StageRules;
	for (auto it = std::next(std::begin(HOST_TIMERS)); it != std::end(HOST_TIMERS); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } < std::tuple{ it->file, it->quest, it->stage }));
	}
	const auto start = FindTimer(File::kFallout4, 0x15DE65, 25);
	const auto end = FindTimer(File::kFallout4, 0x15DE65, 200);
	const auto stop = FindTimer(File::kFallout4, 0x15DE65, 300);
	const auto shutdown = FindTimer(File::kFallout4, 0x15DE65, 1000);
	REQUIRE(start && end && stop && shutdown);
	CHECK(!FindTimer(File::kFallout4, 0x15DE65, 10));  // Sammy at the gate: any game's
	CHECK(!FindTimer(File::kFallout4, 0x15DE65, 20));  // its scene
	CHECK(!FindTimer(File::kCoast, 0x15DE65, 25));     // a form ID is the quest's only in its own file
	// Only the host's game reports them, the shutdown stage too (a friend's own 300 runs it, and reported it would put
	// the guards back in the host's game while the host's event still runs); every game reports the rest.
	CHECK(ReportsTimer(true, start));
	CHECK(ReportsTimer(true, stop));
	CHECK(ReportsTimer(true, shutdown));
	CHECK(!ReportsTimer(false, start));
	CHECK(!ReportsTimer(false, end));
	CHECK(!ReportsTimer(false, stop));
	CHECK(!ReportsTimer(false, shutdown));
	CHECK(ReportsTimer(false, nullptr));
	CHECK(ReportsTimer(true, nullptr));
	// No game sets a friend's start, end or stop (the host's timer runs in the host's game alone); the host's shutdown
	// stage, the event's end for the session, every game takes.
	CHECK(!start->taken && !end->taken && !stop->taken);
	CHECK(shutdown->taken);
	CHECK(!TakesTimer(start));
	CHECK(!TakesTimer(end));
	CHECK(!TakesTimer(stop));
	CHECK(TakesTimer(shutdown));
	CHECK(TakesTimer(nullptr));
	// A timer's stage has no other rule, and its quest is a shared one (QuestFilterTests: SILENT).
	for (const auto& timer : HOST_TIMERS) {
		CHECK(!Find(timer.quest, timer.stage, static_cast<File>(timer.file)));
		CHECK(QuestFilter::IsSilent(timer.file, timer.quest));
	}
}

TEST("stage rules: a game whose scene never set its timer's start sets it itself, whoever set the scene's stage (TIMER_STARTS)")
{
	using namespace StageRules;
	using namespace std::chrono_literals;
	const auto kick = FindTimerStart(File::kFallout4, 0x15DE65, 20);  // FFGoodneighbor08: Sammy's scene sets 25
	REQUIRE(kick);
	CHECK(kick->sets == 25);
	CHECK(kick->within == 30s);
	CHECK(kick->ends == 1000);
	CHECK(!FindTimerStart(File::kFallout4, 0x15DE65, 10));
	CHECK(!FindTimerStart(File::kFallout4, 0x15DE65, 25));
	CHECK(!FindTimerStart(File::kCoast, 0x15DE65, 20));
	// What it sets is a timer's start of HOST_TIMERS, which no game takes from a friend and only the host's game reports:
	// a game that sets it itself starts its own timer, and a friend's ends only its own event.
	for (const auto& row : TIMER_STARTS) {
		const auto timer = FindTimer(static_cast<File>(row.file), row.quest, row.sets);
		REQUIRE(timer);
		CHECK(!TakesTimer(timer));
		CHECK(ReportsTimer(true, timer));
		CHECK(!ReportsTimer(false, timer));
		CHECK(!FindTimer(static_cast<File>(row.file), row.quest, row.after));
		const auto ends = FindTimer(static_cast<File>(row.file), row.quest, row.ends);  // the shutdown stage every game takes
		REQUIRE(ends);
		CHECK(ends->taken);
	}
	// Running, 20 done, no 25 after the scene's time in play: set (the host's game or a friend's, however 20 came).
	CHECK(TimerKick(*kick, true, true, false, false, 30s));
	CHECK(TimerKick(*kick, true, true, false, false, 5min));
	// 25 done (the scene played here, or this game set it already): nothing to do.
	CHECK(!TimerKick(*kick, true, true, true, false, 5min));
	// The scene still has time.
	CHECK(!TimerKick(*kick, true, true, false, false, 29s));
	CHECK(!TimerKick(*kick, true, true, false, false, 0s));
	// 20 not done: no scene was to set it.
	CHECK(!TimerKick(*kick, true, false, false, false, 5min));
	// The quest doesn't run (stopped by its own 300, or never started here): a stage set now would start it again.
	CHECK(!TimerKick(*kick, false, true, false, false, 5min));
	CHECK(!TimerKick(*kick, false, false, false, false, 5min));
	// The event is over here (1000 done: a joiner who caught up the host's 10, 20 and 1000): no timer of its own.
	CHECK(!TimerKick(*kick, true, true, false, true, 5min));
}
