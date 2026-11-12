#include "Test.h"

#include "game/QuestFilter.h"
#include "game/StageRules.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <tuple>
#include <utility>

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
	// are never shared (kDebug); DLC01MQ05's 1 sets none a friend's game would be sent, a plain move.
	CHECK(FlagsOf(0x002833, 2, File::kRobot) == (kMoves | kDebug));  // DLC01MQ04
	CHECK(FlagsOf(0x0010F5, 1, File::kRobot) == kMoves);              // DLC01MQ05
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
	CHECK(!StartRule(File::kNukaWorld, 0x000801));
	CHECK(!Find(0x000807, 0, File::kNukaWorld));  // DLC04MS02
	CHECK(!Find(0x000806, 0, File::kRobot));      // DLC01MQ01
	CHECK(!StageRules::Starting(StartRule(File::kRobot, 0x000806), Find(0x000806, 100, File::kRobot)));
	CHECK(StageRules::Starting(StartRule(File::kNukaWorld, 0x000801), door)->flags == kMoves);

	// Theirs that set another shared quest's stage (-SetsOther): DLC04MQ00's 520 starts DLC04MQ01, DLC01MQ01's 3000 DLC01MQ02.
	CHECK(StageRules::SetsAnother(File::kNukaWorld, 0x000800, 520));
	CHECK(StageRules::SetsAnother(File::kNukaWorld, 0x000801, 1000));  // DLC04MQ02 1
	CHECK(StageRules::SetsAnother(File::kRobot, 0x000806, 3000));
	CHECK(!StageRules::SetsAnother(File::kRobot, 0x000800, 520));
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

