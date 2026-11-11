#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// What some quest stages' scripts do, for setting another player's stages here. Kept free of the
// game so it can be tested.
namespace StageRules
{
	using Clock = std::chrono::steady_clock;

	// What a stage's script does (or a stage it sets, a few levels down, in its own quest or another).
	inline constexpr std::uint8_t kMoves = 1 << 0;   // moves the player (MoveTo, a door it makes them use), fades the screen out, takes away their controls
	inline constexpr std::uint8_t kBridge = 1 << 1;  // ... and starts another shared quest or awards an achievement: needed all the same
	inline constexpr std::uint8_t kPays = 1 << 2;    // takes caps or ordinary items (not quest items) from the player: a bribe, a payment
	inline constexpr std::uint8_t kStory = 1 << 3;   // a side quest's stage that moves the main story or a faction's on
	inline constexpr std::uint8_t kFree = 1 << 4;    // a purchase whose price is a global its script reads: set with that price at 0 (PRICES)
	inline constexpr std::uint8_t kHostile = 1 << 5;  // turns a faction on the player (HOSTILE): it waits until none of that faction is near, never skipped; with kMoves too, the follow offer says "fighting"
	// What a stage does to its own player alone (the Hubologists' rads and extraction chair, their rank's global and bonus
	// spells, the objective that tracks them): never set from a friend's, as a payment isn't. The same bit as kPays (the
	// byte has no free one across the open changes); its own name so the rows say what they are.
	inline constexpr std::uint8_t kOwn = kPays;

	// The game files the rules are for (where a quest's form comes from). The values are tools/questscan.cs's indexes
	// (GearScan.MASTERS), not the game's load order.
	enum class File : std::uint8_t
	{
		kFallout4,
		kCoast,      // DLCCoast.esm, Far Harbor
		kRobot,      // DLCRobot.esm, Automatron
		kNukaWorld,  // DLCNukaWorld.esm
	};

	struct Rule
	{
		std::uint8_t  file;   // File
		std::uint32_t quest;  // form ID in its file
		std::uint16_t stage;
		std::uint8_t  flags;
		std::uint8_t  wait;  // tenths of a second its script waits after the stage is done (Utility.Wait, the player's MoveTo)
	};

