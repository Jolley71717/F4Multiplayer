#include "Test.h"

#include "game/QuestFilter.h"
#include "game/StageRules.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <iterator>
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
	// Automatron's quickstarts (MoveTo Codsworth's marker, Fort Hagen's start).
	CHECK(FlagsOf(0x002833, 2, File::kRobot) == kMoves);  // DLC01MQ04
	CHECK(FlagsOf(0x0010F5, 1, File::kRobot) == kMoves);  // DLC01MQ05
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

	// Their stage 0s that move the player are the developers' quickstarts, none its quest's start-up stage: left out, so
	// a friend's stage that starts the quest here isn't judged by one (StageRules::Starting).
	CHECK(!Find(0x000801, 0, File::kNukaWorld));  // DLC04MQ01
	CHECK(!Find(0x000807, 0, File::kNukaWorld));  // DLC04MS02
	CHECK(!Find(0x000806, 0, File::kRobot));      // DLC01MQ01
	CHECK(!StageRules::Starting(Find(0x000806, 0, File::kRobot), Find(0x000806, 100, File::kRobot)));
	CHECK(StageRules::Starting(Find(0x000801, 0, File::kNukaWorld), door)->flags == kMoves);

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

	// The rows: Kellogg's memory (MQ203 10) and DN070's cave (5) are offered; Kiddie Kingdom's quickstart 6 is a move,
	// not a fight.
	CHECK(Judge(Find(0x0229EA, 10), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(FlagsOf(0x0E9914, 5) == kMoves);  // DN070: MoveTo DN070_TreasuresEntry (the rig's step_follow)
	CHECK(Judge(Find(0x0E9914, 5), { .sharedStory = true, .live = true, .followable = true }) == Verdict::kOffer);
	CHECK(FlagsOf(0x03684F, 6, File::kNukaWorld) == kMoves);
	CHECK(FlagsOf(0x03684F, 740, File::kNukaWorld) == (kMoves | kHostile));
	// kHostile is only ever with kMoves.
	for (const auto& rule : RULES) {
		CHECK(!(rule.flags & kHostile) || (rule.flags & kMoves));
	}
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
	// MS07a's 17 waits 1.5 s; its 0 moves the player (no bridge) and waits 4 s: from the session it isn't
	// set, but it is with the friend who just did it.
	const auto ms07a = Starting(Find(0x01CB51, 0), Find(0x01CB51, 17));
	REQUIRE(ms07a);
	CHECK(ms07a->flags == StageRules::kMoves);
	CHECK(ms07a->wait == 40);
	CHECK(StageRules::Judge(&*ms07a, {}) == Verdict::kSkip);
	CHECK(StageRules::Judge(&*ms07a, { .live = true, .nearFriend = true }) == Verdict::kSet);
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
	// skipped) is judged by MQ203's 0, and MQ204's first stage by MQ204's 0; both are debug stages left out of the
	// table, so The Glowing Sea is caught up.
	CHECK(!Find(0x0229EA, 0));
	CHECK(!Find(0x06B500, 0));
	CHECK(!Starting(Find(0x0229EA, 0), Find(0x0229EA, 20)));
	CHECK(!Starting(Find(0x06B500, 0), Find(0x06B500, 10)));
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

TEST("stage rules: the fades and the endings' slides move the player")
{
	using namespace StageRules;
	CHECK(FlagsOf(0x01ED62, 40) == kMoves);  // DialogueVault111: CryoPod.Activate(player), into the vault's pod
	CHECK(FlagsOf(0x01CC2A, 4) == kMoves);   // MQ102: the Pip-Boy's image-space effect
	// The endings: they fade to black and play the slides, needed all the same (they finish the story).
	CHECK(FlagsOf(0x0229EE, 850) == (kMoves | kBridge));  // MQ302: FadetoBlackImod
	CHECK(FlagsOf(0x0229EE, 855) == (kMoves | kBridge));  // MQ302: EndGameBinks
	CHECK(FlagsOf(0x0C9B0B, 50) == (kMoves | kBridge));   // MQ00
	CHECK(FlagsOf(0x0C9B0B, 60) == (kMoves | kBridge));
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
	// Nuka-World's main line under no type: DLC04MQ03's flag poles, MoveTo each.
	for (std::uint16_t stage = 1; stage <= 5; ++stage) {
		CHECK(FlagsOf(0x000803, stage, File::kNukaWorld) == kMoves);
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
	// Their stage 0s that move the player are the developers' quickstarts, none the quest's start-up stage: left out.
	CHECK(!Find(0x000803, 0, File::kNukaWorld));  // DLC04MQ03
	CHECK(!Find(0x000805, 0, File::kNukaWorld));  // DLC04MQ05 (not SAFE's now either)
	CHECK(!Find(0x0254B7, 0));                    // DN154
	CHECK(!Find(0x145722, 0));                    // BoSM04Postquest

	// A far friend's move is skipped; a payment is never made from a friend's game.
	const auto pole = Find(0x000803, 1, File::kNukaWorld);
	CHECK(Judge(pole, { .sharedStory = true, .live = true, .nearFriend = false }) == Verdict::kSkip);
	CHECK(Judge(pole, { .sharedStory = true, .live = false }) == Verdict::kSkip);
	CHECK(Judge(pole, { .sharedStory = true, .live = true, .nearFriend = true }) == Verdict::kSet);
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
	// DLC03MQ02Fight's quickstart moves the player; its 0 (another) is no start-up stage: left out.
	CHECK(FlagsOf(0x043822, 5, File::kCoast) == kMoves);
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
}

TEST("stage rules: a stage of a quest shared for its outcome that turns a faction on the player is set only with the friend near, as one that moves the player (tools/questscan.ps1 -Safe -Rules)")
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
		const auto rule = Find(quest, stage, file);
		REQUIRE(rule);
		// No bridge: none starts another shared quest or awards an achievement. kHostile: the follow offer says "fighting".
		CHECK(rule->flags == (kMoves | kHostile));
		CHECK(rule->wait == 0);
		// With the friend who did it near: at once, fighting or not. Far: offered to join them, and set once beside them;
		// not taken up, or caught up from the session: never, and no warning.
		CHECK(Judge(rule, { .live = true, .nearFriend = true }) == Verdict::kSet);
		CHECK(Judge(rule, { .live = true, .nearFriend = true, .held = true }) == Verdict::kSet);
		CHECK(Judge(rule, { .live = true, .nearFriend = false }) == Verdict::kSkip);  // the friend not known here: nobody to follow
		CHECK(Judge(rule, { .live = true, .nearFriend = false, .followable = true }) == Verdict::kOffer);
		CHECK(Judge(rule, { .live = false, .nearFriend = true, .followable = true, .offered = 40s }) == Verdict::kSet);
		CHECK(Judge(rule, { .live = false, .followable = true, .offered = OFFER_FOR }) == Verdict::kSkip);
		CHECK(Judge(rule, { .live = false }) == Verdict::kSkip);
		CHECK(Judge(rule, { .live = false, .nearFriend = true }) == Verdict::kSkip);
		CHECK(Judge(rule, { .opening = true }) == Verdict::kOpeningWaits);
	}
	// The peace a stage makes is no row: DN049's x50 (SetEnemy(PlayerFaction, True, False), then StopCombat) and its 55.
	CHECK(!Find(0x04542B, 50));
	CHECK(!Find(0x04542B, 150));
	CHECK(!Find(0x04542B, 155));
	// Nor a settlement given (DN036's endings, Far Harbor's S02 425 and FF01 220), nor two factions set on each other
	// (DLC04DialogueSettlement 700), nor an ally made (DLC03_ADV017 100).
	CHECK(!Find(0x08FCED, 550));
	CHECK(!Find(0x08FCED, 700));
	CHECK(!Find(0x00AEC3, 425, File::kCoast));
	CHECK(!Find(0x023ACB, 220, File::kCoast));
	CHECK(!Find(0x01AABF, 700, File::kNukaWorld));
	CHECK(!Find(0x02C687, 100, File::kCoast));
	// One that moves the player is kMoves the same way: Kiddie Kingdom's quickstart 6, which sets its 600. And the
	// Nucleus's quickstart.
	CHECK(FlagsOf(0x03684F, 6, File::kNukaWorld) == kMoves);
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

TEST("stage rules: a companion's quest's stages that are its owner's (kEach) are set in a friend's game by their EACH acts alone")
{
	using namespace StageRules;
	constexpr std::uint32_t CURIE = 0x16454E;     // COMCurieQuest, Emergent Behavior
	constexpr std::uint32_t MACCREADY = 0x027411;  // COMMacCreadyQuest, Long Road Ahead
	// The flag's own bit: 7 (6 is kDebug's), shared with no other flag.
	CHECK(kEach == 1 << 7);
	CHECK((kEach & (kMoves | kBridge | kPays | kStory | kFree | kHostile | (1 << 6))) == 0);
	// The table is sorted (a stage's acts kept in their order), every row's stage is kEach, and only the companions'
	// quests have kEach stages.
	for (auto it = std::next(std::begin(EACH)); it != std::end(EACH); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } <= std::tuple{ it->file, it->quest, it->stage }));
	}
	for (const auto& each : EACH) {
		CHECK(IsEach(static_cast<File>(each.file), each.quest, each.stage));
		CHECK(QuestFilter::IsCompanionQuest(each.file, each.quest));
	}
	for (const auto& rule : RULES) {
		if (rule.flags & kEach) {
			CHECK(QuestFilter::IsCompanionQuest(rule.file, rule.quest));
			CHECK(rule.wait == 0);  // no script runs here to wait for
		}
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

	// Before the reset: her items into the robot body, then the reset, moved to Amari. Nothing without the robot body.
	CHECK((NewBodyBefore(0x00102249, 0x000865D1, 0x0009A683) ==
		   std::vector<std::string>{ "00102249.removeallitems 000865D1 1", "00102249.recycleactor 0009A683" }));
	CHECK((NewBodyBefore(0x00102249, 0x000865D1, 0) == std::vector<std::string>{ "00102249.removeallitems 000865D1 1", "00102249.recycleactor" }));
	CHECK(NewBodyBefore(0x00102249, 0, 0x0009A683).empty());
	CHECK(NewBodyBefore(0, 0x000865D1, 0x0009A683).empty());
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
	CHECK(Commands(Each{ 0, 0x16454E, 300, Act::kNewBody, 0, 0x1E2F26, 1, 7, true }, QUEST, 0x00102249, 0).empty());  // NewBodyBefore and NewBodyAfter
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
		}
	};
}

TEST("stage rules: a companion's quest is driven by the game whose own scripts took it; its owner's stages are never marked done there without their script")
{
	using namespace StageRules;
	constexpr std::uint32_t CURIE = 0x16454E;
	constexpr std::uint32_t MACCREADY = 0x027411;
	CHECK(DriverOf(File::kFallout4, CURIE) && DriverOf(File::kFallout4, MACCREADY));
	CHECK(!DriverOf(File::kFallout4, 0x1666C5));  // COMCaitQuest: not a companion's quest of this table
	for (const auto& driver : DRIVERS) {
		CHECK(QuestFilter::IsCompanionQuest(driver.file, driver.quest));
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