TEST("stage rules: a friend's stage that starts its quest here is judged and held for the quest's start-up stage too")
{
	using StageRules::Starting;
	CHECK(!Starting(nullptr, nullptr));
	// MQ105's 10 does nothing to the player; starting MQ105 with it runs its 0 first, which moves them.
	const auto clue = Starting(Find(0x0229E6, 0), Find(0x0229E6, 10));
	REQUIRE(clue);
	CHECK(clue->flags == (StageRules::kMoves | StageRules::kBridge));
	CHECK(StageRules::Hold(&*clue) == StageRules::Hold(Find(0x0229E6, 0)));
	StageRules::Situation now;
	now.sharedStory = true;
	CHECK(StageRules::Judge(&*clue, now) == Verdict::kWarn);
	// MS01's 1050 waits 5 s; its 0 moves the player (no bridge) and waits 2 s: from the session it isn't
	// set, but it is with the friend who just did it.
	const auto ms01 = Starting(StageRules::StartRule(StageRules::File::kFallout4, 0x0229F6), Find(0x0229F6, 1050));
	REQUIRE(ms01);
	CHECK(ms01->flags == StageRules::kMoves);
	CHECK(ms01->wait == 50);
	CHECK(StageRules::Judge(&*ms01, {}) == Verdict::kSkip);
	CHECK(StageRules::Judge(&*ms01, { .live = true, .nearFriend = true }) == Verdict::kSet);
	// MS07a's 0 moved the player too, and was judged so with its 17 once; it is a developer's quickstart ("DEBUG -
	// Companion Interjection Quickstart", no start-up stage), which starting the quest never runs: its 17 is judged alone.
	CHECK(StageRules::IsDebug(StageRules::File::kFallout4, 0x01CB51, 0));
	CHECK(!StageRules::StartRule(StageRules::File::kFallout4, 0x01CB51));
	const auto ms07a = Starting(StageRules::StartRule(StageRules::File::kFallout4, 0x01CB51), Find(0x01CB51, 17));
	REQUIRE(ms07a);
	CHECK(ms07a->flags == 0);
	CHECK(StageRules::Judge(&*ms07a, {}) == Verdict::kSet);
	// A quest whose start-up stage does nothing: the stage's own rule.
	const auto vault = Starting(nullptr, Find(0x0B8464, 400));
	REQUIRE(vault);
	CHECK(StageRules::Judge(&*vault, { .live = true }) == Verdict::kSkip);  // Vault 81's 400 takes the player's fusion cores
	CHECK((FlagsOf(0x0B8464, 400) & StageRules::kPays) != 0);
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
	CHECK(!StartRule(File::kFallout4, 0x0229EA));
	CHECK(!StartRule(File::kFallout4, 0x06B500));
	CHECK(!Starting(StartRule(File::kFallout4, 0x0229EA), Find(0x0229EA, 20)));
	CHECK(!Starting(StartRule(File::kFallout4, 0x06B500), Find(0x06B500, 10)));
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
		CHECK(!StartRule(File::kFallout4, quest));
	}
	CHECK(!StartRule(File::kNukaWorld, 0x000801));
	// Any other start-up stage's row is the start rule as before; a quest with no such row has none.
	CHECK(StartRule(File::kFallout4, 0x0229E6) == Find(0x0229E6, 0));  // MQ105 (its 0 is "TESTING ONLY" too, but RepairStory judges by it: unmarked)
	CHECK(StartRule(File::kFallout4, 0x0229F6) == Find(0x0229F6, 0));  // MS01
	CHECK(StartRule(File::kFallout4, 0x0C8675) == Find(0x0C8675, 0));  // BoSR03
	CHECK(!StartRule(File::kFallout4, 0x06D646));                      // DN084: its quickstarts are its 1 to 9
	CHECK(!StartRule(File::kCoast, 0x174029));

	// The airport battle's 10, which play sets to start it (Inst307's 30), and its 15: no row, so set like any stage.
	const Situation live{ .sharedStory = true, .live = true };
	CHECK(!Find(0x174029, 10));
	CHECK(!Starting(StartRule(File::kFallout4, 0x174029), Find(0x174029, 10)));
	CHECK(Judge(nullptr, live) == Verdict::kSet);
	// Judged with the quickstart's row, as it was once, it was skipped: no friend's game ever started the battle.
	const auto asQuickstart = Starting(Find(0x174029, 0), Find(0x174029, 10));
	REQUIRE(asQuickstart);
	CHECK(Judge(&*asQuickstart, live) == Verdict::kSkip);
	// BoS303's 40, which has a row of its own: judged by that row alone.
	const auto brotherhood = Starting(StartRule(File::kFallout4, 0x0FDC8C), Find(0x0FDC8C, 40));
	REQUIRE(brotherhood);
	CHECK(!(brotherhood->flags & kDebug));
	CHECK(brotherhood->flags == FlagsOf(0x0FDC8C, 40));
	CHECK(Judge(&*brotherhood, live) == Verdict::kSet);
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
	// Their quests' real stages are not, nor the ones left unmarked on purpose: DN070 5 (the rig's follow step), MQ105 0
	// (RepairStory's rule), the quickstarts past stage 63 (MS07c 190) and a stage the designer's note calls no debug one.
	CHECK(!IsDebug(File::kFallout4, 0x02BF21, 10));
	CHECK(!IsDebug(File::kFallout4, 0x04402C, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0229EA, 10));
	CHECK(!IsDebug(File::kFallout4, 0x0E9914, 5));
	CHECK(!IsDebug(File::kFallout4, 0x0229E6, 0));
	CHECK(!IsDebug(File::kFallout4, 0x0229FE, 190));
	CHECK(!IsDebug(File::kCoast, 0x04E77A, 160));    // DLC03FarHarborM02: "[MQ02] Far Harbor section skipped"
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
	const auto key = [](const QuickstartSet& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage, a_row.byFile, a_row.byQuest }; };
	CHECK(std::ranges::is_sorted(QUICKSTART_SETS, {}, key));
	CHECK(std::ranges::adjacent_find(QUICKSTART_SETS, {}, key) == std::end(QUICKSTART_SETS));  // one row for a stage and a quickstart's quest
	for (const auto& row : QUICKSTART_SETS) {
		CHECK(row.byStages != 0);
		for (std::uint16_t stage = 0; stage < 64; ++stage) {
			if ((row.byStages >> stage) & 1) {
				CHECK(IsDebug(static_cast<File>(row.byFile), row.byQuest, stage));
			}
		}
		CHECK(!IsDebug(static_cast<File>(row.file), row.quest, row.stage));  // a quickstart is never reported as it is (IsDebug)
	}
	std::size_t quickstarts = 0;
	for (const auto& rule : RULES) {
		if (rule.flags & kDebug) {
			++quickstarts;
			REQUIRE(rule.stage < 64);  // the rows name them by a bit
			const bool sets = std::ranges::any_of(QUICKSTART_SETS, [&](const QuickstartSet& a_row) {
				return a_row.byFile == rule.file && a_row.byQuest == rule.quest && ((a_row.byStages >> rule.stage) & 1);
			});
			// All but MQ00's three that only turn a faction ("Simulate the player getting kicked out of ..."): no stage set.
			const bool factionOnly = rule.file == 0 && rule.quest == 0x0C9B0B && (rule.stage == 10 || rule.stage == 30 || rule.stage == 40);
			CHECK(sets == !factionOnly);
		}
	}
	// Mass Fusion's DN084 1 to 9 and BoS303 0 to 3, the airport battle's 0 to 8, and the 25 others on the way to a kick-out
	// (MQ302 0 to 3, RR303 1 to 4, MQ206 0, MQ00 10 to 60, Min301 0 and 1, MinDestBoS 0, MQ302RR 10, RR304's seven): 47;
	// and the 109 other quests' quickstarts that set a stage a friend's game would be sent.
	CHECK(quickstarts == 156);
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
	// A stuck story's MQ105 (QuestSync::RepairStory) waits too: its start moves the player.
	CHECK(Judge(Find(0x0229E6, 0), { .sharedStory = true, .opening = true }) == Verdict::kOpeningWaits);
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
	// DLC04MQ05 is no longer SAFE's (its 8 turns the gangs on the player), so its quickstarts have no rows.
	CHECK(!Find(0x000805, 1, File::kNukaWorld));
	CHECK(!Find(0x000805, 3, File::kNukaWorld));
	// DLC04MS01: a Nuka-Cola Quartz taken, then a MoveTo.
	CHECK(FlagsOf(0x000806, 75, File::kNukaWorld) == kPays);
	CHECK(FlagsOf(0x000806, 550, File::kNukaWorld) == kMoves);
	// Fallout4.esm's: Diamond City's caps, the treasure's cave (the Combat Zone's are gone with it, the Prydwen's gantry
	// with BoSM04Postquest, which starts the repeatable DN053).
	CHECK(!Find(0x02074B, 1));               // CZ
	CHECK(FlagsOf(0x14191C, 10) == kFree);   // FFDiamondCity02: Home Plate, its price waived in a friend's game (PRICES)
	CHECK(FlagsOf(0x0E9914, 5) == kMoves);   // DN070
	CHECK(!Find(0x145722, 2));               // BoSM04Postquest
	// Far Harbor's treasure dig fades the screen out.
	CHECK(FlagsOf(0x04D2F5, 200, File::kCoast) == kMoves);  // DLC03PostQuestTreasureDig
	// A quest's own item taken is left alone (only its script's wait kept): the fog condenser module.
	const auto module = Find(0x023ACA, 210, File::kCoast);  // DLC03FarHarborFF02
	REQUIRE(module);
	CHECK(module->flags == 0);
	CHECK(module->wait == 50);
	CHECK(!Find(0x10CB94, 100));                   // FFGoodneighbor03: its own item, no wait
	CHECK(!Find(0x02039A, 300, File::kCoast));     // DLC03FarHarborFF03c
	// Their stage 0s that move the player are the developers' quickstarts, none the quest's start-up stage: DLC04MQ03's
	// sets the park's stages (kDebug, never its start: StartRule); the others are left out.
	CHECK(IsDebug(File::kNukaWorld, 0x000803, 0));  // DLC04MQ03
	CHECK(!StartRule(File::kNukaWorld, 0x000803));
	CHECK(!Find(0x000805, 0, File::kNukaWorld));  // DLC04MQ05 (not SAFE's now either)
	CHECK(!Find(0x0254B7, 0));                    // DN154
	CHECK(!Find(0x145722, 0));                    // BoSM04Postquest

	// A far friend's move is skipped, a quickstart's always; a payment is never made from a friend's game.
	const auto move = Find(0x000806, 550, File::kNukaWorld);  // DLC04MS01: Sierra's MoveTo
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
	CHECK(!SetsAnother(File::kFallout4, 0x02074B, 300));  // CZ, no longer SAFE's
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
	CHECK(!Find(0x000808, 0, File::kNukaWorld));
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
		{ File::kNukaWorld, 0x03684F, 600 },  // DLC04_KiddieKingdomMain: its script's one-time Init
		{ File::kNukaWorld, 0x03684F, 700 },
		{ File::kNukaWorld, 0x03684F, 740 },  // Oswald's fight
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
	// Their payments: DN049's pre-war money, the bloodworm meat Nuka-Town's trader takes.
	CHECK(FlagsOf(0x04542B, 711) == kPays);
	CHECK(FlagsOf(0x01AABF, 110, File::kNukaWorld) == kPays);
	CHECK(Judge(Find(0x04542B, 711), { .live = true, .nearFriend = true }) == Verdict::kSkip);
	// Their stages that set another shared quest's (-Safe -SetsOther): Far Harbor's settlements move its main quest on.
	CHECK(SetsAnother(File::kCoast, 0x00A991, 500));  // DLC03FarHarborS01 sets DLC03FarHarborM03 775
	CHECK(SetsAnother(File::kCoast, 0x00AEC3, 400));  // DLC03FarHarborS02 sets DLC03FarHarborM03 875
	CHECK(SetsAnother(File::kCoast, 0x013F3A, 200));  // DLC03Acadia_Institute sets DLC03MQ05 8990
	CHECK(SetsAnother(File::kNukaWorld, 0x000804, 10));  // DLC04MQ04 sets DLC04DialogueSettlement 140
	CHECK(!SetsAnother(File::kNukaWorld, 0x00CBD0, 200));  // DLC04DialogueHubologists: not shared
}

TEST("stage rules: no shared stage a friend's game sets pays through a stage it sets (tools/questscan.ps1 -Safe)")
{
	using namespace StageRules;
	// The scan's last line, SHARED_REACHES_PAYS: the stages of every quest a friend's stage is set on (the story's,
	// the dialogue quests', SAFE's, SILENT's, the radiants') that are no kPays or kFree row and reach, through their
	// fragments and the script functions they call, in their own quest or any other, a stage with a kPays or kFree row,
	// caps taken or the inventory emptied, not read by hand.
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