	// Generated from the quest fragment scripts (Fallout4 - Misc.ba2, DLCCoast - Main.ba2), disassembled
	// and scanned for what each stage does to the player, following the stages each one sets. Only the
	// shared quests' stages that do one of these things, or wait. The bribes and payments were traced to
	// their items by hand (caps, a Stimpak, fusion cores, Jet, whiskey; quest items are left out: they
	// came from the quest). Story is only flagged in Fallout4.esm: Far Harbor files its own story as
	// side quests and main quests both. Sorted by file, quest and stage.
	// The dialogue quests (shared with the story since, see QuestFilter::IsDialogue) were scanned again for what their
	// stages take from the player (tools/questscan.ps1): five pay or sell, and two more, Home Plate's purchase
	// (DialogueDiamondCity 64 and 300), through the stage they set (kFree: see SHARED_REACHES_PAYS). What that scan sees: a
	// quest's own Fragment_Stage_ functions calling RemoveItem, RemoveAllItems, SellItem, RemoveItemByComponent, UnequipItem,
	// UnequipItemSlot or UnequipAll directly on Game.GetPlayer() or a PlayerRef property. In those it found no shared
	// quest's stage taking weapons, armor or the Pip-Boy, unequipping them, or emptying the inventory (RemoveAllItems is
	// only in MQ101, the opening, and DN019JoinCult, both misc quests, never shared). What it does not see: a call made
	// through the quest's own script (kmyQuest) or another script a fragment calls, through an alias that holds the
	// player (Alias_Player.GetRef()) or a variable it does not follow; a scene's, a dialogue line's or any other script's
	// that a stage starts; nor that a shared stage sets a stage of another quest, whose own taking is listed under that
	// quest (a misc one reads as never shared). So it is no proof that no shared stage does; OwnGear guards the local
	// player's gear from what the session itself does, not from what a stage's own scripts do once it is set.
	// kMoves is checked against tools/questscan.ps1 -Moves, which follows what the first scan did not: a call into the
	// quest's own script (kmyQuest.StartQuest) or any other script, a few calls deep, with a branch on a literal argument
	// followed one way (ControlToggle(1) gives the controls back), and a stage of ANY quest a stage sets or starts, shared
	// or not, with that quest's start-up stages. Dangerous Minds' lounger (MQ202 300) does nothing to the player itself:
	// it sets MQ203 10, whose MQ203Script.StartQuest fades out and moves the player into Kellogg's memory, and a joiner
	// catching up a friend's story was sent there (up to 0.10.1). Every stage it lists has kMoves here but two: MQ203's 0
	// and MQ204's 0, debug stages (no start-up flag: never set in a game played through) that set MQ202 300 and MQ203
	// 1000; Starting() judges every stage that starts their quest by its 0, so with them no stage of MQ204 (The Glowing
	// Sea) would ever be caught up.
	// Automatron's and Nuka-World's rows (files 2 and 3) are tools/questscan.ps1 -Rules's output (the same scans, and a
	// wait of each fragment's literal Utility.Wait plus 2 s for each MoveTo of the player), its payments checked by hand:
	// Nuka-World's raids' 90 (DLC04RaidChaseOff, DLC04RaidCoerce: the settlement's caps) and DLC04MS02 500 (the space
	// suits and helmets unequipped and taken); the robot's head DLC01MQ04 200 and the drink DLC04DryRockGulch 218 take
	// are quest items, read from an alias. Most of their moves are the developers' quickstarts (stages 1-4, MoveTo a
	// debug marker), never set in a game played through; DLC04MQ01 1000 sets DLC04MQ01Gauntlet 2500, which walks the
	// player through the Nuka-World station's stairwell door. Their stage 0s that move the player are left out, as
	// MQ203's and MQ204's (none is its quest's start-up stage): DLC01MQ01, DLC01MQ02, DLC01MQ04, DLC01MQ05, DLC04MQ01,
	// DLC04MQ02, DLC04MS02. None is a bridge: an add-on's quests start here when the player goes there.
	// The quests the host's sQuests = safe shares (QuestFilter::SAFE) have their rows from tools/questscan.ps1 -Safe -Rules
	// -Files 0,1,2,3, the same scans, their payments checked by hand: caps, a fern flower and a Nuka-Cola Quartz are
	// kPays; what a stage takes that is the quest's own (Far Harbor's fog condenser modules and its mirelurk carapaces
	// for Brooks, FFGoodneighbor03's HalluciGen gas canister) is not, as for the story's quest items. Most of their
	// moves are quickstarts too (DLC04MQ03 1-5); the others (DN070 5 into the treasure's cave, DLC04MS01 550,
	// DLC03PostQuestTreasureDig 200's fade) move a joiner only along with the friend who did them. Their stage 0s that
	// move the player and are no start-up stage are left out: DN154, DLC04MQ03. BoSM04Postquest is no longer SAFE's (its
	// 0 sets the repeatable DN053), so its gantry quickstart has no row. The
	// objective-less quests it shares too (QuestFilter::SILENT) have theirs from the same scan: a few waits, one
	// quickstart (DLC03MQ02Fight 5; its 0 is no start-up stage, left out with MS05BTapes's 0 and DLC04Grandchester's
	// 0), and Grandchester's pre-war money and ticket, kPays. The pump regulators the Children of Atom's pump takes
	// (DLC03CoA_FFNucleus03_PumpHandler 20 and 30) are the quest's own, as the fog condenser modules: no row. What
	// their stages give (caps from a fight's prize or a reward) both players get, as a shared quest's reward.
	// SAFE's quests shared for their outcomes (tools/questscan.cs ALLOW_OUTCOMES: a faction's hostility, a settlement)
	// come from the same scan. A stage that turns a faction on the player (SetPlayerEnemy, SetEnemy with the player's
	// faction and neither side made neutral, someone put in a PlayerEnemy faction; itself or through what it calls and
	// sets: SafeScan.Hostile) is no row here but HOSTILE's (tools/questscan.ps1 -Hostile), theirs and every other shared
	// quest's alike: a friend's waits until none of that faction is near, and is never skipped (up to 0.16.1 these 18, DN036
	// 500, DN049's x49, the Nucleus's 70, Acadia's 200, Far Harbor S02 210 and Kiddie Kingdom's 600, 700 and 740, were
	// kMoves|kHostile rows, set only beside the friend who did it, else offered and then skipped, while the story's own
	// hostile stages had no row and reached a far friend at once). A kMoves row whose stage is HOSTILE's too (19 of them,
	// no kBridge: MS17 465/467/470 into the Covenant's fight, InstMassFusion 30 the Brotherhood's kick-out, InstKickOut 100,
	// RR304 20, BoSM02 2-9 and BoSM04 2-4 the airport's ghouls, the quickstarts Kiddie Kingdom 6 and DLC01Lair 8) moves the player too: the hold first,
	// then the move's rules, and the offer to go with the friend says they are fighting; not gone with, the stage isn't set
	// (it would move them) but the faction it turns on the player is turned on them here all the same (Verdict::kHostileOnly,
	// HostileCall). Their payments: DN049 711's pre-war money and DLC04DialogueSettlement 110's bloodworm meat, kPays.
	// The add-ons' other faction outcomes, Combat Zone's and the end of The Big Dig's (2026-11-11) bring the developers'
	// quickstarts of CZ (1) and the Mechanist's lair (DLC01Lair 3-9), a few waits, and the Hubologists' rows: their
	// donations (30-37) are kPays, and every other stage of theirs but 200 (turning them on the player, HOSTILE's) is kOwn,
	// read by hand: the rads cast on the player in the extraction chair (20-27), the donation's objective (40, 41, 100) and
	// the rank's global and bonus spells (50-57; 50 gives the robes too), each player's own sessions with them.
	inline constexpr Rule RULES[] = {
		{ 0, 0x003648, 64, kFree, 0 },   // DialogueDiamondCity: Home Plate bought from Geneva (sets FFDiamondCity02 10: its price, the key, the house)
		{ 0, 0x003648, 70, kPays, 0 },   // DialogueDiamondCity: the World Series bat sold for caps
		{ 0, 0x003648, 300, kFree, 0 },  // DialogueDiamondCity: Home Plate bought from the mayor (sets FFDiamondCity02 10 too)
		{ 0, 0x019955, 210, kPays, 0 },  // DialogueBunkerHill: 10 caps
		{ 0, 0x01CB51, 0, kMoves, 40 },  // MS07a
		{ 0, 0x01CB51, 17, 0, 15 },
		{ 0, 0x01CB51, 18, 0, 5 },
		{ 0, 0x01CB51, 20, 0, 5 },
		{ 0, 0x01CB51, 27, 0, 30 },
		{ 0, 0x01CB51, 70, 0, 3 },
		{ 0, 0x01CB51, 120, 0, 10 },
		{ 0, 0x01CB51, 140, 0, 15 },
		{ 0, 0x01CB51, 200, 0, 10 },
		{ 0, 0x01CB51, 210, 0, 10 },
		{ 0, 0x01CC2A, 0, 0, 30 },  // MQ102 (below 10 the opening's: never shared, see IsOpening)
		{ 0, 0x01CC2A, 4, kMoves, 0 },  // the Pip-Boy's image-space effect
		{ 0, 0x01CC2A, 6, 0, 10 },
		{ 0, 0x01CC2A, 7, kMoves, 3 },
		{ 0, 0x01ED62, 40, kMoves, 0 },  // DialogueVault111 (the opening's: never shared, see IsOpening): into the cryopod
		{ 0, 0x02074B, 1, kMoves, 20 },  // CZ (Combat Zone): the developers' quickstart, MoveTo CZDebugMarker
		{ 0, 0x02074B, 200, 0, 10 },
		{ 0, 0x02074B, 250, 0, 20 },
		{ 0, 0x02074B, 265, 0, 2 },
		{ 0, 0x02125F, 30, kPays, 0 },  // FFDiamondCity03: a fern flower
		{ 0, 0x0229E6, 0, kMoves | kBridge, 20 },  // MQ105
		{ 0, 0x0229E6, 177, kPays, 0 },
		{ 0, 0x0229E7, 0, kMoves | kBridge, 40 },  // MQ106
		{ 0, 0x0229E7, 900, 0, 20 },
		{ 0, 0x0229E8, 0, kMoves | kBridge, 40 },  // MQ201
		{ 0, 0x0229E9, 160, kMoves, 0 },  // MQ202
		{ 0, 0x0229E9, 300, kMoves, 100 },  // the lounger: sets MQ203 10, whose script fades out and moves the player into Kellogg's memory
		{ 0, 0x0229EA, 1, kMoves, 100 },  // MQ203, Kellogg's memories (not its 0, see above; 1-8 its debug starts)
		{ 0, 0x0229EA, 2, kMoves, 100 },
		{ 0, 0x0229EA, 3, kMoves, 100 },
		{ 0, 0x0229EA, 4, kMoves, 100 },
		{ 0, 0x0229EA, 5, kMoves, 100 },
		{ 0, 0x0229EA, 6, kMoves, 100 },
		{ 0, 0x0229EA, 7, kMoves, 100 },
		{ 0, 0x0229EA, 8, kMoves, 100 },
		{ 0, 0x0229EA, 10, kMoves, 100 },  // MQ203Script.StartQuest: into the memory
		{ 0, 0x0229EA, 1000, kMoves, 111 },  // MQ203Script.FinishQuest: out of it, back on the lounger
		{ 0, 0x0229EC, 5, 0, 10 },  // MQ207
		{ 0, 0x0229EC, 10, 0, 1 },
		{ 0, 0x0229EC, 190, kMoves, 60 },
		{ 0, 0x0229EC, 200, 0, 10 },
		{ 0, 0x0229EE, 2, 0, 10 },  // MQ302
		{ 0, 0x0229EE, 300, 0, 20 },
		{ 0, 0x0229EE, 700, 0, 30 },
		{ 0, 0x0229EE, 750, kMoves, 80 },
		{ 0, 0x0229EE, 800, 0, 20 },
		{ 0, 0x0229EE, 850, kMoves | kBridge, 250 },  // fades to black for the ending
		{ 0, 0x0229EE, 855, kMoves | kBridge, 1 },  // the ending's slides
		{ 0, 0x0229F6, 0, kMoves, 20 },  // MS01
		{ 0, 0x0229F6, 1050, 0, 50 },
		{ 0, 0x0229F7, 17, kMoves, 0 },  // MS02: MS02Script.Freeze takes the controls
		{ 0, 0x0229FD, 35, kMoves, 75 },  // MS07b
		{ 0, 0x0229FD, 37, kMoves, 45 },
		{ 0, 0x0229FE, 0, kMoves | kBridge | kStory, 30 },  // MS07c
		{ 0, 0x0229FE, 1, kMoves | kStory, 30 },
		{ 0, 0x0229FE, 190, kMoves | kStory, 25 },
		{ 0, 0x0229FE, 217, 0, 10 },
		{ 0, 0x0229FE, 219, kMoves, 20 },
		{ 0, 0x0229FE, 220, kMoves, 3 },
		{ 0, 0x0229FE, 230, kMoves, 10 },
		{ 0, 0x0229FE, 250, 0, 20 },
		{ 0, 0x0229FE, 263, kMoves, 20 },
		{ 0, 0x022A00, 1275, 0, 5 },  // MS09
		{ 0, 0x022A00, 1290, 0, 20 },
		{ 0, 0x022A00, 1295, 0, 20 },
		{ 0, 0x022A00, 1330, 0, 20 },
		{ 0, 0x022A00, 1340, 0, 20 },
		{ 0, 0x022A02, 521, 0, 5 },  // MS11
		{ 0, 0x022A02, 522, 0, 5 },
		{ 0, 0x022A02, 523, 0, 10 },
		{ 0, 0x022A02, 524, 0, 5 },
		{ 0, 0x022A02, 525, 0, 10 },
		{ 0, 0x022A02, 526, 0, 5 },
		{ 0, 0x022A02, 775, 0, 20 },
		{ 0, 0x022A02, 900, 0, 30 },
		{ 0, 0x022A02, 930, 0, 31 },
		{ 0, 0x022A02, 975, 0, 30 },
		{ 0, 0x022A02, 1000, 0, 30 },
		{ 0, 0x022A04, 805, kPays, 0 },  // MS13
		{ 0, 0x022A07, 1, kMoves, 20 },  // MS16
		{ 0, 0x022A07, 59, kMoves, 50 },
		{ 0, 0x022A07, 70, 0, 10 },
		{ 0, 0x022A07, 89, 0, 20 },
		{ 0, 0x022A07, 149, kMoves, 30 },
		{ 0, 0x022A07, 199, kMoves, 30 },
		{ 0, 0x022A07, 260, 0, 5 },
		{ 0, 0x022A08, 0, 0, 5 },  // MS17
		{ 0, 0x022A08, 150, kMoves, 0 },
		{ 0, 0x022A08, 175, kMoves, 0 },
		{ 0, 0x022A08, 204, kMoves, 0 },
		{ 0, 0x022A08, 315, 0, 10 },
		{ 0, 0x022A08, 331, 0, 10 },
		{ 0, 0x022A08, 350, kMoves, 0 },
		{ 0, 0x022A08, 450, 0, 10 },
		{ 0, 0x022A08, 465, kMoves, 10 },
		{ 0, 0x022A08, 467, kMoves, 10 },
		{ 0, 0x022A08, 470, kMoves, 10 },
		{ 0, 0x022A0A, 60, kMoves, 0 },  // MS19: MS19Script.Freeze takes the controls
		{ 0, 0x026340, 1, kMoves, 20 },  // DN121
		{ 0, 0x026340, 2, kMoves, 20 },
		{ 0, 0x026340, 3, kMoves | kBridge, 20 },
		{ 0, 0x026340, 4, kMoves | kBridge, 20 },
		{ 0, 0x026340, 454, 0, 10 },
		{ 0, 0x027556, 442, 0, 1 },  // MS04
		{ 0, 0x027556, 445, 0, 1 },
		{ 0, 0x027556, 1230, 0, 1 },
		{ 0, 0x0284BC, 40, 0, 30 },  // DN015
		{ 0, 0x02B759, 16, 0, 30 },  // DN058
		{ 0, 0x02BF21, 0, kMoves | kBridge, 40 },  // BoS201
		{ 0, 0x02BF21, 1, kMoves | kBridge, 160 },
		{ 0, 0x02BF21, 2, kMoves | kBridge, 160 },
		{ 0, 0x02BF21, 3, kMoves | kBridge, 160 },
		{ 0, 0x02BF21, 4, kMoves | kBridge, 160 },
		{ 0, 0x02BF21, 10, 0, 120 },
		{ 0, 0x02BF21, 35, kMoves, 20 },
		{ 0, 0x02BF21, 45, kMoves, 0 },
		{ 0, 0x02BF21, 140, 0, 10 },
		{ 0, 0x02C8CB, 200, 0, 1 },  // RR302
		{ 0, 0x02C8CB, 10100, 0, 1 },
		{ 0, 0x02C8CB, 10800, 0, 1 },
		{ 0, 0x033527, 3, kPays, 0 },  // V81_03
		{ 0, 0x033527, 550, kPays, 0 },
		{ 0, 0x03A457, 0, 0, 10 },  // Min02
		{ 0, 0x03E0C1, 90, kPays, 0 },  // MinRadiantOwned03ChangeLocOnly
		{ 0, 0x03F244, 5, kPays, 0 },    // DialogueVault81: 5 caps
		{ 0, 0x03F244, 645, kPays, 0 },  // DialogueVault81: 25 fertilizer sold
		{ 0, 0x043275, 1, kMoves | kBridge, 41 },  // RR303
		{ 0, 0x043275, 2, 0, 31 },
		{ 0, 0x043275, 110, 0, 10 },
		{ 0, 0x043275, 200, 0, 1 },
		{ 0, 0x043275, 800, kMoves, 0 },
		{ 0, 0x043275, 900, 0, 100 },
		{ 0, 0x043275, 910, 0, 100 },
		{ 0, 0x043275, 1100, 0, 30 },
		{ 0, 0x043275, 1150, 0, 100 },
		{ 0, 0x043275, 11200, 0, 100 },
		{ 0, 0x04402C, 0, kMoves, 40 },  // BoSM02
		{ 0, 0x04402C, 1, 0, 20 },
		{ 0, 0x04402C, 2, kMoves, 40 },
		{ 0, 0x04402C, 3, kMoves, 40 },
		{ 0, 0x04402C, 4, kMoves, 40 },
		{ 0, 0x04402C, 5, kMoves, 40 },
		{ 0, 0x04402C, 6, kMoves, 80 },
		{ 0, 0x04402C, 7, kMoves, 80 },
		{ 0, 0x04402C, 8, kMoves, 80 },
		{ 0, 0x04402C, 9, kMoves, 80 },
		{ 0, 0x04402C, 150, 0, 60 },
		{ 0, 0x04402C, 210, kMoves, 0 },
		{ 0, 0x04542B, 711, kPays, 0 },  // DN049: RemoveItem(PrewarMoney x5000); its x49 stages, each shop's robots turned on the player, are HOSTILE's
		{ 0, 0x04542B, 856, 0, 50 },
		{ 0, 0x0459D2, 0, 0, 1 },  // RR101
		{ 0, 0x0459D2, 600, kMoves, 40 },
		{ 0, 0x0459D2, 710, 0, 3 },
		{ 0, 0x0498B5, 0, kMoves, 20 },  // InstM01_OLD (an old cut of InstM01, its 0 a playtest start)
		{ 0, 0x0498B5, 35, kPays, 0 },   // 500 caps, as InstM01's 35
		{ 0, 0x0537FC, 0, kMoves | kBridge, 60 },  // BoS201B
		{ 0, 0x0537FF, 0, kMoves | kBridge, 60 },  // BoS202
		{ 0, 0x0537FF, 1, kMoves | kBridge, 60 },
		{ 0, 0x0537FF, 2, kMoves | kBridge, 60 },
		{ 0, 0x0537FF, 30, kMoves, 0 },
		{ 0, 0x0537FF, 50, kMoves, 0 },
		{ 0, 0x055BE4, 1500, 0, 10 },  // MinVsInst
		{ 0, 0x05DDAB, 0, kMoves, 20 },  // BoS100
		{ 0, 0x05DEE4, 0, kStory, 0 },  // Min01
		{ 0, 0x05DEE4, 1, kStory, 0 },
		{ 0, 0x05DEE4, 20, kStory, 0 },
		{ 0, 0x05DEE4, 215, kStory, 0 },
		{ 0, 0x05DEE4, 220, kStory, 0 },
		{ 0, 0x05DEE4, 1500, kStory, 0 },
		{ 0, 0x05DEE4, 2000, kStory, 0 },
		{ 0, 0x05DEE4, 2010, kStory, 0 },
		{ 0, 0x05DEE4, 2100, kStory, 0 },  // Stop(): with 1500 and 2000 left out, the quest would end unfinished
		{ 0, 0x05FD90, 410, 0, 10 },  // RRM01
		{ 0, 0x064C66, 177, 0, 30 },  // DN009_MQ205
		{ 0, 0x064C66, 190, 0, 30 },
		{ 0, 0x064EC7, 0, kMoves | kBridge, 20 },  // BoSR01
		{ 0, 0x068D7B, 0, kMoves, 20 },  // BoS100Fight
		{ 0, 0x06F5C1, 0, kMoves | kBridge, 20 },  // BoS101
		{ 0, 0x06F5C1, 1, kMoves | kBridge, 20 },
		{ 0, 0x06F5C1, 2, kMoves | kBridge, 20 },
		{ 0, 0x06FA37, 222, 0, 1 },  // RR102
		{ 0, 0x06FA37, 250, 0, 10 },
		{ 0, 0x06FA37, 700, 0, 50 },
		{ 0, 0x06FA37, 725, 0, 10 },
		{ 0, 0x07CC61, 100, 0, 1 },  // RR301
		{ 0, 0x07CC61, 200, 0, 10 },
		{ 0, 0x07CC61, 210, 0, 20 },
		{ 0, 0x084E40, 400, kPays, 0 },  // MS13MarowskiRevenge: caps
		{ 0, 0x0986C7, 0, kMoves, 20 },  // InstM01
		{ 0, 0x0986C7, 35, kPays, 0 },
		{ 0, 0x099848, 90, kPays, 0 },  // MinRecruit02
		{ 0, 0x09F09F, 15, 0, 20 },  // FFDiamondCity07FIN
		{ 0, 0x09FF4E, 0, kMoves | kBridge, 60 },  // BoS203
		{ 0, 0x0A274B, 0, kMoves, 0 },  // Inst305
		{ 0, 0x0A274B, 10, kMoves, 0 },
		{ 0, 0x0A274B, 60, 0, 50 },
		{ 0, 0x0A8257, 0, kMoves | kBridge, 20 },  // InstM02
		{ 0, 0x0A8BAF, 5, 0, 10 },  // RRM02
		{ 0, 0x0A8BAF, 75, 0, 3 },
		{ 0, 0x0A8BAF, 100, 0, 8 },
		{ 0, 0x0A8BAF, 400, 0, 40 },
		{ 0, 0x0AA778, 0, 0, 10 },  // Min03
		{ 0, 0x0AA778, 400, 0, 10 },
		{ 0, 0x0ABF55, 500, 0, 30 },  // DN029FirstWave
		{ 0, 0x0AC7B3, 20, 0, 50 },  // Inst306
		{ 0, 0x0ADCA6, 0, 0, 20 },  // DN029SecondWaveA
		{ 0, 0x0ADCE5, 0, kMoves, 70 },  // BoSM04
		{ 0, 0x0ADCE5, 1, 0, 50 },
		{ 0, 0x0ADCE5, 2, kMoves, 70 },
		{ 0, 0x0ADCE5, 3, kMoves, 70 },
		{ 0, 0x0ADCE5, 4, kMoves, 70 },
		{ 0, 0x0AE44C, 200, 0, 10 },  // DN029SecondWaveB
		{ 0, 0x0AE44C, 500, 0, 30 },
		{ 0, 0x0AE51C, 0, kMoves | kBridge, 60 },  // BoS301
		{ 0, 0x0AE51C, 1, kMoves | kBridge, 60 },
		{ 0, 0x0AE51C, 2, kMoves | kBridge, 90 },
		{ 0, 0x0AE51C, 3, kMoves | kBridge, 90 },
		{ 0, 0x0AE51C, 4, kMoves | kBridge, 90 },
		{ 0, 0x0AE51C, 65, 0, 50 },
		{ 0, 0x0AE51C, 75, 0, 50 },
		{ 0, 0x0AE51C, 76, 0, 50 },
		{ 0, 0x0AE51C, 90, 0, 50 },
		{ 0, 0x0B1752, 40, 0, 10 },  // MQ206
		{ 0, 0x0B1752, 500, kMoves | kBridge, 1 },
		{ 0, 0x0B1752, 1000, kMoves | kBridge, 40 },  // the teleporter: the only way into the Institute
		{ 0, 0x0B1752, 1050, 0, 20 },
		{ 0, 0x0B1D79, 0, kMoves | kBridge, 20 },  // BoSM01
		{ 0, 0x0B1D79, 1, kMoves | kBridge, 40 },
		{ 0, 0x0B1D79, 2, kMoves, 20 },
		{ 0, 0x0B1D79, 3, kMoves, 20 },
		{ 0, 0x0B1D79, 4, kMoves, 20 },
		{ 0, 0x0B1D79, 5, kMoves, 20 },
		{ 0, 0x0B1D79, 6, kMoves, 20 },
		{ 0, 0x0B1D79, 7, kMoves, 20 },
		{ 0, 0x0B1D79, 8, kMoves, 20 },
		{ 0, 0x0B1D79, 9, kMoves, 20 },
		{ 0, 0x0B1F1C, 0, 0, 1 },  // RRR01b
		{ 0, 0x0B26BC, 1400, kMoves | kBridge, 1 },  // MQ206RR
		{ 0, 0x0B26BC, 1500, kMoves | kBridge, 40 },
		{ 0, 0x0B26BD, 1400, kMoves | kBridge, 1 },  // MQ206BoS
		{ 0, 0x0B26BD, 1500, kMoves | kBridge, 40 },
		{ 0, 0x0B26BE, 1400, kMoves | kBridge, 1 },  // MQ206Min
		{ 0, 0x0B26BE, 1500, kMoves | kBridge, 40 },
		{ 0, 0x0B26C2, 300, 0, 1 },  // RRR04
		{ 0, 0x0B2D47, 0, kMoves, 20 },  // InstM03
		{ 0, 0x0B2D48, 125, 0, 1 },  // RR201
		{ 0, 0x0B2D48, 1050, 0, 1 },
		{ 0, 0x0B3562, 0, kMoves, 20 },  // BoSM01Postquest
		{ 0, 0x0B8464, 15, 0, 20 },  // V81_00_Intro
		{ 0, 0x0B8464, 400, kPays, 0 },
		{ 0, 0x0B9F9D, 0, kMoves | kBridge, 90 },  // BoS302
		{ 0, 0x0B9F9D, 1, kMoves | kBridge, 90 },
		{ 0, 0x0B9F9D, 2, kMoves | kBridge, 90 },
		{ 0, 0x0B9F9D, 155, 0, 100 },
		{ 0, 0x0BA4F2, 150, kMoves | kPays, 0 },  // MQ101TVStation: sets a stage of MQ101, the pre-war opening, whose start-up stage takes the controls and empties the inventory (RemoveAllItems)
		{ 0, 0x0BA4F2, 200, kMoves | kPays, 0 },
		{ 0, 0x0BACFF, 0, kMoves, 20 },  // Inst307
		{ 0, 0x0BACFF, 30, 0, 50 },
		{ 0, 0x0BACFF, 35, kMoves, 60 },
		{ 0, 0x0BACFF, 40, 0, 20 },
		{ 0, 0x0BACFF, 85, kMoves, 118 },
		{ 0, 0x0BACFF, 90, kMoves, 118 },
		{ 0, 0x0BACFF, 99, kMoves, 60 },
		{ 0, 0x0BACFF, 100, 0, 150 },
		{ 0, 0x0BAD00, 30, kMoves | kBridge, 0 },  // Inst308: the ending's slides
		{ 0, 0x0BAD00, 40, kMoves, 20 },
		{ 0, 0x0BDB04, 0, 0, 10 },  // MinDefendCastle
		{ 0, 0x0C30DC, 0, kMoves | kBridge, 20 },  // BosR02
		{ 0, 0x0C3309, 91, kMoves | kPays, 0 },  // MQ101SanctuaryHills: the same
		{ 0, 0x0C3309, 500, kMoves | kPays, 0 },
		{ 0, 0x0C8675, 0, kMoves | kBridge, 60 },  // BoSR03
		{ 0, 0x0C9B0B, 50, kMoves | kBridge, 0 },  // MQ00: the ending's slides
		{ 0, 0x0C9B0B, 60, kMoves | kBridge, 1 },
		{ 0, 0x0CF3E2, 0, kMoves | kBridge, 60 },  // BoSR04
		{ 0, 0x0D1EB2, 0, kMoves | kBridge, 60 },  // BoSR05
		{ 0, 0x0DDFDB, 0, 0, 10 },  // Min301
		{ 0, 0x0DDFDB, 1, 0, 10 },
		{ 0, 0x0DFB3C, 0, 0, 10 },  // MinDestBoS
		{ 0, 0x0E2058, 30, 0, 20 },  // Inst301
		{ 0, 0x0E2058, 45, 0, 3 },
		{ 0, 0x0E9914, 5, kMoves, 20 },  // DN070: MoveTo DN070_TreasuresEntry
		{ 0, 0x0E9914, 90, 0, 5 },
		{ 0, 0x0E9914, 95, 0, 20 },
		{ 0, 0x0E9914, 105, 0, 25 },
		{ 0, 0x0EBE71, 700, 0, 30 },  // MQ00MamaMurphy
		{ 0, 0x0FDC8C, 0, kMoves | kBridge, 100 },  // BoS303
		{ 0, 0x0FDC8C, 1, kMoves | kBridge, 100 },
		{ 0, 0x0FDC8C, 2, kMoves | kBridge, 120 },
		{ 0, 0x0FDC8C, 3, kMoves | kBridge, 100 },
		{ 0, 0x0FDC8C, 40, 0, 30 },
		{ 0, 0x0FDC8C, 200, 0, 30 },
		{ 0, 0x0FDC8C, 210, 0, 30 },
		{ 0, 0x0FDC8C, 220, 0, 30 },
		{ 0, 0x0FF3C2, 600, 0, 130 },  // DN011
		{ 0, 0x1073CE, 700, 0, 20 },  // DN138
		{ 0, 0x107A1C, 0, kMoves | kBridge, 100 },  // BoS302B
		{ 0, 0x108EC0, 30, kMoves, 0 },  // FFGoodneighbor01
		{ 0, 0x108EC0, 40, kMoves, 110 },  // the Memory Den's sequence (FFGoodneighbor01MemorySequence 10: fades out, into the memory)
		{ 0, 0x108EC0, 70, kMoves, 0 },
		{ 0, 0x108EC0, 80, kMoves, 0 },
		{ 0, 0x108EC0, 100, kStory, 0 },
		{ 0, 0x10B9D7, 200, 0, 40 },  // DN116
		{ 0, 0x10B9D7, 300, 0, 80 },
		{ 0, 0x10C64A, 350, 0, 20 },  // MQ302Min
		{ 0, 0x10C64A, 360, 0, 62 },
		{ 0, 0x10C64A, 400, 0, 35 },
		{ 0, 0x10C64B, 300, 0, 20 },  // MQ302BoS
		{ 0, 0x10C64C, 330, 0, 52 },  // MQ302RR
		{ 0, 0x10C64C, 700, 0, 10 },
		{ 0, 0x122221, 400, 0, 1 },  // FFBunkerHill02
		{ 0, 0x1323AB, 10, 0, 5 },  // FFGoodneighbor07 (the end of The Big Dig)
		{ 0, 0x134297, 0, kMoves | kBridge, 20 },  // MQ106Investigations
		{ 0, 0x136329, 0, kMoves | kBridge, 40 },  // Min207
		{ 0, 0x14191C, 10, kFree, 0 },  // FFDiamondCity02: Home Plate's price, its key and the house
		{ 0, 0x142EDC, 0, kMoves | kBridge, 40 },  // BoSM00
		{ 0, 0x142FD3, 1, kMoves | kBridge, 41 },  // RR304
		{ 0, 0x142FD3, 20, kMoves, 61 },
		{ 0, 0x142FD3, 25, kMoves | kBridge, 71 },
		{ 0, 0x142FD3, 27, 0, 1 },
		{ 0, 0x142FD3, 30, kMoves | kBridge, 125 },
		{ 0, 0x142FD3, 100, 0, 1 },
		{ 0, 0x142FD3, 800, 0, 1 },
		{ 0, 0x142FD3, 1200, 0, 100 },
		{ 0, 0x14B717, 25, 0, 5 },  // MS05B
		{ 0, 0x14B717, 125, 0, 25 },
		{ 0, 0x14B717, 157, 0, 10 },
		{ 0, 0x14B717, 252, kMoves, 20 },
		{ 0, 0x14B717, 253, kMoves, 20 },
		{ 0, 0x14B717, 275, 0, 5 },
		{ 0, 0x14B717, 500, 0, 10 },
		{ 0, 0x15BD39, 30, kMoves, 80 },  // InstMassFusion
		{ 0, 0x15BD39, 40, 0, 20 },
		{ 0, 0x1666C5, 0, kMoves, 40 },  // COMCaitQuest
		{ 0, 0x16D036, 100, kMoves, 60 },  // InstKickOut
		{ 0, 0x173ED9, 0, kMoves | kBridge, 100 },  // BoS304
		{ 0, 0x173ED9, 1, kMoves | kBridge, 100 },
		{ 0, 0x173ED9, 2, 0, 90 },
		{ 0, 0x173ED9, 40, 0, 70 },
		{ 0, 0x173ED9, 45, 0, 10 },
		{ 0, 0x173ED9, 57, 0, 10 },
		{ 0, 0x173ED9, 255, 0, 20 },
		{ 0, 0x1764DF, 40, kPays, 0 },  // FFDiamondCity10
		{ 0, 0x17CA39, 10, kMoves | kBridge, 60 },  // Inst307ChangeLocation
		{ 0, 0x182EAE, 0, 0, 70 },  // BoS305
		{ 0, 0x1851A0, 10, 0, 20 },  // BoSEnable
		{ 0, 0x1851A0, 20, 0, 50 },
		{ 0, 0x1870D0, 0, 0, 40 },  // UFOCrashQuest
		{ 0, 0x1870D0, 5, 0, 40 },
		{ 1, 0x001B3F, 500, 0, 10 },  // DLC03MQ01
		{ 1, 0x001B40, 0, kMoves | kBridge, 20 },  // DLC03MQ02
		{ 1, 0x001B40, 480, kPays, 0 },
		{ 1, 0x001B41, 210, kMoves, 40 },  // DLC03MQ03
		{ 1, 0x001B41, 220, kMoves, 60 },
		{ 1, 0x001B42, 1, kMoves | kBridge, 20 },  // DLC03MQ04
		{ 1, 0x001B43, 200, kMoves, 55 },  // DLC03MQ05
		{ 1, 0x001B44, 1, 0, 20 },  // DLC03MQ06
		{ 1, 0x001B44, 2, 0, 20 },
		{ 1, 0x001B44, 3, 0, 20 },
		{ 1, 0x001B44, 120, 0, 40 },
		{ 1, 0x001B44, 190, 0, 80 },
		{ 1, 0x004C8E, 1, kMoves, 20 },  // DLC03CoA_DialogueNucleus: MoveTo DebugMarker (its 70, the Children of Atom's kick-out, is HOSTILE's)
		{ 1, 0x004C8F, 10, 0, 10 },  // DLC03CoA_DialogueNucleusEntrance
		{ 1, 0x005BD7, 90, kPays, 0 },   // DLC03DialogueFarHarbor: the charge card sold for caps
		{ 1, 0x00AFB1, 0, kMoves, 20 },  // DLC03AtomM01
		{ 1, 0x00AFB1, 70, 0, 5 },
		{ 1, 0x01055C, 30, 0, 45 },  // DLC03CoA_FFNucleus01
		{ 1, 0x013F3A, 250, 0, 6 },  // DLC03Acadia_Institute (its 200, Acadia turned on the player, is HOSTILE's)
		{ 1, 0x013F3A, 460, 0, 32 },
		{ 1, 0x01CBFA, 1, kMoves, 50 },  // DLC03MQ04_01: into DiMA's memories (MQ04Script.EnterVR)
		{ 1, 0x01E3FE, 350, kMoves, 50 },  // DLC03MQ04_05: out of them
		{ 1, 0x02127F, 1, kMoves | kBridge, 85 },  // DLC03MQ06a
		{ 1, 0x02127F, 200, kMoves, 95 },
		{ 1, 0x02127F, 255, 0, 15 },
		{ 1, 0x02127F, 760, 0, 40 },
		{ 1, 0x023ACA, 210, 0, 50 },  // DLC03FarHarborFF02: takes a fog condenser module, the quest's own item
		{ 1, 0x023ACA, 220, 0, 50 },
		{ 1, 0x023ACA, 230, 0, 50 },
		{ 1, 0x0247B3, 45, 0, 5 },  // DLC03AtomM01_EncounterHandler
		{ 1, 0x0247B3, 50, 0, 5 },
		{ 1, 0x03389A, 15, 0, 20 },  // DLC03_V118_Intro
		{ 1, 0x036763, 548, kMoves, 82 },  // DLC03_V118_Quest: the date (faded out, moved to it)
		{ 1, 0x040AF0, 100, 0, 60 },  // DLC03MQ06_PlayerExitWindFarm
		{ 1, 0x040AF0, 200, 0, 10 },
		{ 1, 0x043822, 5, kMoves, 20 },  // DLC03MQ02Fight: MoveTo DLC03MQ02Fight_PlayerQuickstartMarker2 (a quickstart)
		{ 1, 0x04D2F5, 200, kMoves, 60 },  // DLC03PostQuestTreasureDig: FadeOutGame
		{ 2, 0x0008BB, 3, kMoves, 20 },  // DLC01Lair, the developers' quickstarts: MoveTo DLC01Lair01_QuickstartFacilityEntranceMarker
		{ 2, 0x0008BB, 4, kMoves, 20 },  // MoveTo DLC01Lair01_QuickstartProductionEntranceMarker
		{ 2, 0x0008BB, 5, kMoves, 20 },  // MoveTo DLC01Lair01_QuickstartBypassMarker
		{ 2, 0x0008BB, 6, kMoves, 20 },  // MoveTo DLC01Lair_QuickstartMechanistBattle
		{ 2, 0x0008BB, 7, kMoves, 20 },  // MoveTo DLC01Lair_QuickstartMechanistBattle
		{ 2, 0x0008BB, 8, kMoves, 20 },  // MoveTo DLC01Lair_QuickstartMechanistBattle
		{ 2, 0x0008BB, 9, kMoves, 20 },  // MoveTo DLC01Lair_MBattle_SparkRestorePowerPoint
		{ 2, 0x0008BB, 100, 0, 12 },
		{ 2, 0x0008BB, 200, 0, 70 },
		{ 2, 0x0008BB, 210, 0, 20 },
		{ 2, 0x0008BB, 290, 0, 15 },
		{ 2, 0x0008BB, 480, 0, 10 },
		{ 2, 0x0008BB, 1000, 0, 45 },
		{ 2, 0x0008BB, 1150, 0, 20 },
		{ 2, 0x0008BB, 1350, 0, 20 },
		{ 2, 0x0008BB, 1499, 0, 20 },
		{ 2, 0x0010F5, 1, kMoves, 20 },  // DLC01MQ05: MoveTo CodsworthTalkMarker (quickstarts)
		{ 2, 0x0010F5, 2, kMoves, 20 },
		{ 2, 0x002833, 1, kMoves, 20 },  // DLC01MQ04: MoveTo CodsworthTalkMarker, DLC01MQ04HagenStart (quickstarts)
		{ 2, 0x002833, 2, kMoves, 20 },
		{ 2, 0x002833, 3, kMoves, 20 },
		{ 3, 0x000801, 1, kMoves, 0 },  // DLC04MQ01: DLC04MQ01Gauntlet 5, 6, 9 MoveTo its quickstart markers
		{ 3, 0x000801, 2, kMoves, 0 },
		{ 3, 0x000801, 3, kMoves, 0 },
		{ 3, 0x000801, 217, 0, 20 },
		{ 3, 0x000801, 1000, kMoves, 0 },  // DLC04MQ01Gauntlet 2500: the player through DLC04NukaStationStairwellDoorChain
		{ 3, 0x000803, 1, kMoves, 20 },  // DLC04MQ03: MoveTo DLC04GZFlagPoleMarkerRef
		{ 3, 0x000803, 2, kMoves, 20 },  // MoveTo DLC04DRGFlagPoleMarkerRef
		{ 3, 0x000803, 3, kMoves, 20 },  // MoveTo DLC04KKFlagPoleMarkerRef
		{ 3, 0x000803, 4, kMoves, 20 },  // MoveTo DLC04SAFlagPoleMarkerRef
		{ 3, 0x000803, 5, kMoves, 20 },  // MoveTo DLC04BPFlagPoleMarkerRef
		{ 3, 0x000804, 1, kMoves, 20 },  // DLC04MQ04: MoveTo PlayerDebugMarker (quickstarts)
		{ 3, 0x000804, 2, kMoves, 20 },
		{ 3, 0x000804, 3, kMoves, 100 },
		{ 3, 0x000804, 4, kMoves, 20 },
		{ 3, 0x000804, 500, 0, 50 },
		{ 3, 0x000806, 75, kPays, 0 },  // DLC04MS01: a Nuka-Cola Quartz
		{ 3, 0x000806, 400, 0, 50 },
		{ 3, 0x000806, 450, 0, 20 },
		{ 3, 0x000806, 475, 0, 10 },
		{ 3, 0x000806, 500, 0, 10 },
		{ 3, 0x000806, 550, kMoves, 20 },  // MoveTo DLC04MS01PT3
		{ 3, 0x000806, 755, 0, 30 },
		{ 3, 0x000807, 500, kPays, 0 },  // DLC04MS02: the space suits and helmets unequipped and taken (RemoveItem x5 each)
		{ 3, 0x000808, 1, 0, 1 },  // DLC04Grandchester
		{ 3, 0x000808, 2, 0, 1 },
		{ 3, 0x000808, 3, 0, 1 },
		{ 3, 0x000808, 100, kPays, 0 },  // pre-war money x65
		{ 3, 0x000808, 120, kPays, 0 },  // its ticket
		{ 3, 0x00CBD0, 20, kOwn, 0 },  // DLC04DialogueHubologists, each player's own sessions (kOwn; its 200, the Hubologists turned on the player, is HOSTILE's): the rads cast on the player in the extraction chair
		{ 3, 0x00CBD0, 21, kOwn, 0 },
		{ 3, 0x00CBD0, 22, kOwn, 0 },
		{ 3, 0x00CBD0, 23, kOwn, 0 },
		{ 3, 0x00CBD0, 24, kOwn, 0 },
		{ 3, 0x00CBD0, 25, kOwn, 0 },
		{ 3, 0x00CBD0, 26, kOwn, 0 },
		{ 3, 0x00CBD0, 27, kOwn, 0 },
		{ 3, 0x00CBD0, 30, kPays, 0 },  // the donations: caps x50
		{ 3, 0x00CBD0, 31, kPays, 0 },  // x100
		{ 3, 0x00CBD0, 32, kPays, 0 },  // x250
		{ 3, 0x00CBD0, 33, kPays, 0 },  // x500
		{ 3, 0x00CBD0, 34, kPays, 0 },  // x1000
		{ 3, 0x00CBD0, 35, kPays, 0 },  // x2500
		{ 3, 0x00CBD0, 36, kPays, 0 },  // x5000
		{ 3, 0x00CBD0, 37, kPays, 0 },  // x10000
		{ 3, 0x00CBD0, 40, kOwn, 0 },  // the donation's objective shown
		{ 3, 0x00CBD0, 41, kOwn, 0 },
		{ 3, 0x00CBD0, 50, kOwn, 0 },  // the rank's global and its bonus spell (50 the robes too), the objective completed
		{ 3, 0x00CBD0, 51, kOwn, 0 },
		{ 3, 0x00CBD0, 52, kOwn, 0 },
		{ 3, 0x00CBD0, 53, kOwn, 0 },
		{ 3, 0x00CBD0, 54, kOwn, 0 },
		{ 3, 0x00CBD0, 55, kOwn, 0 },
		{ 3, 0x00CBD0, 56, kOwn, 0 },
		{ 3, 0x00CBD0, 57, kOwn, 0 },
		{ 3, 0x00CBD0, 100, kOwn, 0 },  // the objective failed
		{ 3, 0x014352, 90, kPays, 0 },  // DLC04RaidChaseOff: caps
		{ 3, 0x016E18, 95, 0, 10 },     // DLC04RaidSubdue
		{ 3, 0x019088, 90, kPays, 0 },  // DLC04RaidCoerce: caps
		{ 3, 0x01AABF, 110, kPays, 0 },  // DLC04DialogueSettlement: RemoveItem(DLC04_MeatBloodworm x10)
		{ 3, 0x0295C8, 50, 0, 100 },  // DLC04GZBattlezone
		{ 3, 0x0295C8, 60, 0, 20 },
		{ 3, 0x03684F, 6, kMoves, 50 },  // DLC04_KiddieKingdomMain: MoveTo temp6 (a quickstart; its 600, 700 and 740, the ghouls and Oswald turned on the player, are HOSTILE's)
		{ 3, 0x03684F, 791, 0, 5 },
	};

