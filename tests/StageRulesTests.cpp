#include "Test.h"

#include "game/StageRules.h"

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
	CHECK(FlagsOf(0x0BA4F2, 150) == kMoves);               // MQ101TVStation: would start MQ101, the pre-war opening, again
	CHECK(FlagsOf(0x0C3309, 500) == kMoves);               // MQ101SanctuaryHills: the same
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