	// How many stages of the quests a friend's stage is set on (shared by their type, the finales, the dialogue quests,
	// QuestFilter::SAFE's and SILENT's, the radiants shared by their run; not PER_GAME) that are no kPays row themselves
	// set, through their fragments and the script functions those call, a few levels down and in their own quest or another, shared or
	// not, a stage with a kPays row, or take caps or empty the inventory themselves. A friend's game never sets a stage
	// that pays, but one of these runs its fragment here when the friend's is set, and the stage it sets pays in this game:
	// DialogueDiamondCity 64 and 300, Home Plate bought from Geneva or the mayor, set FFDiamondCity02 10, which takes the
	// price and gives the key and the house, so up to 0.13.1 a friend's purchase was paid for again by every player in the
	// session who had the caps. They are kFree rows now (PRICES): set here with the price at 0, so every player has the key
	// and the house and only the buyer pays. A kFree stage is not walked (its price is waived here), but one reached from
	// another stage counts as a payment, as a kPays one does. tools/questscan.ps1 -Safe walks every such stage that way and
	// prints the count last, as this line (tests/StageRulesTests.cpp checks it is 0). The scan does list
	// MS13MarowskiRevenge 380, which sets its own 400 (the caps), and does not count it: 400 takes them only if 380 is not
	// done (a GetStageDone branch the scan does not follow), and it is set from 380. Paste the scan's line here when it
	// changes; a count above 0 is a payment a friend's stage would make here.
	inline constexpr std::size_t SHARED_REACHES_PAYS = 0;

	// The purchases a friend's stage makes here for free (kFree): what a friend bought, every player in the session has, and
	// only the buyer pays. The script that sells it reads its price from a global (FFDiamondCity02 10: RemoveItem(Caps001,
	// ServiceCostBuyHouse.GetValueInt()), then the key, the doors' and the house's owner, its map marker), so the global is
	// 0 here while that script runs: from just before the stage is set until what is bought (the key, given right after
	// the RemoveItem) is in the player's inventory, or PRICE_TIMEOUT of play if it never comes. A purchase already made
	// here (its bought stage done) is set as it is: its script's SetStage does nothing. Form IDs in the quest's own file.
	struct Price
	{
		std::uint8_t  file;         // File
		std::uint32_t quest;        // the friend's stage
		std::uint16_t stage;
		std::uint32_t global;       // the price, read by the script that takes it
		std::uint32_t boughtQuest;  // the stage whose script takes the price and gives what is bought
		std::uint16_t boughtStage;
		std::uint32_t given;        // given by that script after it takes the price
	};
	inline constexpr Price PRICES[] = {
		{ 0, 0x003648, 64, 0x0D35FA, 0x14191C, 10, 0x141946 },   // DialogueDiamondCity, Geneva: ServiceCostBuyHouse, FFDiamondCity02 10, DmndPlayerHouseKey
		{ 0, 0x003648, 300, 0x0D35FA, 0x14191C, 10, 0x141946 },  // DialogueDiamondCity, the mayor: the same
		{ 0, 0x14191C, 10, 0x0D35FA, 0x14191C, 10, 0x141946 },   // FFDiamondCity02 itself (shared with sQuests = safe)
	};
	// How long a waived price stays 0 at most, in play (time paused or loading doesn't count: no script runs then). The script
	// that takes it normally runs within a second of the stage, but Papyrus runs it from a queue, which a heavily scripted
	// game can leave minutes behind: a price put back before the script runs would make the friend pay in full. The cost of
	// a long window is small: only this player's own purchase is free meanwhile, and no save or load holds the price at 0
	// (PriceWaiver).
	inline constexpr auto PRICE_TIMEOUT = std::chrono::minutes(5);

	inline const Price* FindPrice(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto it = std::ranges::find_if(PRICES, [&](const Price& a_price) {
			return a_price.file == static_cast<std::uint8_t>(a_file) && a_price.quest == a_quest && a_price.stage == a_stage;
		});
		return it != std::end(PRICES) ? &*it : nullptr;
	}

	// Whether a waived price goes back: what was bought has been given (the script is past its RemoveItem), or it has been
	// waived PRICE_TIMEOUT of play (the script never ran: the quest could not start here).
	inline bool PriceBack(bool a_given, Clock::duration a_played)
	{
		return a_given || a_played >= PRICE_TIMEOUT;
	}

	// The one price waived now, and when it goes back (game-free: QuestSync does what each call says to the global, and
	// tests/StageRulesTests.cpp walks the calls). The rule it keeps: a waiver is never dropped without its price put back, so
	// no save holds a global at 0. Each way a waiver ends (End) returns what to put back, and so does a save (BeforeSave),
	// which leaves the waiver on:
	//   given / timed out  Poll: what was bought is in the inventory, or PRICE_TIMEOUT of play went by
	//   a save             BeforeSave: back for the save; the next Poll holds it at 0 again
	//   a load begins      End: the loaded world has the value its save had, and nothing sets it to 0 after, though the
	//                      frame hold lets frames run in a load longer than its limit
	//   the main menu      Poll sees another main menu than the one at Begin (quit to the menu, then a new game, whose
	//                      kNewGame has no message before it); and End, after a load or a new game, as a last resort
	class PriceWaiver
	{
	public:
		struct Waived
		{
			std::uint32_t   global = 0;
			float           value = 0.0f;       // the global's own value, to put back
			std::uint32_t   given = 0;          // what the script gives after it takes the price
			std::uint32_t   givenBefore = 0;    // how many of it the player had when the waiver began
			std::uint32_t   quest = 0;          // the stage that began it (for the log)
			std::uint16_t   stage = 0;
			std::uint32_t   mainMenus = 0;      // the main menus opened by then (OpenMenus::MainMenus)
			Clock::duration played{};
		};

		enum class Do
		{
			kNothing,  // no waiver
			kHold,     // the global stays 0 (the caller sets it again if a save put it back)
			kRestore,  // the global gets its value back (Waived::value) and the waiver is over
		};
		enum class Why
		{
			kNone,
			kGiven,
			kTimeout,
			kMainMenu,
		};
		struct Polled
		{
			Do     what = Do::kNothing;
			Why    why = Why::kNone;
			Waived waived;  // as it was, for the log and the restore
		};

		[[nodiscard]] bool On() const { return waived.has_value(); }
		[[nodiscard]] const std::optional<Waived>& Current() const { return waived; }

		// A friend's purchase is being set: its price goes to 0 (the caller does it, whatever this returns true for). One
		// waiver at a time, of one global: another stage of the same purchase extends it (the window starts over, the
		// first value is kept); a purchase with another price is refused (false: its stage isn't set) rather than zero a
		// global that would never be put back.
		bool Begin(const Waived& a_new, Clock::time_point a_now)
		{
			if (waived) {
				if (waived->global != a_new.global) {
					return false;
				}
				waived->played = {};
			} else {
				waived = a_new;
				waived->played = {};
			}
			polled = a_now;
			return true;
		}

		// Each poll: a_paused is true while the game is paused or loading (no time played), a_given whether what was bought
		// has come, a_mainMenus the main menus opened so far.
		Polled Poll(Clock::time_point a_now, bool a_paused, bool a_given, std::uint32_t a_mainMenus)
		{
			if (!waived) {
				return {};
			}
			if (!a_paused) {
				waived->played += a_now - polled;
			}
			polled = a_now;
			Polled result{ Do::kHold, Why::kNone, *waived };
			if (a_mainMenus != waived->mainMenus) {
				result.what = Do::kRestore;
				result.why = Why::kMainMenu;
			} else if (PriceBack(a_given, waived->played)) {
				result.what = Do::kRestore;
				result.why = a_given ? Why::kGiven : Why::kTimeout;
			}
			if (result.what == Do::kRestore) {
				waived.reset();
			}
			return result;
		}

		// A save is coming: the value to put back for it, the waiver left on.
		[[nodiscard]] std::optional<Waived> BeforeSave() const { return waived; }

		// A load begins, or the world is loaded or new (nothing may be left on across it): the value to put back, the waiver
		// over.
		std::optional<Waived> End()
		{
			return std::exchange(waived, std::nullopt);
		}

	private:
		std::optional<Waived> waived;
		Clock::time_point     polled{};
	};

	// Stages whose script takes the player's controls away until a later stage gives them back. They
	// are only set once that stage is coming too (waiting here, or done), so the player is never left
	// without controls: in MQ206 "Molecular Level", 500 takes them at the teleporter and 1200 gives them
	// back in the Institute (1000 between them is the teleport itself, which is why 500 is a bridge
	// too: it starts MQ207, and nothing else does in normal play). Until the release is coming, only
	// the lock's own quest waits (see Verdict::kQuestWaits): a release that never comes (the friend's
	// game crashed in the teleport) mustn't hold up every other quest. The faction versions' 1400 set
	// MQ206's 500.
	struct Lock
	{
		std::uint8_t  file;
		std::uint32_t quest;
		std::uint16_t stage;
		std::uint32_t releaseQuest;
		std::uint16_t releaseStage;
	};
	inline constexpr Lock LOCKS[] = {
		{ 0, 0x0B1752, 500, 0x0B1752, 1200 },   // MQ206
		{ 0, 0x0B26BC, 1400, 0x0B1752, 1200 },  // MQ206RR
		{ 0, 0x0B26BD, 1400, 0x0B1752, 1200 },  // MQ206BoS
		{ 0, 0x0B26BE, 1400, 0x0B1752, 1200 },  // MQ206Min
	};

	inline std::optional<File> FileOf(std::string_view a_name)
	{
		const auto is = [&](std::string_view a_file) {
			return std::ranges::equal(a_name, a_file, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
		};
		if (is("Fallout4.esm")) {
			return File::kFallout4;
		}
		if (is("DLCCoast.esm")) {
			return File::kCoast;
		}
		if (is("DLCRobot.esm")) {
			return File::kRobot;
		}
		if (is("DLCNukaWorld.esm")) {
			return File::kNukaWorld;
		}
		return std::nullopt;
	}

	// The game file a StageRules::File names, as the load order lists it (tools/questscan.cs MASTERS: Fallout4.esm,
	// DLCCoast.esm, DLCRobot.esm, DLCNukaWorld.esm). The inverse of FileOf.
	inline constexpr std::string_view FileName(File a_file)
	{
		switch (a_file) {
		case File::kCoast:
			return "DLCCoast.esm";
		case File::kRobot:
			return "DLCRobot.esm";
		case File::kNukaWorld:
			return "DLCNukaWorld.esm";
		default:
			return "Fallout4.esm";
		}
	}

	inline const Rule* Find(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto key = std::tuple{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		const auto it = std::ranges::lower_bound(RULES, key, {}, [](const Rule& a_rule) { return std::tuple{ a_rule.file, a_rule.quest, a_rule.stage }; });
		return it != std::end(RULES) && std::tuple{ it->file, it->quest, it->stage } == key ? &*it : nullptr;
	}

	// The opening's quests in Fallout4.esm (see Story::InOpening): MQ102 "Out of Time" until the player first steps
	// out of Vault 111 (its 10, set by the trigger outside the vault's lift), and the vault's dialogue quest, whose
	// stages put the player in the cryopod (40), play the vault's scenes, open the vault door (95), give the last
	// look in the mirror (98), start the ride up (100, set by the lift's script, which then fades the screen to black
	// and leaves it so until the trigger at the top fades it back in) and greet the player outside (110). Every player
	// goes through these in their own game, in their own order (a dialogue quest's stages aren't in order: each game
	// has a different set), and set from a friend's anywhere else they pull the player back into the pod or start the
	// vault's scenes around them. Never reported, never set from a friend's.
	inline constexpr std::uint32_t MQ102 = 0x01CC2A;
	inline constexpr std::uint16_t MQ102_OUT_OF_THE_VAULT = 10;
	inline constexpr std::uint32_t DIALOGUE_VAULT111 = 0x01ED62;

	inline bool IsOpening(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		if (a_file != File::kFallout4) {
			return false;
		}
		return (a_quest == MQ102 && a_stage < MQ102_OUT_OF_THE_VAULT) || a_quest == DIALOGUE_VAULT111;
	}

	inline const Lock* FindLock(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto it = std::ranges::find_if(LOCKS, [&](const Lock& a_lock) {
			return a_lock.file == static_cast<std::uint8_t>(a_file) && a_lock.quest == a_quest && a_lock.stage == a_stage;
		});
		return it != std::end(LOCKS) ? &*it : nullptr;
	}

	// Stages whose own script sets a stage of another shared quest (directly, or through a stage of its own quest it
	// sets): DialogueCodsworthPostWar's 50, the talk with Codsworth in Sanctuary, sets Out of Time's 30 and starts his
	// search, its 200 sets Out of Time's 50; Out of Time's 40, the search over, sets Codsworth's 100. Set here ahead of
	// the other quest's stages that came before it, such a stage moves that quest past them, and they are then dropped
	// as already behind it (StageLog::Queue: a quest's stages only move forward): their scripts never run here, and the
	// objectives they show and complete are left as they were. So one is only set once its quest's stages are the
	// oldest still waiting (StageLog::Queue::Poll). Generated with tools/questscan.ps1 -SetsOther from the quest
	// fragment scripts of Fallout4.esm, DLCCoast.esm, DLCRobot.esm and DLCNukaWorld.esm: a fragment's SetStage on a
	// quest property other than its own quest, kept if that property names a shared quest or a quest it can't be
	// matched to (its name isn't the quest's). With the quests the host's sQuests = safe shares taken for shared
	// (QuestFilter::SAFE; tools/questscan.ps1 -Safe -SetsOther): theirs, and the shared quests' stages that set one of
	// theirs (MS19's that move MS19MiscSeeForsythe on, Far Harbor's main quest's that move its own side quests), and
	// the same for the objective-less ones it shares (QuestFilter::SILENT: MS11's that start the Castle's waves, MQ106
	// 900's BoSEnable 10, the Prydwen's arrival). In a session that doesn't share them, an extra row only holds its
	// stage until what came before it is set. Sorted by file, quest and stage.
	struct Setter
	{
		std::uint8_t  file;
		std::uint32_t quest;
		std::uint16_t stage;
	};
	inline constexpr Setter SETS_ANOTHER[] = {
		{ 0, 0x003648, 5 }, { 0, 0x003648, 17 }, { 0, 0x003648, 64 }, { 0, 0x003648, 170 }, { 0, 0x003648, 300 }, { 0, 0x003648, 500 }, { 0, 0x003648, 600 },  // DialogueDiamondCity
		{ 0, 0x0179F3, 5 }, { 0, 0x0179F3, 50 }, { 0, 0x0179F3, 200 },  // DialogueConcordArea
		{ 0, 0x0190BC, 0 }, { 0, 0x0190BC, 22 }, { 0, 0x0190BC, 23 }, { 0, 0x0190BC, 25 }, { 0, 0x0190BC, 30 }, { 0, 0x0190BC, 40 }, { 0, 0x0190BC, 50 },  // DialogueDiamondCityEntrance
		{ 0, 0x0190BC, 52 }, { 0, 0x0190BC, 55 }, { 0, 0x0190BC, 90 }, { 0, 0x0190BC, 200 }, { 0, 0x0190BC, 400 }, { 0, 0x0190BC, 1000 },
		{ 0, 0x01CC2A, 0 }, { 0, 0x01CC2A, 8 }, { 0, 0x01CC2A, 10 }, { 0, 0x01CC2A, 34 }, { 0, 0x01CC2A, 38 }, { 0, 0x01CC2A, 40 }, { 0, 0x01CC2A, 55 },  // MQ102
		{ 0, 0x01CC2A, 65 }, { 0, 0x01CC2A, 70 }, { 0, 0x01CC2A, 75 }, { 0, 0x01CC2A, 80 }, { 0, 0x01CC2A, 85 }, { 0, 0x01CC2A, 89 }, { 0, 0x01CC2A, 90 },
		{ 0, 0x01CC2A, 100 }, { 0, 0x01CC2A, 110 }, { 0, 0x01CC2A, 115 }, { 0, 0x01CC2A, 122 }, { 0, 0x01CC2A, 123 }, { 0, 0x01CC2A, 130 }, { 0, 0x01CC2A, 140 },
		{ 0, 0x01CC2A, 150 }, { 0, 0x01CC2A, 1000 },
		{ 0, 0x01D727, 10 }, { 0, 0x01D727, 40 }, { 0, 0x01D727, 500 },  // FFDiamondCity01
		{ 0, 0x01D806, 10 }, { 0, 0x01D806, 77 }, { 0, 0x01D806, 79 },  // DN092
		{ 0, 0x01E64E, 50 }, { 0, 0x01E64E, 200 }, { 0, 0x01E64E, 275 },  // DialogueCodsworthPostWar
		{ 0, 0x01F25E, 50 }, { 0, 0x01F25E, 205 }, { 0, 0x01F25E, 210 },  // MQ104
		{ 0, 0x02074B, 300 }, { 0, 0x02074B, 500 }, { 0, 0x02074B, 510 },  // CZ
		{ 0, 0x0229E5, 100 },  // MQ103
		{ 0, 0x0229E6, 0 }, { 0, 0x0229E6, 62 }, { 0, 0x0229E6, 200 }, { 0, 0x0229E6, 600 }, { 0, 0x0229E6, 1000 },  // MQ105
		{ 0, 0x0229E7, 0 }, { 0, 0x0229E7, 10 }, { 0, 0x0229E7, 500 }, { 0, 0x0229E7, 900 }, { 0, 0x0229E7, 1000 }, { 0, 0x0229E7, 1150 }, { 0, 0x0229E7, 2000 },  // MQ106
		{ 0, 0x0229E8, 0 }, { 0, 0x0229E8, 15 }, { 0, 0x0229E8, 17 }, { 0, 0x0229E8, 100 }, { 0, 0x0229E8, 110 }, { 0, 0x0229E8, 150 },  // MQ201
		{ 0, 0x0229E9, 0 }, { 0, 0x0229E9, 300 },  // MQ202
		{ 0, 0x0229EA, 0 }, { 0, 0x0229EA, 20 },  // MQ203
		{ 0, 0x0229EB, 40 }, { 0, 0x0229EB, 100 },  // MQ205
		{ 0, 0x0229EC, 1 }, { 0, 0x0229EC, 5 }, { 0, 0x0229EC, 10 },  // MQ207
		{ 0, 0x0229EE, 0 }, { 0, 0x0229EE, 1 }, { 0, 0x0229EE, 2 }, { 0, 0x0229EE, 3 }, { 0, 0x0229EE, 300 }, { 0, 0x0229EE, 310 }, { 0, 0x0229EE, 355 },  // MQ302
		{ 0, 0x0229EE, 357 }, { 0, 0x0229EE, 360 }, { 0, 0x0229EE, 400 }, { 0, 0x0229EE, 425 }, { 0, 0x0229EE, 430 }, { 0, 0x0229EE, 460 }, { 0, 0x0229EE, 500 },
		{ 0, 0x0229EE, 600 }, { 0, 0x0229EE, 700 }, { 0, 0x0229EE, 710 }, { 0, 0x0229EE, 720 }, { 0, 0x0229EE, 730 }, { 0, 0x0229EE, 740 }, { 0, 0x0229EE, 800 },
		{ 0, 0x0229EE, 900 }, { 0, 0x0229EE, 1000 },
		{ 0, 0x0229F7, 9 }, { 0, 0x0229F7, 1000 },  // MS02
		{ 0, 0x0229FD, 10 }, { 0, 0x0229FD, 19 },  // MS07b
		{ 0, 0x0229FE, 0 }, { 0, 0x0229FE, 1 }, { 0, 0x0229FE, 190 }, { 0, 0x0229FE, 290 }, { 0, 0x0229FE, 300 }, { 0, 0x0229FE, 305 }, { 0, 0x0229FE, 500 },  // MS07c
		{ 0, 0x0229FE, 600 },
		{ 0, 0x022A00, 100 }, { 0, 0x022A00, 150 }, { 0, 0x022A00, 340 }, { 0, 0x022A00, 360 }, { 0, 0x022A00, 400 }, { 0, 0x022A00, 410 }, { 0, 0x022A00, 450 },  // MS09
		{ 0, 0x022A00, 480 }, { 0, 0x022A00, 500 }, { 0, 0x022A00, 555 }, { 0, 0x022A00, 560 }, { 0, 0x022A00, 600 }, { 0, 0x022A00, 640 }, { 0, 0x022A00, 650 },
		{ 0, 0x022A00, 750 }, { 0, 0x022A00, 800 }, { 0, 0x022A00, 850 },
		{ 0, 0x022A02, 170 }, { 0, 0x022A02, 200 }, { 0, 0x022A02, 775 }, { 0, 0x022A02, 1050 }, { 0, 0x022A02, 1100 },  // MS11
		{ 0, 0x022A05, 10 },  // MS14
		{ 0, 0x022A07, 1 }, { 0, 0x022A07, 20 }, { 0, 0x022A07, 59 }, { 0, 0x022A07, 89 }, { 0, 0x022A07, 199 }, { 0, 0x022A07, 200 }, { 0, 0x022A07, 230 },  // MS16
		{ 0, 0x022A07, 260 }, { 0, 0x022A07, 270 },
		{ 0, 0x022A0A, 4 }, { 0, 0x022A0A, 9 }, { 0, 0x022A0A, 35 }, { 0, 0x022A0A, 60 }, { 0, 0x022A0A, 100 }, { 0, 0x022A0A, 150 }, { 0, 0x022A0A, 400 },  // MS19
		{ 0, 0x026340, 3 }, { 0, 0x026340, 4 }, { 0, 0x026340, 30 }, { 0, 0x026340, 50 },  // DN121
		{ 0, 0x026AA0, 15 },  // DN120
		{ 0, 0x02B4E9, 350 },  // Inst303
		{ 0, 0x02BF20, 0 }, { 0, 0x02BF20, 200 },  // BoS200
		{ 0, 0x02BF21, 0 }, { 0, 0x02BF21, 1 }, { 0, 0x02BF21, 2 }, { 0, 0x02BF21, 3 }, { 0, 0x02BF21, 4 }, { 0, 0x02BF21, 5 }, { 0, 0x02BF21, 10 },  // BoS201
		{ 0, 0x02BF21, 15 }, { 0, 0x02BF21, 100 }, { 0, 0x02BF21, 140 }, { 0, 0x02BF21, 150 }, { 0, 0x02BF21, 160 }, { 0, 0x02BF21, 170 }, { 0, 0x02BF21, 255 },
		{ 0, 0x02C8CB, 100 }, { 0, 0x02C8CB, 500 }, { 0, 0x02C8CB, 600 }, { 0, 0x02C8CB, 700 }, { 0, 0x02C8CB, 1070 }, { 0, 0x02C8CB, 1090 },  // RR302
		{ 0, 0x033582, 35 }, { 0, 0x033582, 40 },  // DialogueGoodneighbor
		{ 0, 0x03A457, 0 },  // Min02
		{ 0, 0x03F244, 14 }, { 0, 0x03F244, 15 }, { 0, 0x03F244, 102 }, { 0, 0x03F244, 300 }, { 0, 0x03F244, 400 },  // DialogueVault81
		{ 0, 0x043275, 1 }, { 0, 0x043275, 2 }, { 0, 0x043275, 100 }, { 0, 0x043275, 110 }, { 0, 0x043275, 200 }, { 0, 0x043275, 300 }, { 0, 0x043275, 350 },  // RR303
		{ 0, 0x043275, 1010 }, { 0, 0x043275, 1020 }, { 0, 0x043275, 1030 }, { 0, 0x043275, 1050 }, { 0, 0x043275, 1100 }, { 0, 0x043275, 1150 }, { 0, 0x043275, 1300 },
		{ 0, 0x043275, 1400 },
		{ 0, 0x04402C, 0 }, { 0, 0x04402C, 1 }, { 0, 0x04402C, 2 }, { 0, 0x04402C, 3 }, { 0, 0x04402C, 4 }, { 0, 0x04402C, 5 }, { 0, 0x04402C, 6 },  // BoSM02
		{ 0, 0x04402C, 7 }, { 0, 0x04402C, 8 }, { 0, 0x04402C, 9 }, { 0, 0x04402C, 10 }, { 0, 0x04402C, 430 },
		{ 0, 0x0456E8, 12 }, { 0, 0x0456E8, 30 },  // FFDiamondCity07
		{ 0, 0x0459D2, 800 }, { 0, 0x0459D2, 1050 }, { 0, 0x0459D2, 1100 },  // RR101
		{ 0, 0x046BB5, 255 },  // DN131
		{ 0, 0x047E90, 100 }, { 0, 0x047E90, 150 },  // MS09Intro
		{ 0, 0x04AD05, 5 }, { 0, 0x04AD05, 100 }, { 0, 0x04AD05, 150 },  // DN101
		{ 0, 0x04AF6C, 20 },  // BoS201Radio
		{ 0, 0x0503B8, 50 },  // MS09Mission01
		{ 0, 0x052392, 5 },  // DialogueInstituteReactScene1
		{ 0, 0x0537FC, 0 }, { 0, 0x0537FC, 10 }, { 0, 0x0537FC, 20 }, { 0, 0x0537FC, 30 }, { 0, 0x0537FC, 40 }, { 0, 0x0537FC, 50 }, { 0, 0x0537FC, 255 },  // BoS201B
		{ 0, 0x0537FF, 0 }, { 0, 0x0537FF, 1 }, { 0, 0x0537FF, 2 }, { 0, 0x0537FF, 20 }, { 0, 0x0537FF, 255 },  // BoS202
		{ 0, 0x05BC93, 1 }, { 0, 0x05BC93, 450 }, { 0, 0x05BC93, 475 },  // DialogueRailroad
		{ 0, 0x05DDAB, 0 }, { 0, 0x05DDAB, 10 }, { 0, 0x05DDAB, 20 }, { 0, 0x05DDAB, 30 }, { 0, 0x05DDAB, 40 }, { 0, 0x05DDAB, 50 }, { 0, 0x05DDAB, 52 },  // BoS100
		{ 0, 0x05DDAB, 55 }, { 0, 0x05DDAB, 255 }, { 0, 0x05DDAB, 275 },
		{ 0, 0x05DEE4, 0 }, { 0, 0x05DEE4, 1 }, { 0, 0x05DEE4, 20 }, { 0, 0x05DEE4, 100 }, { 0, 0x05DEE4, 220 }, { 0, 0x05DEE4, 230 }, { 0, 0x05DEE4, 250 },  // Min01
		{ 0, 0x05DEE4, 1500 }, { 0, 0x05DEE4, 2000 }, { 0, 0x05DEE4, 2010 },
		{ 0, 0x05F76C, 117 },  // DialogueBunkerHillGenericNPCs
		{ 0, 0x05FD90, 700 },  // RRM01
		{ 0, 0x064C66, 250 },  // DN009_MQ205
		{ 0, 0x064EC7, 0 }, { 0, 0x064EC7, 10 }, { 0, 0x064EC7, 40 }, { 0, 0x064EC7, 50 },  // BoSR01
		{ 0, 0x068D7B, 1 }, { 0, 0x068D7B, 2 }, { 0, 0x068D7B, 30 }, { 0, 0x068D7B, 100 },  // BoS100Fight
		{ 0, 0x06B500, 0 }, { 0, 0x06B500, 5 }, { 0, 0x06B500, 15 }, { 0, 0x06B500, 200 },  // MQ204
		{ 0, 0x06D41F, 500 },  // MQ201Piper
		{ 0, 0x06F102, 5 }, { 0, 0x06F102, 10 },  // BoS100Radio
		{ 0, 0x06F5C1, 0 }, { 0, 0x06F5C1, 1 }, { 0, 0x06F5C1, 2 }, { 0, 0x06F5C1, 50 }, { 0, 0x06F5C1, 320 }, { 0, 0x06F5C1, 455 }, { 0, 0x06F5C1, 555 },  // BoS101
		{ 0, 0x06FA37, 0 }, { 0, 0x06FA37, 800 }, { 0, 0x06FA37, 1050 },  // RR102
		{ 0, 0x07CC61, 10 },  // RR301
		{ 0, 0x084E40, 380 },  // MS13MarowskiRevenge
		{ 0, 0x0980FD, 15 },  // MQ205_InstituteRadio
		{ 0, 0x099E5D, 13 },  // Vault81Tour
		{ 0, 0x09FF4E, 0 }, { 0, 0x09FF4E, 30 }, { 0, 0x09FF4E, 255 },  // BoS203
		{ 0, 0x0A274B, 200 },  // Inst305
		{ 0, 0x0A8257, 0 }, { 0, 0x0A8257, 1600 }, { 0, 0x0A8257, 1700 }, { 0, 0x0A8257, 1800 }, { 0, 0x0A8257, 2000 },  // InstM02
		{ 0, 0x0A8258, 50 }, { 0, 0x0A8258, 95 }, { 0, 0x0A8258, 200 },  // Inst302
		{ 0, 0x0AA778, 0 },  // Min03
		{ 0, 0x0ABF55, 500 },  // DN029FirstWave
		{ 0, 0x0AC7B3, 20 }, { 0, 0x0AC7B3, 25 }, { 0, 0x0AC7B3, 30 }, { 0, 0x0AC7B3, 200 },  // Inst306
		{ 0, 0x0ADCA6, 400 }, { 0, 0x0ADCA6, 450 },  // DN029SecondWaveA
		{ 0, 0x0ADCE5, 0 }, { 0, 0x0ADCE5, 1 }, { 0, 0x0ADCE5, 2 }, { 0, 0x0ADCE5, 3 }, { 0, 0x0ADCE5, 4 },  // BoSM04
		{ 0, 0x0AE44C, 500 }, { 0, 0x0AE44C, 1000 },  // DN029SecondWaveB
		{ 0, 0x0AE51C, 0 }, { 0, 0x0AE51C, 1 }, { 0, 0x0AE51C, 2 }, { 0, 0x0AE51C, 3 }, { 0, 0x0AE51C, 4 }, { 0, 0x0AE51C, 65 }, { 0, 0x0AE51C, 75 },  // BoS301
		{ 0, 0x0AE51C, 76 }, { 0, 0x0AE51C, 90 }, { 0, 0x0AE51C, 170 }, { 0, 0x0AE51C, 255 },
		{ 0, 0x0B1752, 0 }, { 0, 0x0B1752, 20 }, { 0, 0x0B1752, 500 }, { 0, 0x0B1752, 1050 }, { 0, 0x0B1752, 1200 },  // MQ206
		{ 0, 0x0B1D79, 0 }, { 0, 0x0B1D79, 1 }, { 0, 0x0B1D79, 30 }, { 0, 0x0B1D79, 116 }, { 0, 0x0B1D79, 119 }, { 0, 0x0B1D79, 120 }, { 0, 0x0B1D79, 145 },  // BoSM01
		{ 0, 0x0B1D79, 149 }, { 0, 0x0B1D79, 150 }, { 0, 0x0B1D79, 200 },
		{ 0, 0x0B1F1D, 100 }, { 0, 0x0B1F1D, 400 },  // RRR01a
		{ 0, 0x0B26BC, 1100 }, { 0, 0x0B26BC, 1200 }, { 0, 0x0B26BC, 1400 }, { 0, 0x0B26BC, 1475 }, { 0, 0x0B26BC, 1500 },  // MQ206RR
		{ 0, 0x0B26BD, 1100 }, { 0, 0x0B26BD, 1200 }, { 0, 0x0B26BD, 1375 }, { 0, 0x0B26BD, 1400 }, { 0, 0x0B26BD, 1500 },  // MQ206BoS
		{ 0, 0x0B26BE, 1100 }, { 0, 0x0B26BE, 1200 }, { 0, 0x0B26BE, 1350 }, { 0, 0x0B26BE, 1400 }, { 0, 0x0B26BE, 1450 }, { 0, 0x0B26BE, 1475 }, { 0, 0x0B26BE, 1500 },  // MQ206Min
		{ 0, 0x0B26C2, 300 },  // RRR04
		{ 0, 0x0B2D48, 5 }, { 0, 0x0B2D48, 10 }, { 0, 0x0B2D48, 15 }, { 0, 0x0B2D48, 20 }, { 0, 0x0B2D48, 25 }, { 0, 0x0B2D48, 100 }, { 0, 0x0B2D48, 1050 },  // RR201
		{ 0, 0x0B2D48, 8900 },
		{ 0, 0x0B8464, 500 },  // V81_00_Intro
		{ 0, 0x0B926B, 120 },  // RRR03
		{ 0, 0x0B9F9D, 0 }, { 0, 0x0B9F9D, 1 }, { 0, 0x0B9F9D, 2 }, { 0, 0x0B9F9D, 20 }, { 0, 0x0B9F9D, 255 },  // BoS302
		{ 0, 0x0BACE9, 60 }, { 0, 0x0BACE9, 70 }, { 0, 0x0BACE9, 80 },  // BoSM01DistressSignals
		{ 0, 0x0BACFF, 100 }, { 0, 0x0BACFF, 200 },  // Inst307
		{ 0, 0x0BAD00, 50 }, { 0, 0x0BAD00, 200 },  // Inst308
		{ 0, 0x0BDB04, 0 }, { 0, 0x0BDB04, 15 }, { 0, 0x0BDB04, 20 }, { 0, 0x0BDB04, 40 }, { 0, 0x0BDB04, 50 }, { 0, 0x0BDB04, 100 }, { 0, 0x0BDB04, 180 },  // MinDefendCastle
		{ 0, 0x0BDB04, 200 },
		{ 0, 0x0C30DC, 0 }, { 0, 0x0C30DC, 10 }, { 0, 0x0C30DC, 40 }, { 0, 0x0C30DC, 50 },  // BosR02
		{ 0, 0x0C8675, 0 },  // BoSR03
		{ 0, 0x0CF3E2, 0 },  // BoSR04
		{ 0, 0x0CF3E3, 10 }, { 0, 0x0CF3E3, 20 },  // BoSR04_AO_Scribe
		{ 0, 0x0D1EB2, 0 },  // BoSR05
		{ 0, 0x0DDFDB, 0 }, { 0, 0x0DDFDB, 1 }, { 0, 0x0DDFDB, 15 }, { 0, 0x0DDFDB, 30 }, { 0, 0x0DDFDB, 50 },  // Min301
		{ 0, 0x0DFB3C, 0 }, { 0, 0x0DFB3C, 500 },  // MinDestBoS
		{ 0, 0x0E2058, 300 }, { 0, 0x0E2058, 1000 },  // Inst301
		{ 0, 0x0FDC8C, 0 }, { 0, 0x0FDC8C, 1 }, { 0, 0x0FDC8C, 2 }, { 0, 0x0FDC8C, 3 }, { 0, 0x0FDC8C, 255 },  // BoS303
		{ 0, 0x0FF3C2, 600 }, { 0, 0x0FF3C2, 1000 },  // DN011
		{ 0, 0x107A1C, 0 }, { 0, 0x107A1C, 255 },  // BoS302B
		{ 0, 0x108EC0, 100 },  // FFGoodneighbor01
		{ 0, 0x10C64A, 10 }, { 0, 0x10C64A, 350 }, { 0, 0x10C64A, 360 }, { 0, 0x10C64A, 1910 }, { 0, 0x10C64A, 2000 },  // MQ302Min
		{ 0, 0x10C64B, 1000 },  // MQ302BoS
		{ 0, 0x10C64C, 10 }, { 0, 0x10C64C, 325 }, { 0, 0x10C64C, 326 }, { 0, 0x10C64C, 950 }, { 0, 0x10C64C, 1000 },  // MQ302RR
		{ 0, 0x11B36E, 475 },  // MinRecruit00
		{ 0, 0x122221, 300 },  // FFBunkerHill02
		{ 0, 0x1323AB, 0 }, { 0, 0x1323AB, 90 }, { 0, 0x1323AB, 200 },  // FFGoodneighbor07
		{ 0, 0x134297, 0 },  // MQ106Investigations
		{ 0, 0x136329, 0 },  // Min207
		{ 0, 0x138442, 20 },  // CZPostQuest
		{ 0, 0x13C640, 255 },  // BoS204
		{ 0, 0x142EDC, 0 },  // BoSM00
		{ 0, 0x142FD3, 1 }, { 0, 0x142FD3, 20 }, { 0, 0x142FD3, 25 }, { 0, 0x142FD3, 27 }, { 0, 0x142FD3, 30 }, { 0, 0x142FD3, 100 }, { 0, 0x142FD3, 200 },  // RR304
		{ 0, 0x142FD3, 300 }, { 0, 0x142FD3, 400 }, { 0, 0x142FD3, 500 }, { 0, 0x142FD3, 600 }, { 0, 0x142FD3, 700 }, { 0, 0x142FD3, 800 }, { 0, 0x142FD3, 900 },
		{ 0, 0x142FD3, 950 }, { 0, 0x142FD3, 1000 }, { 0, 0x142FD3, 1100 }, { 0, 0x142FD3, 1200 }, { 0, 0x142FD3, 1300 }, { 0, 0x142FD3, 1400 }, { 0, 0x142FD3, 1500 },
		{ 0, 0x142FD3, 1600 },
		{ 0, 0x146C84, 200 },  // MS10b
		{ 0, 0x14A24C, 300 },  // MS07cTapes
		{ 0, 0x14B717, 7 }, { 0, 0x14B717, 25 }, { 0, 0x14B717, 50 }, { 0, 0x14B717, 125 }, { 0, 0x14B717, 150 }, { 0, 0x14B717, 157 }, { 0, 0x14B717, 225 },  // MS05B
		{ 0, 0x14B717, 250 }, { 0, 0x14B717, 252 }, { 0, 0x14B717, 253 }, { 0, 0x14B717, 280 }, { 0, 0x14B717, 400 }, { 0, 0x14B717, 440 }, { 0, 0x14B717, 495 },
		{ 0, 0x14B717, 500 }, { 0, 0x14B717, 510 }, { 0, 0x14B717, 750 }, { 0, 0x14B717, 800 }, { 0, 0x14B717, 1100 },
		{ 0, 0x14DCFE, 20 },  // MS07bGrasshopper
		{ 0, 0x153307, 0 },  // DialogueRailroadLiamFuneral
		{ 0, 0x15BD39, 300 },  // InstMassFusion
		{ 0, 0x162A3E, 10 },  // BoS303LL
		{ 0, 0x16854F, 9 }, { 0, 0x16854F, 10 },  // MS05BTapes
		{ 0, 0x168550, 20 }, { 0, 0x168550, 30 },  // MS05BPostQuest
		{ 0, 0x173ED9, 0 }, { 0, 0x173ED9, 1 }, { 0, 0x173ED9, 2 }, { 0, 0x173ED9, 10 }, { 0, 0x173ED9, 20 }, { 0, 0x173ED9, 25 }, { 0, 0x173ED9, 50 },  // BoS304
		{ 0, 0x173ED9, 255 },
		{ 0, 0x1761C7, 20 }, { 0, 0x1761C7, 30 }, { 0, 0x1761C7, 40 },  // DialogueDrinkingBuddy
		{ 0, 0x1764DF, 5 },  // FFDiamondCity10
		{ 0, 0x17CA39, 10 },  // Inst307ChangeLocation
		{ 0, 0x17D18C, 700 },  // DialogueGoodneighborRufus
		{ 0, 0x182EAE, 0 }, { 0, 0x182EAE, 10 }, { 0, 0x182EAE, 255 },  // BoS305
		{ 0, 0x1851A0, 10 },  // BoSEnable
		{ 0, 0x19E746, 100 },  // RRAct3Lost
		{ 0, 0x1A001C, 70 }, { 0, 0x1A001C, 75 },  // Min00
		{ 0, 0x2041BC, 25 },  // DialogueDiamondCityExtra
		{ 0, 0x225917, 10 },  // BoSPostQuestMMWon
		{ 1, 0x001B3E, 10 }, { 1, 0x001B3E, 100 },  // DLC03MQ00
		{ 1, 0x001B3F, 400 }, { 1, 0x001B3F, 500 },  // DLC03MQ01
		{ 1, 0x001B40, 0 }, { 1, 0x001B40, 1 }, { 1, 0x001B40, 200 }, { 1, 0x001B40, 210 }, { 1, 0x001B40, 220 }, { 1, 0x001B40, 400 }, { 1, 0x001B40, 600 },  // DLC03MQ02
		{ 1, 0x001B40, 650 }, { 1, 0x001B40, 700 }, { 1, 0x001B40, 800 },
		{ 1, 0x001B41, 100 }, { 1, 0x001B41, 250 }, { 1, 0x001B41, 350 }, { 1, 0x001B41, 400 }, { 1, 0x001B41, 500 }, { 1, 0x001B41, 1000 },  // DLC03MQ03
		{ 1, 0x001B42, 1 }, { 1, 0x001B42, 15 }, { 1, 0x001B42, 150 }, { 1, 0x001B42, 250 }, { 1, 0x001B42, 350 }, { 1, 0x001B42, 450 }, { 1, 0x001B42, 550 },  // DLC03MQ04
		{ 1, 0x001B42, 1000 },
		{ 1, 0x001B43, 0 }, { 1, 0x001B43, 1 }, { 1, 0x001B43, 50 }, { 1, 0x001B43, 60 }, { 1, 0x001B43, 105 }, { 1, 0x001B43, 120 }, { 1, 0x001B43, 125 },  // DLC03MQ05
		{ 1, 0x001B43, 220 }, { 1, 0x001B43, 230 }, { 1, 0x001B43, 240 }, { 1, 0x001B43, 250 }, { 1, 0x001B43, 500 }, { 1, 0x001B43, 8900 },
		{ 1, 0x001B44, 1 }, { 1, 0x001B44, 2 }, { 1, 0x001B44, 3 }, { 1, 0x001B44, 200 }, { 1, 0x001B44, 250 }, { 1, 0x001B44, 500 },  // DLC03MQ06
		{ 1, 0x004C8E, 30 }, { 1, 0x004C8E, 52 }, { 1, 0x004C8E, 55 }, { 1, 0x004C8E, 57 }, { 1, 0x004C8E, 60 }, { 1, 0x004C8E, 75 }, { 1, 0x004C8E, 90 },  // DLC03CoA_DialogueNucleus
		{ 1, 0x004C8E, 92 }, { 1, 0x004C8E, 96 }, { 1, 0x004C8E, 97 }, { 1, 0x004C8E, 98 }, { 1, 0x004C8E, 99 }, { 1, 0x004C8E, 115 }, { 1, 0x004C8E, 117 },
		{ 1, 0x004C8E, 119 }, { 1, 0x004C8E, 120 }, { 1, 0x004C8E, 124 }, { 1, 0x004C8E, 125 }, { 1, 0x004C8E, 130 },
		{ 1, 0x004C8F, 3 }, { 1, 0x004C8F, 100 },  // DLC03CoA_DialogueNucleusEntrance
		{ 1, 0x004F2C, 145 }, { 1, 0x004F2C, 150 },  // DLC03MQPostQuest
		{ 1, 0x005BD7, 700 }, { 1, 0x005BD7, 710 }, { 1, 0x005BD7, 720 }, { 1, 0x005BD7, 9000 },  // DLC03DialogueFarHarbor
		{ 1, 0x00A990, 0 }, { 1, 0x00A990, 100 }, { 1, 0x00A990, 250 }, { 1, 0x00A990, 290 }, { 1, 0x00A990, 300 }, { 1, 0x00A990, 375 },  // DLC03FarHarborFF03a
		{ 1, 0x00A991, 100 }, { 1, 0x00A991, 200 }, { 1, 0x00A991, 500 }, { 1, 0x00A991, 9000 },  // DLC03FarHarborS01
		{ 1, 0x00A992, 1 }, { 1, 0x00A992, 50 }, { 1, 0x00A992, 100 }, { 1, 0x00A992, 500 },  // DLC03FarHarborM01
		{ 1, 0x00AB19, 150 }, { 1, 0x00AB19, 200 }, { 1, 0x00AB19, 350 },  // DLC03FarHarborS01Defend
		{ 1, 0x00AEC3, 1 }, { 1, 0x00AEC3, 100 }, { 1, 0x00AEC3, 200 }, { 1, 0x00AEC3, 300 }, { 1, 0x00AEC3, 325 }, { 1, 0x00AEC3, 400 },  // DLC03FarHarborS02
		{ 1, 0x00AEC4, 50 }, { 1, 0x00AEC4, 100 }, { 1, 0x00AEC4, 300 }, { 1, 0x00AEC4, 350 },  // DLC03FarHarborFF03b
		{ 1, 0x00AFB1, 0 }, { 1, 0x00AFB1, 20 }, { 1, 0x00AFB1, 150 }, { 1, 0x00AFB1, 725 },  // DLC03AtomM01
		{ 1, 0x01055C, 10 }, { 1, 0x01055C, 200 },  // DLC03CoA_FFNucleus01
		{ 1, 0x013F39, 40 }, { 1, 0x013F39, 200 }, { 1, 0x013F39, 1000 },  // DLC03Acadia_BoS
		{ 1, 0x013F3A, 40 }, { 1, 0x013F3A, 200 }, { 1, 0x013F3A, 1000 },  // DLC03Acadia_Institute
		{ 1, 0x01536F, 600 }, { 1, 0x01536F, 700 }, { 1, 0x01536F, 750 },  // DLC03AcadiaM01
		{ 1, 0x01B0AF, 100 }, { 1, 0x01B0AF, 700 },  // DLC03AtomM02
		{ 1, 0x01CBFA, 100 }, { 1, 0x01CBFA, 102 }, { 1, 0x01CBFA, 104 }, { 1, 0x01CBFA, 106 }, { 1, 0x01CBFA, 108 }, { 1, 0x01CBFA, 110 }, { 1, 0x01CBFA, 115 },  // DLC03MQ04_01
		{ 1, 0x01CBFA, 120 }, { 1, 0x01CBFA, 160 }, { 1, 0x01CBFA, 250 }, { 1, 0x01CBFA, 300 },
		{ 1, 0x01CBFE, 100 }, { 1, 0x01CBFE, 250 }, { 1, 0x01CBFE, 300 },  // DLC03MQ04_02
		{ 1, 0x01D165, 100 }, { 1, 0x01D165, 250 }, { 1, 0x01D165, 300 },  // DLC03MQ04_03
		{ 1, 0x01E3FE, 100 },  // DLC03MQ04_05
		{ 1, 0x01E3FF, 100 }, { 1, 0x01E3FF, 250 }, { 1, 0x01E3FF, 300 },  // DLC03MQ04_04
		{ 1, 0x02039A, 0 }, { 1, 0x02039A, 50 }, { 1, 0x02039A, 100 }, { 1, 0x02039A, 300 }, { 1, 0x02039A, 350 },  // DLC03FarHarborFF03c
		{ 1, 0x020BF1, 250 }, { 1, 0x020BF1, 650 }, { 1, 0x020BF1, 670 },  // DLC03DialogueDiMA
		{ 1, 0x02127F, 1 }, { 1, 0x02127F, 95 }, { 1, 0x02127F, 255 }, { 1, 0x02127F, 270 }, { 1, 0x02127F, 290 }, { 1, 0x02127F, 300 }, { 1, 0x02127F, 750 },  // DLC03MQ06a
		{ 1, 0x02127F, 755 }, { 1, 0x02127F, 760 }, { 1, 0x02127F, 765 }, { 1, 0x02127F, 770 }, { 1, 0x02127F, 780 }, { 1, 0x02127F, 785 }, { 1, 0x02127F, 790 },
		{ 1, 0x02127F, 1000 },
		{ 1, 0x02399F, 0 }, { 1, 0x02399F, 1 }, { 1, 0x02399F, 50 }, { 1, 0x02399F, 100 }, { 1, 0x02399F, 500 },  // DLC03FarHarborS03
		{ 1, 0x023ACA, 100 }, { 1, 0x023ACA, 400 },  // DLC03FarHarborFF02
		{ 1, 0x023ACB, 100 }, { 1, 0x023ACB, 400 },  // DLC03FarHarborFF01
		{ 1, 0x02B4FF, 20 }, { 1, 0x02B4FF, 30 },  // DLC03CoA_FFNucleus03_PumpHandler
		{ 1, 0x02B500, 10 }, { 1, 0x02B500, 30 }, { 1, 0x02B500, 50 },  // DLC03CoA_FFNucleus03
		{ 1, 0x02BBDB, 10 },  // DLC03DialogueKasumiAcadia
		{ 1, 0x02C8B3, 10 }, { 1, 0x02C8B3, 200 }, { 1, 0x02C8B3, 300 }, { 1, 0x02C8B3, 310 }, { 1, 0x02C8B3, 320 }, { 1, 0x02C8B3, 500 },  // DLC03CoA_FFNucleus02
		{ 1, 0x0316F2, 500 }, { 1, 0x0316F2, 503 }, { 1, 0x0316F2, 600 }, { 1, 0x0316F2, 650 }, { 1, 0x0316F2, 710 }, { 1, 0x0316F2, 801 }, { 1, 0x0316F2, 802 },  // DLC03DialogueV118
		{ 1, 0x0316F2, 803 }, { 1, 0x0316F2, 804 }, { 1, 0x0316F2, 805 }, { 1, 0x0316F2, 806 }, { 1, 0x0316F2, 807 }, { 1, 0x0316F2, 810 }, { 1, 0x0316F2, 820 },
		{ 1, 0x0316F2, 830 }, { 1, 0x0316F2, 900 }, { 1, 0x0316F2, 950 }, { 1, 0x0316F2, 960 }, { 1, 0x0316F2, 970 }, { 1, 0x0316F2, 1350 }, { 1, 0x0316F2, 1370 },
		{ 1, 0x0316F2, 1400 }, { 1, 0x0316F2, 1500 }, { 1, 0x0316F2, 2000 },
		{ 1, 0x03389A, 100 }, { 1, 0x03389A, 300 },  // DLC03_V118_Intro
		{ 1, 0x034DEA, 10 },  // DLC03MQ06_WindFarmBuildingQuest
		{ 1, 0x036762, 5 },  // DLC03_V118_PearlAvery
		{ 1, 0x036763, 10 }, { 1, 0x036763, 300 }, { 1, 0x036763, 880 },  // DLC03_V118_Quest
		{ 1, 0x043822, 0 }, { 1, 0x043822, 1 }, { 1, 0x043822, 5 }, { 1, 0x043822, 41 }, { 1, 0x043822, 61 }, { 1, 0x043822, 100 },  // DLC03MQ02Fight
		{ 1, 0x04B129, 0 },  // DLC03_V118_Quest2
		{ 1, 0x04B1F6, 400 }, { 1, 0x04B1F6, 500 },  // DLC03AcadiaM03
		{ 1, 0x04B95A, 200 },  // DLC03AcadiaM04
		{ 1, 0x04E779, 1 },  // DLC03FarHarborM03
		{ 1, 0x04E77A, 150 }, { 1, 0x04E77A, 160 }, { 1, 0x04E77A, 590 }, { 1, 0x04E77A, 595 }, { 1, 0x04E77A, 600 },  // DLC03FarHarborM02
		{ 1, 0x053D16, 200 },  // DLC03Acadia_BoSVertibird
		{ 2, 0x000801, 0 }, { 2, 0x000801, 2200 }, { 2, 0x000801, 3000 },  // DLC01MQ02
		{ 2, 0x000806, 2100 }, { 2, 0x000806, 3000 },  // DLC01MQ01
		{ 2, 0x0008BB, 0 }, { 2, 0x0008BB, 1 }, { 2, 0x0008BB, 3 }, { 2, 0x0008BB, 4 }, { 2, 0x0008BB, 5 }, { 2, 0x0008BB, 6 }, { 2, 0x0008BB, 7 },  // DLC01Lair
		{ 2, 0x0008BB, 8 }, { 2, 0x0008BB, 10 }, { 2, 0x0008BB, 101 }, { 2, 0x0008BB, 280 }, { 2, 0x0008BB, 400 }, { 2, 0x0008BB, 480 }, { 2, 0x0008BB, 980 },
		{ 2, 0x0008BB, 1050 }, { 2, 0x0008BB, 1060 }, { 2, 0x0008BB, 1100 }, { 2, 0x0008BB, 1350 }, { 2, 0x0008BB, 1400 }, { 2, 0x0008BB, 1498 }, { 2, 0x0008BB, 1499 },
		{ 2, 0x0008BB, 1500 }, { 2, 0x0008BB, 1501 }, { 2, 0x0008BB, 1502 }, { 2, 0x0008BB, 2000 },
		{ 2, 0x0010F5, 0 }, { 2, 0x0010F5, 1 }, { 2, 0x0010F5, 2 }, { 2, 0x0010F5, 680 }, { 2, 0x0010F5, 801 }, { 2, 0x0010F5, 810 }, { 2, 0x0010F5, 880 },  // DLC01MQ05
		{ 2, 0x0010F5, 885 }, { 2, 0x0010F5, 890 }, { 2, 0x0010F5, 1000 }, { 2, 0x0010F5, 1100 },
		{ 2, 0x002833, 0 }, { 2, 0x002833, 1 }, { 2, 0x002833, 2 }, { 2, 0x002833, 3 }, { 2, 0x002833, 50 }, { 2, 0x002833, 110 }, { 2, 0x002833, 160 },  // DLC01MQ04
		{ 2, 0x002833, 190 }, { 2, 0x002833, 255 },
		{ 2, 0x00FF87, 40 }, { 2, 0x00FF87, 50 }, { 2, 0x00FF87, 100 },  // DLC01MQ01Caravan
		{ 3, 0x000800, 520 }, { 3, 0x000800, 800 }, { 3, 0x000800, 1000 },  // DLC04MQ00
		{ 3, 0x000801, 0 }, { 3, 0x000801, 1 }, { 3, 0x000801, 2 }, { 3, 0x000801, 3 }, { 3, 0x000801, 1000 },  // DLC04MQ01
		{ 3, 0x000802, 0 }, { 3, 0x000802, 1000 },  // DLC04MQ02
		{ 3, 0x000803, 655 },  // DLC04MQ03
		{ 3, 0x000804, 1 }, { 3, 0x000804, 2 }, { 3, 0x000804, 3 }, { 3, 0x000804, 4 }, { 3, 0x000804, 10 }, { 3, 0x000804, 50 }, { 3, 0x000804, 300 },  // DLC04MQ04
		{ 3, 0x000804, 305 },
		{ 3, 0x000806, 40 }, { 3, 0x000806, 60 }, { 3, 0x000806, 70 }, { 3, 0x000806, 75 }, { 3, 0x000806, 100 },  // DLC04MS01
		{ 3, 0x000807, 100 }, { 3, 0x000807, 300 }, { 3, 0x000807, 500 },  // DLC04MS02
		{ 3, 0x000808, 11 },  // DLC04Grandchester
		{ 3, 0x00DB37, 15 }, { 3, 0x00DB37, 20 }, { 3, 0x00DB37, 105 },  // DLC04RaidWipeOut
		{ 3, 0x014352, 20 },  // DLC04RaidChaseOff
		{ 3, 0x016E18, 15 }, { 3, 0x016E18, 20 }, { 3, 0x016E18, 65 }, { 3, 0x016E18, 70 }, { 3, 0x016E18, 95 }, { 3, 0x016E18, 100 },  // DLC04RaidSubdue
		{ 3, 0x019088, 20 },  // DLC04RaidCoerce
		{ 3, 0x01AABF, 47 }, { 3, 0x01AABF, 200 },  // DLC04DialogueSettlement
		{ 3, 0x01E34A, 210 }, { 3, 0x01E34A, 212 }, { 3, 0x01E34A, 300 }, { 3, 0x01E34A, 1000 }, { 3, 0x01E34A, 1500 }, { 3, 0x01E34A, 1600 }, { 3, 0x01E34A, 2000 },  // DLC04GZMainQuest
		{ 3, 0x035791, 20 },  // DLC04MS01BradbertonDies
		{ 3, 0x03684E, 1000 },  // DLC04_BottlingPlantMAIN
		{ 3, 0x03684F, 780 }, { 3, 0x03684F, 795 }, { 3, 0x03684F, 800 }, { 3, 0x03684F, 820 }, { 3, 0x03684F, 1000 },  // DLC04_KiddieKingdomMain
		{ 3, 0x0431AB, 800 }, { 3, 0x0431AB, 900 }, { 3, 0x0431AB, 1000 },  // DLC04DryRockGulch
	};

	inline bool SetsAnother(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto key = std::tuple{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		return std::ranges::binary_search(SETS_ANOTHER, key, {}, [](const Setter& a_setter) { return std::tuple{ a_setter.file, a_setter.quest, a_setter.stage }; });
	}

	// Stages of the shared quests that turn a faction on the player, with that faction: a kick-out, an ending, a story stage that
	// sets a place's people on the player as its fight begins. A friend's such stage (StageRules::kHostile, which QuestSync adds to
	// its rule from this table) is set live beside the friend who did it, as any stage, and otherwise waits, its quest with it, while
	// any of that faction is near this player (alive, not disabled, not the player's own companion, loaded and within HostileHold::RANGE;
	// a fight with anyone else is the capped combat hold's), and is set once
	// none is: the chooser's choice is both players', near or far, live or caught up, and it is never skipped (the maintainer's
	// rule, 2026-11-11, in place of "set only beside the friend, else offered, then skipped": a far friend's kick-out or ending
	// split the saves for good, while the story's own 300-odd hostile stages, with no row, reached them at once; most of those
	// are a quest's fight being set up, harmless far from it, and the hold costs them nothing there). Generated with
	// tools/questscan.ps1 -Hostile (HostileScan in tools/questscan.cs) from the quest fragment scripts of Fallout4.esm, DLCCoast.esm,
	// DLCRobot.esm and DLCNukaWorld.esm: every stage of every quest a friend's stage is set on (shared by its type, a finale, a
	// dialogue quest, SAFE's and SILENT's; not radiants or PER_GAME) whose fragment, a script function it calls or a stage it sets or
	// starts calls SetPlayerEnemy, SetEnemy on the player's faction with neither side made neutral, or AddToFaction into a PlayerEnemy
	// faction (then the faction is that one: a member of it near is an enemy already); the faction the script names, resolved in
	// the VMAD of the quest whose script makes the call, and for MQ00's kick-out helper (a number into its Factions array) and the
	// Galactic Zone's start-up (every one of its enemy factions) by hand in the scan (its BY_HAND). One row per stage and faction,
	// the first way found (DN049's x49 each turn one shop's robots). StartCombat on the player is a passing fight, not a faction's
	// standing: no row (the audit of 2026-11-10). The faction's file is its own (File), not the quest's: Far Harbor's S02 210 adds
	// Malcolm to Fallout4.esm's PlayerEnemyFaction. The call (HostileCall) is what QuestSync makes itself, through Papyrus, for
	// a stage that also moves the player (a kMoves row, no kBridge) when the player didn't go with the friend: the stage is
	// not set, the hostility is (Verdict::kHostileOnly), unless the stage stands for a faction's kick-out, which is run here
	// itself (KICKOUTS). Sorted by file, quest and stage.
	enum HostileCall : std::uint8_t
	{
		kSetEnemy,        // Faction.SetEnemy(PlayerFaction), neither side neutral: enemies both ways (or PlayerFaction.SetEnemy(the faction): the same)
		kSetPlayerEnemy,  // Faction.SetPlayerEnemy(): the faction's player-enemy flag
		kAddToFaction,    // an actor the stage's alias holds put into a PlayerEnemy faction: not made without the stage (no kMoves row is one)
	};
	struct Hostile
	{
		std::uint8_t  file;         // File: the quest's
		std::uint32_t quest;        // form ID in its file
		std::uint16_t stage;
		std::uint8_t  factionFile;  // File: the faction's
		std::uint32_t faction;      // form ID in its file
		HostileCall   call;         // how the script turns it on the player
	};
	inline constexpr Hostile HOSTILE[] = {
		{ 0, 0x02074B, 200, 0, 0x117D52, kSetEnemy },  // CZ: SetEnemy CZ_Faction PlayerFaction (by 250)
		{ 0, 0x02074B, 240, 0, 0x117D52, kSetEnemy },  // SetEnemy CZ_Faction PlayerFaction (by 250)
		{ 0, 0x02074B, 243, 0, 0x117D52, kSetEnemy },  // SetEnemy CZ_Faction PlayerFaction (by 250)
		{ 0, 0x02074B, 245, 0, 0x117D52, kSetEnemy },  // SetEnemy CZ_Faction PlayerFaction (by 250)
		{ 0, 0x02074B, 247, 0, 0x117D52, kSetEnemy },  // SetEnemy CZ_Faction PlayerFaction (by 250)
		{ 0, 0x02074B, 250, 0, 0x117D52, kSetEnemy },  // SetEnemy CZ_Faction PlayerFaction
		{ 0, 0x0229EC, 5, 0, 0x05E558, kSetEnemy },  // MQ207: SetEnemy PlayerFaction temp4 (by 200) [by hand]
		{ 0, 0x0229EC, 200, 0, 0x05E558, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x0229EE, 2, 0, 0x05E558, kSetEnemy },  // MQ302: SetEnemy InstituteFaction PlayerFaction (by Min301 0)
		{ 0, 0x0229EE, 400, 0, 0x05E558, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x0229EE, 1000, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.CheckForMQAdvance)
		{ 0, 0x0229F6, 40, 0, 0x106CA3, kSetEnemy },  // MS01: SetEnemy MS01BulletFaction PlayerFaction
		{ 0, 0x0229FE, 232, 0, 0x14BF23, kSetPlayerEnemy },  // MS07c: SetPlayerEnemy EddieFaction (by 240)
		{ 0, 0x0229FE, 234, 0, 0x14BF23, kSetPlayerEnemy },  // SetPlayerEnemy EddieFaction
		{ 0, 0x0229FE, 240, 0, 0x14BF23, kSetPlayerEnemy },  // SetPlayerEnemy EddieFaction
		{ 0, 0x022A00, 1450, 0, 0x047EBD, kSetEnemy },  // MS09: SetEnemy PlayerFaction CrimeCabotHouse
		{ 0, 0x022A02, 390, 0, 0x08C9DA, kSetEnemy },  // MS11: SetEnemy pMS11ScavengersNPCFaction pPlayerFaction
		{ 0, 0x022A02, 728, 0, 0x08C9DA, kSetEnemy },  // SetEnemy pMS11ScavengersNPCFaction pPlayerFaction
		{ 0, 0x022A02, 800, 0, 0x08C9DA, kSetEnemy },  // SetEnemy pMS11ScavengersNPCFaction pPlayerFaction
		{ 0, 0x022A02, 1050, 0, 0x08C9DB, kSetEnemy },  // SetEnemy pMS11IronsidesRobotsFaction pPlayerFaction (by 1100)
		{ 0, 0x022A02, 1100, 0, 0x08C9DB, kSetEnemy },  // SetEnemy pMS11IronsidesRobotsFaction pPlayerFaction
		{ 0, 0x022A02, 1200, 0, 0x08C9DA, kSetEnemy },  // SetEnemy pMS11ScavengersNPCFaction pPlayerFaction
		{ 0, 0x022A02, 1275, 0, 0x08C9DA, kSetEnemy },  // SetEnemy pMS11ScavengersNPCFaction pPlayerFaction
		{ 0, 0x022A02, 8990, 0, 0x08C9DB, kSetEnemy },  // SetEnemy pMS11IronsidesRobotsFaction pPlayerFaction
		{ 0, 0x022A05, 60, 0, 0x0E356E, kSetEnemy },  // MS14: SetEnemy MS14BadGuyFaction PlayerFaction
		{ 0, 0x022A07, 210, 0, 0x106C2F, kAddToFaction },  // MS16: AddToFaction pPlayerEnemyFaction
		{ 0, 0x022A07, 220, 0, 0x106C2F, kAddToFaction },  // AddToFaction pPlayerEnemyFaction
		{ 0, 0x022A08, 90, 0, 0x074794, kSetEnemy },  // MS17: SetEnemy pCovenantHQSecurityFaction pPlayerFaction
		{ 0, 0x022A08, 315, 0, 0x074794, kSetEnemy },  // SetEnemy pCovenantHQSecurityFaction pPlayerFaction
		{ 0, 0x022A08, 331, 0, 0x074794, kSetEnemy },  // SetEnemy pCovenantHQSecurityFaction pPlayerFaction (by 315)
		{ 0, 0x022A08, 420, 0, 0x0786F1, kSetEnemy },  // SetEnemy pMS17HonestDanFaction pPlayerFaction
		{ 0, 0x022A08, 450, 0, 0x074794, kSetEnemy },  // SetEnemy pCovenantHQSecurityFaction pPlayerFaction (by 315)
		{ 0, 0x022A08, 465, 0, 0x062030, kSetEnemy },  // SetEnemy pCovenantFaction pPlayerFaction (by 470)
		{ 0, 0x022A08, 467, 0, 0x062030, kSetEnemy },  // SetEnemy pCovenantFaction pPlayerFaction (by 470)
		{ 0, 0x022A08, 470, 0, 0x062030, kSetEnemy },  // SetEnemy pCovenantFaction pPlayerFaction
		{ 0, 0x022A08, 490, 0, 0x0786F1, kSetEnemy },  // SetEnemy pMS17HonestDanFaction pPlayerFaction
		{ 0, 0x022A08, 760, 0, 0x02DFEF, kAddToFaction },  // AddToFaction pREPlayerEnemy
		{ 0, 0x02633E, 0, 0, 0x19EC30, kSetEnemy },  // DN079: SetEnemy DN079_CanneryTurretFaction PlayerFaction
		{ 0, 0x02633E, 520, 0, 0x03EFAE, kSetEnemy },  // SetEnemy DN079_CanneryFaction PlayerFaction
		{ 0, 0x027556, 435, 0, 0x128CF7, kSetEnemy },  // MS04: SetEnemy pMS04KendraFaction pPlayerFaction
		{ 0, 0x027556, 470, 0, 0x128CEF, kSetEnemy },  // SetEnemy pMS04AJFaction pPlayerFaction
		{ 0, 0x027556, 510, 0, 0x128CEA, kSetEnemy },  // SetEnemy pMS04WayneDelancyFaction pPlayerFaction
		{ 0, 0x027556, 820, 0, 0x186971, kSetEnemy },  // SetEnemy pMS04SmilingKatesPosseFaction pPlayerFaction
		{ 0, 0x027556, 1307, 0, 0x02B746, kSetEnemy },  // SetEnemy pMS04SinjinPersonalFaction pPlayerFaction (by 1310)
		{ 0, 0x027556, 1310, 0, 0x02B746, kSetEnemy },  // SetEnemy pMS04SinjinPersonalFaction pPlayerFaction
		{ 0, 0x027556, 1320, 0, 0x02B746, kSetEnemy },  // SetEnemy pMS04SinjinPersonalFaction pPlayerFaction (by 1310)
		{ 0, 0x027556, 1340, 0, 0x02B746, kSetEnemy },  // SetEnemy pMS04SinjinPersonalFaction pPlayerFaction (by 1310)
		{ 0, 0x027556, 1345, 0, 0x02B746, kSetEnemy },  // SetEnemy pMS04SinjinPersonalFaction pPlayerFaction (by 1310)
		{ 0, 0x0284BC, 110, 0, 0x028FB0, kSetEnemy },  // DN015: SetEnemy DN015_RobotFaction PlayerFaction
		{ 0, 0x0284BC, 250, 0, 0x028FB2, kSetEnemy },  // SetEnemy DN015_DirectorFaction PlayerFaction
		{ 0, 0x02BF21, 1, 0, 0x091895, kSetEnemy },  // BoS201: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x02BF21, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x02BF21, 3, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x02BF21, 4, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x033527, 650, 0, 0x106C2F, kAddToFaction },  // V81_03: AddToFaction PlayerEnemyFaction
		{ 0, 0x03A457, 70, 0, 0x05E558, kSetEnemy },  // Min02: SetEnemy InstituteFaction PlayerFaction (by 500)
		{ 0, 0x03A457, 200, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 400)
		{ 0, 0x03A457, 250, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 200)
		{ 0, 0x03A457, 400, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 500)
		{ 0, 0x03A457, 500, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 520)
		{ 0, 0x03A457, 520, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 570)
		{ 0, 0x03A457, 570, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by min02script.CheckRadioTransmitterStatus)
		{ 0, 0x03A457, 600, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.SetProgress)
		{ 0, 0x043275, 1, 0, 0x05DE41, kSetEnemy },  // RR303: SetEnemy pBrotherhoodofSteelFaction pPlayerFaction
		{ 0, 0x043275, 100, 0, 0x19E9A6, kSetEnemy },  // SetEnemy pBoSAttackingRailroadFaction pPlayerFaction
		{ 0, 0x043275, 1000, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction
		{ 0, 0x043275, 1010, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1000)
		{ 0, 0x043275, 1020, 0, 0x19E9A6, kSetEnemy },  // SetEnemy pBoSAttackingRailroadFaction pPlayerFaction (by 100)
		{ 0, 0x043275, 1030, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1000)
		{ 0, 0x043275, 1150, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy pBrotherhoodofSteelFaction
		{ 0, 0x04402C, 1, 0, 0x091895, kSetEnemy },  // BoSM02: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 2)
		{ 0, 0x04402C, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 3, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 4, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 5, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 6, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 7, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 8, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 9, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x04402C, 255, 0, 0x0BC0F7, kSetEnemy },  // SetEnemy BoSInitiateClarkeFaction PlayerFaction (by 380)
		{ 0, 0x04402C, 256, 0, 0x0BC0F7, kSetEnemy },  // SetEnemy BoSInitiateClarkeFaction PlayerFaction (by 380)
		{ 0, 0x04402C, 280, 0, 0x0BC0F7, kSetEnemy },  // SetEnemy BoSInitiateClarkeFaction PlayerFaction (by 255)
		{ 0, 0x04402C, 380, 0, 0x0BC0F7, kSetEnemy },  // SetEnemy BoSInitiateClarkeFaction PlayerFaction
		{ 0, 0x04542B, 120, 0, 0x1AC2B4, kSetEnemy },  // DN049: SetEnemy DN049MaddensChampFaction PlayerFaction
		{ 0, 0x04542B, 149, 0, 0x0E5251, kSetEnemy },  // SetEnemy DN049MaddensFaction PlayerFaction
		{ 0, 0x04542B, 221, 0, 0x0EAC9C, kSetEnemy },  // SetEnemy DN049OutletGutsyDemoModelsFaction PlayerFaction
		{ 0, 0x04542B, 249, 0, 0x0E8D59, kSetEnemy },  // SetEnemy DN049OutletFaction PlayerFaction
		{ 0, 0x04542B, 349, 0, 0x0E8D5D, kSetEnemy },  // SetEnemy DN049DinerFaction PlayerFaction
		{ 0, 0x04542B, 449, 0, 0x0E8D4E, kSetEnemy },  // SetEnemy DN049FallonsFaction PlayerFaction
		{ 0, 0x04542B, 549, 0, 0x0E7B5C, kSetEnemy },  // SetEnemy DN049BakeryFaction PlayerFaction
		{ 0, 0x04542B, 649, 0, 0x0E7AFD, kSetEnemy },  // SetEnemy DN049SlocumFaction PlayerFaction
		{ 0, 0x04542B, 749, 0, 0x0E933D, kSetEnemy },  // SetEnemy DN049BowlingAlleyFaction PlayerFaction
		{ 0, 0x04542B, 849, 0, 0x0E990D, kSetEnemy },  // SetEnemy DN049DirectorFaction PlayerFaction
		{ 0, 0x0457C6, 250, 0, 0x08AF6C, kSetEnemy },  // MS13FindPhoto: SetEnemy GoodneighborMarowskiFaction PlayerFaction
		{ 0, 0x0459D2, 609, 0, 0x0994F6, kSetEnemy },  // RR101: SetEnemy pPlayerFaction pRailroadFaction
		{ 0, 0x0459D2, 1013, 0, 0x0994F6, kSetEnemy },  // SetEnemy pRailroadFaction pPlayerFaction
		{ 0, 0x04AD05, 55, 0, 0x0F17C1, kSetEnemy },  // DN101: SetEnemy DN101_RaiderFaction PlayerFaction
		{ 0, 0x04AD05, 60, 0, 0x0F17C1, kSetEnemy },  // SetEnemy DN101_RaiderFaction PlayerFaction
		{ 0, 0x0537FF, 1, 0, 0x091895, kSetEnemy },  // BoS202: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0537FF, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x05DEE4, 220, 0, 0x05E558, kSetEnemy },  // Min01: SetEnemy InstituteFaction PlayerFaction (by MinRecruit00 10)
		{ 0, 0x05DEE4, 1500, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 2010)
		{ 0, 0x05DEE4, 2000, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.SetProgress)
		{ 0, 0x05DEE4, 2010, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by MinRecruit00 10)
		{ 0, 0x068D7B, 1, 0, 0x091895, kSetEnemy },  // BoS100Fight: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 99)
		{ 0, 0x068D7B, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x068D7B, 20, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction
		{ 0, 0x068D7B, 30, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 20)
		{ 0, 0x068D7B, 99, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 20)
		{ 0, 0x06F5C1, 1, 0, 0x091895, kSetEnemy },  // BoS101: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x06F5C1, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x06F5C1, 140, 0, 0x248414, kSetPlayerEnemy },  // SetPlayerEnemy DN017_CrimeFactionArcjet
		{ 0, 0x07CC61, 210, 0, 0x10C4F4, kSetEnemy },  // RR301: SetEnemy pRR301SynthFaction pPlayerFaction
		{ 0, 0x0890B4, 300, 0, 0x074794, kSetEnemy },  // DialogueCovenant: SetEnemy pCovenantHQSecurityFaction pPlayerFaction
		{ 0, 0x08FCED, 500, 0, 0x026DCF, kSetEnemy },  // DN036: SetEnemy DN036PhyllisFaction PlayerFaction
		{ 0, 0x0A8257, 750, 0, 0x106C2F, kAddToFaction },  // InstM02: AddToFaction PlayerEnemyFaction
		{ 0, 0x0A8258, 70, 0, 0x0EF224, kSetEnemy },  // Inst302: SetEnemy BunkerHillInstituteFaction PlayerFaction
		{ 0, 0x0A8258, 91, 0, 0x0EF224, kSetEnemy },  // SetEnemy BunkerHillInstituteFaction PlayerFaction
		{ 0, 0x0AA778, 980, 0, 0x05E558, kSetEnemy },  // Min03: SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.SetProgress)
		{ 0, 0x0AA778, 1000, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by 980)
		{ 0, 0x0ADCE5, 1, 0, 0x091895, kSetEnemy },  // BoSM04: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 2)
		{ 0, 0x0ADCE5, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x0ADCE5, 3, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x0ADCE5, 4, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by 1)
		{ 0, 0x0AE51C, 1, 0, 0x091895, kSetEnemy },  // BoS301: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0AE51C, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0AE51C, 3, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0AE51C, 4, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0AE51C, 178, 0, 0x144047, kSetEnemy },  // SetEnemy DN166AtomFaction PlayerFaction
		{ 0, 0x0B1D79, 1, 0, 0x091895, kSetEnemy },  // BoSM01: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 2)
		{ 0, 0x0B1D79, 139, 0, 0x0B3D7D, kSetEnemy },  // SetEnemy BoSM01_BrandisFaction PlayerFaction (by 140)
		{ 0, 0x0B1D79, 140, 0, 0x0B3D7D, kSetEnemy },  // SetEnemy BoSM01_BrandisFaction PlayerFaction
		{ 0, 0x0B2D48, 790, 0, 0x05E558, kSetEnemy },  // RR201: SetEnemy InstituteFaction PlayerFaction (by InstKickOut 0)
		{ 0, 0x0B9F9D, 1, 0, 0x091895, kSetEnemy },  // BoS302: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0B9F9D, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0B9F9D, 255, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by BoSDansePostGame 20)
		{ 0, 0x0C9B0B, 10, 0, 0x05DE41, kSetEnemy },  // MQ00: SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x0C9B0B, 30, 0, 0x0994F6, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x0C9B0B, 40, 0, 0x05E558, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x0DDFDB, 1, 0, 0x05E558, kSetEnemy },  // Min301: SetEnemy InstituteFaction PlayerFaction (by 0)
		{ 0, 0x0E2058, 40, 0, 0x0E73CA, kAddToFaction },  // Inst301: AddToFaction Inst301PlayerEnemyFaction
		{ 0, 0x0E2058, 50, 0, 0x0E73CA, kAddToFaction },  // AddToFaction Inst301PlayerEnemyFaction
		{ 0, 0x0EF21C, 30, 0, 0x0EF225, kSetEnemy },  // Inst302Combat: SetEnemy BunkerHillBoSFaction PlayerFaction
		{ 0, 0x0FDC8C, 1, 0, 0x091895, kSetEnemy },  // BoS303: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0FDC8C, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0FDC8C, 3, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x0FDC8C, 30, 0, 0x05E558, kSetPlayerEnemy },  // SetPlayerEnemy InstituteFaction
		{ 0, 0x0FDC8C, 250, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.CheckForMQAdvance)
		{ 0, 0x1069A9, 75, 0, 0x106C2F, kAddToFaction },  // DialogueDrumlinDiner: AddToFaction PlayerEnemyFaction (by 100)
		{ 0, 0x1069A9, 80, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 200)
		{ 0, 0x1069A9, 100, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction
		{ 0, 0x1069A9, 200, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction
		{ 0, 0x1069A9, 300, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 100)
		{ 0, 0x1069A9, 310, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 100)
		{ 0, 0x1069A9, 400, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 200)
		{ 0, 0x1069A9, 410, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 200)
		{ 0, 0x107A1C, 30, 0, 0x0994F6, kSetPlayerEnemy },  // BoS302B: SetPlayerEnemy pRailroadFaction (by RRKickOut 100)
		{ 0, 0x10C64A, 1910, 0, 0x05E558, kSetEnemy },  // MQ302Min: SetEnemy InstituteFaction PlayerFaction (by MQ302 1000)
		{ 0, 0x10C64A, 2000, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by MQ302 1000)
		{ 0, 0x10C64B, 1000, 0, 0x05E558, kSetEnemy },  // MQ302BoS: SetEnemy InstituteFaction PlayerFaction (by MQ302 1000)
		{ 0, 0x10C64C, 1000, 0, 0x05E558, kSetEnemy },  // MQ302RR: SetEnemy InstituteFaction PlayerFaction (by MQ302 1000)
		{ 0, 0x1323AB, 30, 0, 0x1324C9, kSetEnemy },  // FFGoodneighbor07: SetEnemy BobbiFaction PlayerFaction
		{ 0, 0x136329, 200, 0, 0x05E558, kSetEnemy },  // Min207: SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.CheckForMQAdvance)
		{ 0, 0x142FD3, 1, 0, 0x05DE41, kSetEnemy },  // RR304: SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by RR303 1)
		{ 0, 0x142FD3, 20, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x142FD3, 25, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x142FD3, 30, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x15BD39, 30, 0, 0x05DE41, kSetPlayerEnemy },  // InstMassFusion: SetPlayerEnemy BrotherhoodofSteelFaction (by BoSKickOut 20)
		{ 0, 0x16D036, 0, 0, 0x05E558, kSetEnemy },  // InstKickOut: SetEnemy InstituteFaction PlayerFaction (by instkickoutquestscript.KickOut)
		{ 0, 0x16D036, 100, 0, 0x05E558, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x173ED9, 1, 0, 0x091895, kSetEnemy },  // BoS304: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x173ED9, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x173ED9, 255, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by InstKickOut 0)
		{ 0, 0x1764DF, 150, 0, 0x106C2F, kAddToFaction },  // FFDiamondCity10: AddToFaction PlayerEnemyFaction
		{ 0, 0x1764DF, 170, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 150)
		{ 1, 0x001B3E, 100, 1, 0x005C7E, kSetEnemy },  // DLC03MQ00: SetEnemy pPlayerFaction pDLC03SettlementFarHarborFaction (by dlc03:dcl03mq00script.DLC03KickOutOfFarHarbor)
		{ 1, 0x001B40, 570, 1, 0x01A213, kSetPlayerEnemy },  // DLC03MQ02: SetPlayerEnemy pDLC03MQ02LoneChildAtomFaction
		{ 1, 0x001B43, 800, 0, 0x106C2F, kAddToFaction },  // DLC03MQ05: AddToFaction pPlayerEnemyFaction
		{ 1, 0x001B43, 850, 1, 0x00463D, kSetEnemy },  // SetEnemy pDLC03AcadiaFaction pPlayerFaction (by DLC03AcadiaDefense 100)
		{ 1, 0x001B44, 1, 1, 0x030DC4, kSetEnemy },  // DLC03MQ06: SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x001B44, 2, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x001B44, 3, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x004C8E, 70, 1, 0x004462, kSetEnemy },  // DLC03CoA_DialogueNucleus: SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
		{ 1, 0x004C8F, 3, 1, 0x030DC4, kSetEnemy },  // DLC03CoA_DialogueNucleusEntrance: SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by 50)
		{ 1, 0x004C8F, 10, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by 50)
		{ 1, 0x004C8F, 50, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction
		{ 1, 0x004C8F, 150, 1, 0x004462, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
		{ 1, 0x005BD7, 9000, 1, 0x005C7E, kSetEnemy },  // DLC03DialogueFarHarbor: SetEnemy pDLC03SettlementFarHarborFaction pPlayerFaction
		{ 1, 0x00AEC3, 210, 0, 0x106C2F, kAddToFaction },  // DLC03FarHarborS02: AddToFaction pPlayerEnemyFaction
		{ 1, 0x00AFB1, 700, 1, 0x004462, kSetEnemy },  // DLC03AtomM01: SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
		{ 1, 0x013F39, 200, 1, 0x00463D, kSetEnemy },  // DLC03Acadia_BoS: SetEnemy pDLC03AcadiaFaction pPlayerFaction (by DLC03AcadiaDefense 100)
		{ 1, 0x013F3A, 200, 1, 0x00463D, kSetEnemy },  // DLC03Acadia_Institute: SetEnemy pDLC03AcadiaFaction pPlayerFaction (by DLC03AcadiaDefense 100)
		{ 1, 0x01536F, 405, 0, 0x106C2F, kAddToFaction },  // DLC03AcadiaM01: AddToFaction PlayerEnemyFaction
		{ 1, 0x01B0AF, 520, 0, 0x106C2F, kAddToFaction },  // DLC03AtomM02: AddToFaction PlayerEnemyFaction
		{ 1, 0x02127F, 1, 1, 0x030DC4, kSetEnemy },  // DLC03MQ06a: SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 10)
		{ 1, 0x02127F, 95, 1, 0x022F6D, kSetEnemy },  // SetEnemy DLC03MQ06a_ChildrenofAtomFaction PlayerFaction
		{ 1, 0x02127F, 755, 1, 0x004462, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
		{ 1, 0x02127F, 760, 1, 0x004462, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
		{ 1, 0x0316F2, 503, 1, 0x048012, kSetEnemy },  // DLC03DialogueV118: SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by 710)
		{ 1, 0x0316F2, 710, 1, 0x048012, kSetEnemy },  // SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by dlc03:dlc03dialoguev118script.StartConfrontationFight)
		{ 1, 0x0316F2, 801, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 802, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 803, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 804, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 805, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 806, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 807, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by 810)
		{ 1, 0x0316F2, 810, 1, 0x020629, kSetEnemy },  // SetEnemy DLC03Vault118_ResidentFaction PlayerFaction (by dlc03:dlc03dialoguev118script.SetKickout)
		{ 1, 0x036763, 465, 1, 0x048012, kSetEnemy },  // DLC03_V118_Quest: SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by dlc03:dlc03_v118_questscript.StartConfrontationFight)
		{ 1, 0x036763, 565, 1, 0x048012, kSetEnemy },  // SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by dlc03:dlc03_v118_questscript.StartConfrontationFight)
		{ 1, 0x036763, 665, 1, 0x048012, kSetEnemy },  // SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by dlc03:dlc03_v118_questscript.StartConfrontationFight)
		{ 1, 0x036763, 765, 1, 0x048012, kSetEnemy },  // SetEnemy DLC03Vault118_CombatFaction PlayerFaction (by dlc03:dlc03_v118_questscript.StartConfrontationFight)
		{ 1, 0x040A87, 100, 0, 0x05E558, kSetEnemy },  // DLC03WorkshopRadiantOwned05: SetEnemy InstituteFaction PlayerFaction (by 400)
		{ 1, 0x040A87, 400, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by DLC03:WorkshopRadiantOwnedScript.DLC03FinishQuest)
		{ 2, 0x0008BB, 8, 2, 0x0008B5, kSetEnemy },  // DLC01Lair: SetEnemy DLC01MechanistFaction PlayerFaction (by 1350)
		{ 2, 0x0008BB, 1350, 2, 0x0008B5, kSetEnemy },  // SetEnemy DLC01MechanistFaction PlayerFaction (by 1400)
		{ 2, 0x0008BB, 1400, 2, 0x0008B5, kSetEnemy },  // SetEnemy DLC01MechanistFaction PlayerFaction
		{ 2, 0x0010F5, 885, 2, 0x0008B5, kSetEnemy },  // DLC01MQ05: SetEnemy DLC01MechanistFaction PlayerFaction (by DLC01Lair 1400)
		{ 2, 0x0010F5, 890, 2, 0x0008B5, kSetEnemy },  // SetEnemy DLC01MechanistFaction PlayerFaction (by 885)
		{ 3, 0x000800, 347, 3, 0x037BA6, kSetEnemy },  // DLC04MQ00: SetEnemy DLC04HarveyHostileFaction PlayerFaction
		{ 3, 0x000802, 1500, 3, 0x00F438, kSetEnemy },  // DLC04MQ02: SetEnemy DLC04GangDisciplesFaction PlayerFaction (by DLC04RaiderKickout 20)
		{ 3, 0x000807, 99, 3, 0x00CC95, kSetEnemy },  // DLC04MS02: SetEnemy DLC04HubologistFaction PlayerFaction
		{ 3, 0x00CBD0, 200, 3, 0x00CC95, kSetEnemy },  // DLC04DialogueHubologists: SetEnemy DLC04HubologistFaction PlayerFaction
		{ 3, 0x01E34A, 10, 3, 0x04A02F, kSetEnemy },  // DLC04GZMainQuest: SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A030, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02C, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02D, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A033, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02E, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A031, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A032, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x0243A5, 200, 3, 0x037BA6, kSetEnemy },  // DLC04DialogueHarvey: SetEnemy DLC04HarveyHostileFaction PlayerFaction
		{ 3, 0x035790, 10, 0, 0x106C2F, kAddToFaction },  // DLC04MS01SierraAttacks: AddToFaction PlayerEnemyFaction
		{ 3, 0x03684F, 6, 3, 0x056F29, kSetEnemy },  // DLC04_KiddieKingdomMain: SetEnemy DLC04HostileGhoulFaction PlayerFaction (by 600)
		{ 3, 0x03684F, 600, 3, 0x056F29, kSetEnemy },  // SetEnemy DLC04HostileGhoulFaction PlayerFaction (by dlc04:dlc04kiddiekingdommainquestscript.TheaterFightEnd)
		{ 3, 0x03684F, 700, 3, 0x056F29, kSetEnemy },  // SetEnemy DLC04HostileGhoulFaction PlayerFaction (by dlc04:dlc04kiddiekingdommainquestscript.AddToPAQueue)
		{ 3, 0x03684F, 740, 3, 0x03E4F2, kSetEnemy },  // SetEnemy DLC04OswaldFaction PlayerFaction (by dlc04:dlc04kiddiekingdommainquestscript.StartFight)
		{ 3, 0x0431AB, 700, 0, 0x106C2F, kAddToFaction },  // DLC04DryRockGulch: AddToFaction PlayerEnemyFaction
	};

	// The HOSTILE rows of a stage (none: it turns no faction on the player), in order.
	inline std::span<const Hostile> HostileOf(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto key = std::tuple{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		const auto [first, last] = std::ranges::equal_range(HOSTILE, key, {}, [](const Hostile& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage }; });
		return { first, last };
	}

	// The HOSTILE rows a stage is judged by: its own, and when it starts its quest here (a_starting: the game runs the
	// quest's start-up stage first, as Starting merges the RULES rows) its quest's stage 0's too (InstKickOut 0, DN079 0:
	// the kick-out is the start-up's). Stage 0 itself: its own rows once.
	inline std::vector<Hostile> HostileRows(File a_file, std::uint32_t a_quest, std::uint16_t a_stage, bool a_starting)
	{
		const auto own = HostileOf(a_file, a_quest, a_stage);
		std::vector<Hostile> rows(own.begin(), own.end());
		if (a_starting && a_stage != 0) {
			const auto start = HostileOf(a_file, a_quest, 0);
			rows.insert(rows.begin(), start.begin(), start.end());
		}
		return rows;
	}

	// The three stages that move the player (kMoves, no kBridge) and are HOSTILE's as a faction's kick-out: not gone with
	// the friend (Verdict::kHostileOnly), the kick-out itself is run here in place of the row's bare call, since the kick-out
	// moves nobody and everything else it does (the faction's quests failed, the player out of the faction, its companion
	// dismissed, fast travel to its base refused) belongs here as much as the hostility. Read off the game's scripts
	// (tools/questscan.cs's reader; the maintainer's rule 2026-11-11, the review of the hold):
	//  - InstMassFusion 30 (Mass Fusion for the Institute) sets BoSKickOut 20, whose fragment is BoSKickOutScript.KickOut():
	//    PlayerBoS_KickedOut, RemoveFromFaction, SetPlayerEnemy and SetEnemy on the Brotherhood, MQ00's helper, the airport
	//    workshop unowned, the Prydwen's fast travel off, the Brotherhood's quests failed (355), Danse: COMDanse 91 and
	//    affinity -5000 when he has been a companion, else into PlayerEnemyFaction, and the kick-out message. No MoveTo and
	//    no test of where the player is: the stage's own teleport to Mass Fusion's roof is InstMassFusion 30's, not the
	//    kick-out's. So BoSKickOut 20 is run here, and InstMassFusion 30 is not.
	//  - RR304 20 (Rockets' Red Glare) sets RR304 1, which sets BoSKickOut 20 the same way (and RR304 1 is a bridge, set
	//    here itself: by the time 20 is judged the kick-out is done here, and nothing is run twice).
	//  - InstKickOut 100 is the teleport out of the Institute (TeleportPlayerOutSpell, Wait 4, MoveTo the exterior marker,
	//    unconditional), which the game sets only for a player inside, after the kick-out proper: the quest's start-up
	//    stage 0, InstKickOutQuestScript.Kickout(player's location): the Institute's quests failed (1500), ComX688 91 and
	//    affinity -5000 for X6-88, RemoveFromFaction, SetEnemy on the Institute, the message; then by where the player is:
	//    inside, stage 20 (leave; the relay out sets 100), outside, stage 10 with the map marker, the relay door locked and
	//    MQ00's helper. So the quest is started here (its stage 0 judges this player's place itself) and 100 is not set;
	//    a quest already started or run here was kicked out here already, and nothing is done.
	// A kick-out stage done here already (BoSKickOut 20 by RR304 1; a load of a save past it) is not run again.
	struct KickOut
	{
		std::uint8_t  file;   // File: the mover's quest's
		std::uint32_t quest;  // the mover: a kMoves row that is HOSTILE's
		std::uint16_t stage;
		std::uint32_t kickQuest;  // the kick-out, in the same file
		std::uint16_t kickStage;  // 0: start the quest (its start-up stage is the kick-out) rather than set a stage
	};
	inline constexpr KickOut KICKOUTS[] = {
		{ 0, 0x142FD3, 20, 0x16FF04, 20 },   // RR304 20 -> BoSKickOut 20 (through RR304 1)
		{ 0, 0x15BD39, 30, 0x16FF04, 20 },   // InstMassFusion 30 -> BoSKickOut 20
		{ 0, 0x16D036, 100, 0x16D036, 0 },   // InstKickOut 100 -> InstKickOut started (its 0 is the kick-out)
	};

	// The kick-out a stage runs here in place of its hostility call, or null.
	inline const KickOut* KickOutOf(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		for (const auto& row : KICKOUTS) {
			if (row.file == static_cast<std::uint8_t>(a_file) && row.quest == a_quest && row.stage == a_stage) {
				return &row;
			}
		}
		return nullptr;
	}

	// A stage that starts its quest here (the quest isn't running: setting a stage starts it) runs the
	// quest's start-up stage first, its 0 in the game's quests (MQ105's 0 moves the player): the stage is
	// judged and held for what both do. Null if neither does anything.
	inline std::optional<Rule> Starting(const Rule* a_start, const Rule* a_stage)
	{
		if (!a_start) {
			return a_stage ? std::optional<Rule>(*a_stage) : std::nullopt;
		}
		Rule rule = a_stage ? *a_stage : *a_start;
		if (a_stage) {
			rule.flags = static_cast<std::uint8_t>(a_start->flags | a_stage->flags);
			rule.wait = (std::max)(a_start->wait, a_stage->wait);
		}
		return rule;
	}

	// How long after a stage reads done its script may still be running: nothing that came after it is
	// set until then (a script that waits is overtaken otherwise).
	inline Clock::duration Hold(const Rule* a_rule)
	{
		if (!a_rule || a_rule->wait == 0) {
			return Clock::duration::zero();
		}
		return std::chrono::milliseconds(100 * a_rule->wait) + std::chrono::seconds(1);
	}

	// What to do with another player's stage when its turn comes.
	enum class Verdict
	{
		kSet,
		kSkip,   // never: it isn't the player's to have
		kLater,       // not yet; nothing that came after it is set either
		kWarn,        // not yet: tell the player first that it will move them
		kQuestWaits,  // not yet: its quest waits for the stage that frees the player; other quests go on
		kOpeningWaits,  // not yet: it moves the player, and this game is still in the opening; other quests go on
		kOffer,         // not yet: the player is offered to go with the friend who did it (the teleport key); its quest waits, other quests go on
		kHostileWaits,  // not yet: it turns a faction on the player, and some of that faction are near (HOSTILE); its quest waits, other quests go on
		kHostileOnly,   // never the stage: it moves the player, who didn't go with the friend; the faction it turns on the player is turned on them here all the same (HostileCall)
	};

	struct Situation
	{
		bool                           sharedStory = false;   // the host shares the main story and the factions'
		bool                           live = false;          // it just happened in a friend's game (not caught up from the session)
		bool                           nearFriend = false;    // the friend who did it is close by
		bool                           factionNear = false;   // a kHostile stage: one of the faction it turns on the player is near (HostileHold)
		bool                           held = false;          // caught-up stages wait now (the player is fighting; capped, QuestSync's MAX_COMBAT_HOLD)
		bool                           releaseComing = true;  // a stage that takes the controls away: the one that gives them back is coming too
		bool                           opening = false;       // this game is still in the opening (Story::InOpening)
		bool                           friendInOpening = false;  // a friend's is (their status): it holds nothing here
		std::optional<Clock::duration> warned;                // since the player was told a stage would move them
		bool                           followable = false;    // the friend who did it is known: the teleport key can take the player to them
		std::optional<Clock::duration> offered;               // since the player was offered to go with them (kOffer)
	};

	inline constexpr auto WARNING = std::chrono::seconds(10);
	// After a warning, more stages that move the player go without another one this long.
	inline constexpr auto WARNING_COVERS = std::chrono::seconds(60);
	// A friend's stage is live (they did it just now, not caught up from the session) this long after it
	// came; a stage that moves the player is set then if the friend who did it is within NEAR_FRIEND
	// (game units, about 85 m): it moves them along with that friend.
	inline constexpr auto  LIVE_FOR = std::chrono::seconds(30);
	inline constexpr float NEAR_FRIEND = 6000.0f;
	// A live stage that moves the player, the friend who did it far away: the player is offered to go with them (the
	// teleport key) this long, and it is set once they are beside the friend, as if they had been there all along.
	inline constexpr auto OFFER_FOR = std::chrono::seconds(90);

	// A friend's stage that moves the player is set while the player is with them (it moves them along,
	// as it moved the friend), and skipped otherwise, unless nothing else would start what it starts:
	// then the player is warned first, and it waits while they fight. A friend's payment is never made
	// here, and without a shared story a side quest doesn't move the player's own story on. Nothing that moves
	// the player is set while this game is in the opening: the vault's own scripts fade the screen and take the controls
	// there, and a friend's stage moving the player meanwhile breaks the scripts that give them back. A friend still in
	// the vault holds nobody else's: their own game holds them (with three or more, the others go on together).
	// A live one whose friend is far is first offered: the player can go with them (the teleport key) for OFFER_FOR,
	// and it is set once they are beside the friend. Not taken up, it goes as before: skipped, or a bridge warned and set.
	// Never offered for a stage caught up from the session (only a live one), nor while this game is in the opening.
	// One that turns a faction on the player (kHostile: HOSTILE) is set live beside the friend who did it, as before (they
	// are in that fight together), and otherwise waits, its quest with it, while any of that faction is near this player
	// (Situation::factionNear, HostileHold), and is set once none is: the chooser's choice is both players', caught up or
	// live, near or far, and it is never skipped (up to 0.16.1 a far friend's was offered, then skipped for good, so a
	// kick-out or an ending split the saves). A fight with anyone else holds it as it holds a caught-up stage (held: the
	// capped combat hold), live or not: nothing that starts a fight is set mid-fight. One that moves the player too
	// (kMoves, no kBridge) takes the move's path once the hold lets it through: offered live, set beside the friend; not
	// gone with (the offer run out, nobody to follow, caught up), the stage is not set, but the faction is turned on the
	// player here all the same (kHostileOnly: the chooser's choice, without the move).
	inline Verdict Judge(const Rule* a_rule, const Situation& a_now)
	{
		const std::uint8_t flags = a_rule ? a_rule->flags : 0;
		if ((flags & kPays) || ((flags & kStory) && !a_now.sharedStory)) {
			return Verdict::kSkip;
		}
		const bool withFriend = a_now.live && a_now.nearFriend;
		if ((flags & kHostile) && a_now.factionNear && !withFriend) {
			return Verdict::kHostileWaits;
		}
		if ((flags & kMoves) && a_now.opening) {
			return Verdict::kOpeningWaits;
		}
		if (!a_now.releaseComing) {
			return Verdict::kQuestWaits;
		}
		const bool offerOpen = a_now.offered && *a_now.offered < OFFER_FOR;
		if ((a_now.live || offerOpen) && (!(flags & kMoves) || a_now.nearFriend)) {
			return (flags & kHostile) && a_now.held && !a_now.nearFriend ? Verdict::kLater : Verdict::kSet;
		}
		if ((flags & kMoves) && a_now.followable && (offerOpen || (a_now.live && !a_now.offered))) {
			return Verdict::kOffer;
		}
		if (a_now.held) {
			return Verdict::kLater;
		}
		if (!(flags & kMoves)) {
			return Verdict::kSet;
		}
		if (!(flags & kBridge)) {
			return (flags & kHostile) ? Verdict::kHostileOnly : Verdict::kSkip;
		}
		if (!a_now.warned) {
			return Verdict::kWarn;
		}
		return *a_now.warned >= WARNING ? Verdict::kSet : Verdict::kLater;
	}
}
