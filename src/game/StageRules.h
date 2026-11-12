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
#include <unordered_set>
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
	inline constexpr std::uint8_t kDebug = 1 << 6;   // a developer's quickstart, which only the console sets (IsDebug): never shared, neither reported nor set from a friend's, and what its script sets is not reported either (QUICKSTART_SETS)

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
	// catching up a friend's story was sent there (up to 0.10.1). Every stage it lists has kMoves here. MQ203's 0 and
	// MQ204's 0, debug stages (no start-up flag: never set in a game played through) that set MQ202 300 and MQ203 1000,
	// had no row once, as Starting() judged every stage that starts their quest by its 0 and no stage of MQ204 (The
	// Glowing Sea) would have been caught up; they are kDebug rows now, which StartRule never gives as a quest's start.
	// Automatron's and Nuka-World's rows (files 2 and 3) are tools/questscan.ps1 -Rules's output (the same scans, and a
	// wait of each fragment's literal Utility.Wait plus 2 s for each MoveTo of the player), its payments checked by hand:
	// Nuka-World's raids' 90 (DLC04RaidChaseOff, DLC04RaidCoerce: the settlement's caps) and DLC04MS02 500 (the space
	// suits and helmets unequipped and taken); the robot's head DLC01MQ04 200 and the drink DLC04DryRockGulch 218 take
	// are quest items, read from an alias. Most of their moves are the developers' quickstarts (stages 0-4, MoveTo a
	// debug marker), never set in a game played through: kDebug rows (below); DLC04MQ01 1000 sets DLC04MQ01Gauntlet 2500,
	// which walks the player through the Nuka-World station's stairwell door. Their other stage 0s that move the player
	// are left out (no start-up stage): DLC01MQ01, DLC04MS02. None is a bridge: an add-on's quests start here when the
	// player goes there.
	// The quests the host's sQuests = safe shares (QuestFilter::SAFE) have their rows from tools/questscan.ps1 -Safe -Rules
	// -Files 0,1,2,3, the same scans, their payments checked by hand: caps, a fern flower and a Nuka-Cola Quartz are
	// kPays; what a stage takes that is the quest's own (Far Harbor's fog condenser modules and its mirelurk carapaces
	// for Brooks, FFGoodneighbor03's HalluciGen gas canister) is not, as for the story's quest items. Most of their
	// moves are quickstarts too (DLC04MQ03 0-5: kDebug); the others (DN070 5 into the treasure's cave, DLC04MS01 550,
	// DLC03PostQuestTreasureDig 200's fade) move a joiner only along with the friend who did them. DN154's 0, which
	// moves the player and is no start-up stage, is left out. BoSM04Postquest is no longer SAFE's (its 0 sets the
	// repeatable DN053), so its gantry quickstart has no row. The
	// objective-less quests it shares too (QuestFilter::SILENT) have theirs from the same scan: a few waits, one
	// quickstart (DLC03MQ02Fight 5, kDebug; its 0 is no start-up stage, left out with MS05BTapes's 0 and
	// DLC04Grandchester's 0), and Grandchester's pre-war money and ticket, kPays. The pump regulators the Children of Atom's pump takes
	// (DLC03CoA_FFNucleus03_PumpHandler 20 and 30) are the quest's own, as the fog condenser modules: no row. What
	// their stages give (caps from a fight's prize or a reward) both players get, as a shared quest's reward.
	// SAFE's quests shared for their outcomes (tools/questscan.cs ALLOW_OUTCOMES: a faction's hostility, a settlement)
	// come from the same scan. A stage that turns a faction on the player (SetPlayerEnemy, SetEnemy with the player's
	// faction and neither side made neutral, someone put in a PlayerEnemy faction; itself or through what it calls and
	// sets: SafeScan.Hostile) is no row here but HOSTILE's (tools/questscan.ps1 -Hostile), theirs and every other shared
	// quest's alike: a friend's waits until none of that faction is near, and is never skipped (up to 0.16.1 these 18, DN036
	// 500, DN049's x49, the Nucleus's 70, Acadia's 200, Far Harbor S02 210 and Kiddie Kingdom's 600, 700 and 740, were
	// kMoves|kHostile rows, set only beside the friend who did it, else offered and then skipped, while the story's own
	// hostile stages had no row and reached a far friend at once). A kMoves row whose stage is HOSTILE's too and that play
	// can set (5 of them, no kBridge: MS17 465/467/470 into the Covenant's fight, InstMassFusion 30 the Brotherhood's
	// kick-out, InstKickOut 100; the others, such as BoSM02 2-9 and BoSM04 2-4 into the airport's ghouls and Kiddie
	// Kingdom's 6, are quickstarts, never judged) moves the player too: the hold first,
	// then the move's rules, and the offer to go with the friend says they are fighting; not gone with, the stage isn't set
	// (it would move them) but the faction it turns on the player is turned on them here all the same (Verdict::kHostileOnly,
	// HostileCall). Their payments: DN049 711's pre-war money and DLC04DialogueSettlement 110's bloodworm meat, kPays.
	// The developers' quickstarts on the way to a faction's kick-out (Mass Fusion's DN084 1-9 and BoS303 0-3; the airport
	// battle's Inst307Fight 0-8; The Nuclear Option's MQ302 0-3 and MQ302RR 10, MQ206 0 and the story helper's MQ00 10,
	// 30, 40, 50 and 60; the Minutemen's Min301 0-1 and MinDestBoS 0; the Railroad's RR303 1-4 and RR304 1, 2, 20, 25-27
	// and 30: each jumps the player to a later point of the quest or sets what play would have set by then, a faction's
	// kick-out included, and the game never sets one in play: only the console does; Inst307Fight's 0 and 1 and RR304's 1
	// are the ones the others go through, and play starts those quests at Inst307Fight 10 and RR304 100) are kDebug rows,
	// by hand in the scan (RuleScan's DEBUG in tools/questscan.cs, which says why for each; tools/questscan.ps1 -NeverSet
	// lists every stage that nothing the scan can see sets from play, with the designers' own notes on them, which is
	// where these were found): never shared, see IsDebug, and neither is what they set, see QUICKSTART_SETS. So are the
	// other quests' quickstarts that set a stage a friend's game would be sent (156 kDebug rows in all: the Brotherhood's
	// own quests' BoS100 to BoS305 and BoSM00 to BoSM04, the story's MQ106, MQ202 to MQ204, the Railroad's RR101,
	// RR201, RR301 and RRM02, the Institute's, the add-ons' main quests and more), each with a designer's note naming it
	// a debug, test or quick start. A kDebug stage 0 is not its quest's start-up stage, and no stage that starts the quest
	// is judged by it (StartRule). Left unmarked: five quickstarts past stage 63 (QUICKSTART_SETS names them by a bit:
	// MS07c 190, MS16 89, MS05B 252 and 253, DLC04MS01 550), DN070 5 (the rig's follow step sets it at the host's
	// console as a friend's stage that moves the player), MQ105 0 (QuestSync's RepairStory judges its start of a stuck
	// MQ105 by that row) and the -NeverSet stages with no such note; they are shared as
	// their rows above say.
	inline constexpr Rule RULES[] = {
		{ 0, 0x003648, 64, kFree, 0 },   // DialogueDiamondCity: Home Plate bought from Geneva (sets FFDiamondCity02 10: its price, the key, the house)
		{ 0, 0x003648, 70, kPays, 0 },   // DialogueDiamondCity: the World Series bat sold for caps
		{ 0, 0x003648, 300, kFree, 0 },  // DialogueDiamondCity: Home Plate bought from the mayor (sets FFDiamondCity02 10 too)
		{ 0, 0x019955, 210, kPays, 0 },  // DialogueBunkerHill: 10 caps
		{ 0, 0x01CB51, 0, kMoves | kDebug, 40 },  // MS07a: MoveTo pPlayerDebugMarker [a quickstart]
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
		{ 0, 0x02125F, 30, kPays, 0 },  // FFDiamondCity03: a fern flower
		{ 0, 0x0229E6, 0, kMoves | kBridge, 20 },  // MQ105
		{ 0, 0x0229E6, 177, kPays, 0 },
		{ 0, 0x0229E7, 0, kMoves | kDebug, 40 },  // MQ106: MoveTo MQ106TestPlayerMarker [a quickstart]
		{ 0, 0x0229E7, 900, 0, 20 },
		{ 0, 0x0229E8, 0, kMoves | kBridge, 40 },  // MQ201
		{ 0, 0x0229E9, 0, kDebug, 0 },  // MQ202 [a quickstart]
		{ 0, 0x0229E9, 160, kMoves, 0 },  // MQ202
		{ 0, 0x0229E9, 300, kMoves, 100 },  // the lounger: sets MQ203 10, whose script fades out and moves the player into Kellogg's memory
		{ 0, 0x0229EA, 0, kMoves | kDebug, 0 },  // MQ203: FadeOutGame (by MQ202 300) [a quickstart]
		{ 0, 0x0229EA, 1, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 2, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 3, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 4, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 5, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 6, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 7, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 8, kMoves | kDebug, 0 },  // FadeOutGame (by 10) [a quickstart]
		{ 0, 0x0229EA, 10, kMoves, 100 },  // MQ203Script.StartQuest: into the memory
		{ 0, 0x0229EA, 1000, kMoves, 111 },  // MQ203Script.FinishQuest: out of it, back on the lounger
		{ 0, 0x0229EC, 5, 0, 10 },  // MQ207
		{ 0, 0x0229EC, 10, 0, 1 },
		{ 0, 0x0229EC, 190, kMoves, 60 },
		{ 0, 0x0229EC, 200, 0, 10 },
		{ 0, 0x0229EE, 0, kDebug, 0 },  // MQ302 [a quickstart]
		{ 0, 0x0229EE, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0229EE, 2, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0229EE, 3, kDebug, 0 },  // [a quickstart]
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
		{ 0, 0x0229FE, 0, kMoves | kDebug, 30 },  // MS07c: MoveTo pPlayerTestMarker [a quickstart]
		{ 0, 0x0229FE, 1, kMoves | kDebug, 20 },  // MoveTo pAudioTestMarker [a quickstart]
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
		{ 0, 0x022A07, 1, kMoves | kDebug, 20 },  // MS16: MoveTo PlayerStartSpot [a quickstart]
		{ 0, 0x022A07, 59, kMoves | kDebug, 50 },  // MoveTo temp29 [a quickstart]
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
		{ 0, 0x02BF20, 0, kDebug, 0 },  // BoS200 [a quickstart]
		{ 0, 0x02BF21, 0, kMoves | kBridge, 40 },  // BoS201
		{ 0, 0x02BF21, 1, kMoves | kDebug, 40 },  // BoS201: MoveTo BoS201DanseRoofMarker [a quickstart]
		{ 0, 0x02BF21, 2, kMoves | kDebug, 40 },  // MoveTo BoS201QuickStartPostMarker [a quickstart]
		{ 0, 0x02BF21, 3, kMoves | kDebug, 40 },  // MoveTo BoSPlayerPrydwenMarker [a quickstart]
		{ 0, 0x02BF21, 4, kMoves | kDebug, 40 },  // MoveTo BoSPlayerPrydwenMarker [a quickstart]
		{ 0, 0x02BF21, 10, 0, 120 },
		{ 0, 0x02BF21, 35, kMoves | kDebug, 20 },  // MoveTo BoS201QuickStartPostMarker [a quickstart]
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
		{ 0, 0x043275, 1, kMoves | kDebug, 1 },  // RR303: DisablePlayerControls (by RR101 600) [a quickstart]
		{ 0, 0x043275, 2, kDebug, 1 },  // [a quickstart]
		{ 0, 0x043275, 3, kDebug, 0 },  // [a quickstart]
		{ 0, 0x043275, 4, kDebug, 0 },  // [a quickstart]
		{ 0, 0x043275, 110, 0, 10 },
		{ 0, 0x043275, 200, 0, 1 },
		{ 0, 0x043275, 800, kMoves, 0 },
		{ 0, 0x043275, 900, 0, 100 },
		{ 0, 0x043275, 910, 0, 100 },
		{ 0, 0x043275, 1100, 0, 30 },
		{ 0, 0x043275, 1150, 0, 100 },
		{ 0, 0x043275, 11200, 0, 100 },
		{ 0, 0x04402C, 0, kMoves | kDebug, 20 },  // BoSM02: MoveTo BoSMQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x04402C, 2, kMoves | kDebug, 20 },  // MoveTo BoSM02SupplyDepotQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 3, kMoves | kDebug, 20 },  // MoveTo BoSM02SupplyDepotQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 4, kMoves | kDebug, 20 },  // MoveTo BoSM02PrydwenBerthsQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 5, kMoves | kDebug, 20 },  // MoveTo temp14 [a quickstart]
		{ 0, 0x04402C, 6, kMoves | kDebug, 20 },  // MoveTo BoSM02SupplyDepotQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 7, kMoves | kDebug, 20 },  // MoveTo DN165_BostonAirportRuinsCOCMarker [a quickstart]
		{ 0, 0x04402C, 8, kMoves | kDebug, 20 },  // MoveTo BoSM02ConfrontationQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 9, kMoves | kDebug, 20 },  // MoveTo BoSMQuickstartMarker [a quickstart]
		{ 0, 0x04402C, 150, 0, 60 },
		{ 0, 0x04402C, 210, kMoves, 0 },
		{ 0, 0x04542B, 711, kPays, 0 },  // DN049: RemoveItem(PrewarMoney x5000); its x49 stages, each shop's robots turned on the player, are HOSTILE's
		{ 0, 0x04542B, 856, 0, 50 },
		{ 0, 0x0459D2, 0, 0, 1 },  // RR101
		{ 0, 0x0459D2, 5, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0459D2, 6, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0459D2, 600, kMoves, 40 },
		{ 0, 0x0459D2, 710, 0, 3 },
		{ 0, 0x0498B5, 0, kMoves | kDebug, 20 },  // InstM01_OLD: MoveTo InstM03Playtest1 [a quickstart]
		{ 0, 0x0498B5, 35, kPays, 0 },   // 500 caps, as InstM01's 35
		{ 0, 0x0537FC, 0, kMoves | kDebug, 40 },  // BoS201B: MoveTo BoS201BStart [a quickstart]
		{ 0, 0x0537FF, 0, kMoves | kBridge, 60 },  // BoS202
		{ 0, 0x0537FF, 1, kMoves | kDebug, 40 },  // BoS202: MoveTo BoS202IntStart [a quickstart]
		{ 0, 0x0537FF, 2, kMoves | kDebug, 40 },  // MoveTo BoS202PilotStartMarker [a quickstart]
		{ 0, 0x0537FF, 30, kMoves, 0 },
		{ 0, 0x0537FF, 50, kMoves, 0 },
		{ 0, 0x055BE4, 0, kDebug, 0 },  // MinVsInst [a quickstart]
		{ 0, 0x055BE4, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x055BE4, 1500, 0, 10 },  // MinVsInst
		{ 0, 0x05BC93, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x05DDAB, 0, kMoves | kDebug, 20 },  // BoS100: MoveTo BoS00Start [a quickstart]
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
		{ 0, 0x06B500, 0, kMoves | kDebug, 0 },  // MQ204: FadeOutGame (by MQ203 1000) [a quickstart]
		{ 0, 0x06D646, 1, kMoves | kDebug, 20 },  // DN084: MoveTo DN084_BoS303QuickstartMarker [a quickstart]
		{ 0, 0x06D646, 2, kMoves | kDebug, 20 },  // MoveTo temp21 (by InstMassFusion 30) [a quickstart]
		{ 0, 0x06D646, 3, kMoves | kDebug, 60 },  // MoveTo DN084_DescentQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 4, kMoves | kDebug, 60 },  // MoveTo DN084_DescentQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 5, kMoves | kDebug, 20 },  // MoveTo DN084_LabQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 6, kMoves | kDebug, 20 },  // MoveTo DN084_LabQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 7, kMoves | kDebug, 20 },  // MoveTo DN084_FinalBattleQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 8, kMoves | kDebug, 20 },  // MoveTo DN084_FinalBattleQuickstartMarker [a quickstart]
		{ 0, 0x06D646, 9, kMoves | kDebug, 0 },  // MoveTo DN084_DescentQuickstartMarker (by 3) [a quickstart]
		{ 0, 0x06D646, 225, 0, 45 },
		{ 0, 0x06D646, 230, 0, 45 },
		{ 0, 0x06D646, 242, 0, 20 },
		{ 0, 0x06D646, 260, 0, 30 },
		{ 0, 0x06D646, 350, 0, 42 },
		{ 0, 0x06D646, 360, 0, 30 },
		{ 0, 0x06D646, 370, 0, 20 },
		{ 0, 0x06D646, 372, 0, 15 },
		{ 0, 0x06D646, 375, 0, 10 },
		{ 0, 0x06D646, 377, 0, 20 },
		{ 0, 0x06D646, 379, 0, 10 },
		{ 0, 0x06D646, 382, 0, 20 },
		{ 0, 0x06D646, 395, 0, 38 },
		{ 0, 0x06F5C1, 0, kMoves | kDebug, 20 },  // BoS101: MoveTo BoS101PlayerStartMarker [a quickstart]
		{ 0, 0x06F5C1, 1, kMoves | kDebug, 20 },  // MoveTo BoS01PlayerDebugStart [a quickstart]
		{ 0, 0x06F5C1, 2, kMoves | kDebug, 20 },  // MoveTo BoS101QSPlayerMarker [a quickstart]
		{ 0, 0x06FA37, 222, 0, 1 },  // RR102
		{ 0, 0x06FA37, 250, 0, 10 },
		{ 0, 0x06FA37, 700, 0, 50 },
		{ 0, 0x06FA37, 725, 0, 10 },
		{ 0, 0x07CC61, 10, kDebug, 0 },  // RR301 [a quickstart]
		{ 0, 0x07CC61, 100, 0, 1 },  // RR301
		{ 0, 0x07CC61, 200, 0, 10 },
		{ 0, 0x07CC61, 210, 0, 20 },
		{ 0, 0x084E40, 400, kPays, 0 },  // MS13MarowskiRevenge: caps
		{ 0, 0x0986C7, 0, kMoves | kDebug, 20 },  // InstM01: MoveTo InstM03Playtest1 [a quickstart]
		{ 0, 0x0986C7, 35, kPays, 0 },
		{ 0, 0x099848, 90, kPays, 0 },  // MinRecruit02
		{ 0, 0x09F09F, 15, 0, 20 },  // FFDiamondCity07FIN
		{ 0, 0x09FF4E, 0, kMoves | kBridge, 60 },  // BoS203
		{ 0, 0x0A274B, 0, kDebug, 0 },  // Inst305 [a quickstart]
		{ 0, 0x0A274B, 10, kMoves, 0 },
		{ 0, 0x0A274B, 60, 0, 50 },
		{ 0, 0x0A8257, 0, kMoves | kDebug, 20 },  // InstM02: MoveTo InstM04Playtest1 [a quickstart]
		{ 0, 0x0A8BAF, 5, kDebug, 10 },  // RRM02 [a quickstart]
		{ 0, 0x0A8BAF, 75, 0, 3 },
		{ 0, 0x0A8BAF, 100, 0, 8 },
		{ 0, 0x0A8BAF, 400, 0, 40 },
		{ 0, 0x0AA778, 0, 0, 10 },  // Min03
		{ 0, 0x0AA778, 400, 0, 10 },
		{ 0, 0x0ABF55, 500, 0, 30 },  // DN029FirstWave
		{ 0, 0x0AC7B3, 0, kDebug, 0 },  // Inst306 [a quickstart]
		{ 0, 0x0AC7B3, 20, 0, 50 },  // Inst306
		{ 0, 0x0ADCA6, 0, 0, 20 },  // DN029SecondWaveA
		{ 0, 0x0ADCE5, 0, kMoves | kDebug, 20 },  // BoSM04: MoveTo BoSMQuickstartMarker [a quickstart]
		{ 0, 0x0ADCE5, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0ADCE5, 2, kMoves | kDebug, 20 },  // MoveTo BoSM04VirgilQuickstartMarker [a quickstart]
		{ 0, 0x0ADCE5, 3, kMoves | kDebug, 20 },  // MoveTo BoSM04VirgilQuickstartMarker [a quickstart]
		{ 0, 0x0ADCE5, 4, kMoves | kDebug, 20 },  // MoveTo BoSMQuickstartMarker [a quickstart]
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
		{ 0, 0x0B1752, 0, kDebug, 0 },  // MQ206 [a quickstart]
		{ 0, 0x0B1752, 40, 0, 10 },  // MQ206
		{ 0, 0x0B1752, 500, kMoves | kBridge, 1 },
		{ 0, 0x0B1752, 1000, kMoves | kBridge, 40 },  // the teleporter: the only way into the Institute
		{ 0, 0x0B1752, 1050, 0, 20 },
		{ 0, 0x0B1D79, 0, kMoves | kDebug, 0 },  // BoSM01: MoveTo BoSM01_WarpPoliceStation (by 2) [a quickstart]
		{ 0, 0x0B1D79, 1, kMoves | kDebug, 20 },  // MoveTo BoSMQuickstartMarker [a quickstart]
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
		{ 0, 0x0B2D47, 0, kMoves | kDebug, 20 },  // InstM03: MoveTo InstM03Playtest1 [a quickstart]
		{ 0, 0x0B2D48, 5, kDebug, 0 },  // RR201 [a quickstart]
		{ 0, 0x0B2D48, 10, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0B2D48, 15, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0B2D48, 20, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0B2D48, 25, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0B2D48, 125, 0, 1 },  // RR201
		{ 0, 0x0B2D48, 1050, 0, 1 },
		{ 0, 0x0B3562, 0, kMoves, 20 },  // BoSM01Postquest
		{ 0, 0x0B8464, 15, 0, 20 },  // V81_00_Intro
		{ 0, 0x0B8464, 400, kPays, 0 },
		{ 0, 0x0B9F9D, 0, kMoves | kDebug, 40 },  // BoS302: MoveTo BoS302PlayerMarker [a quickstart]
		{ 0, 0x0B9F9D, 1, kMoves | kDebug, 40 },  // MoveTo BoS302PlayerBunkerGo [a quickstart]
		{ 0, 0x0B9F9D, 2, kMoves | kDebug, 40 },  // MoveTo BoS302PlayerBunkerExitGo [a quickstart]
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
		{ 0, 0x0C9B0B, 10, kDebug, 0 },  // MQ00 [a quickstart]
		{ 0, 0x0C9B0B, 30, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0C9B0B, 40, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0C9B0B, 50, kMoves | kDebug, 0 },  // DisablePlayerControls (by mq00script.EndGameBinks) [a quickstart]
		{ 0, 0x0C9B0B, 60, kMoves | kDebug, 1 },  // DisablePlayerControls (by mq00script.EndGameBinks) [a quickstart]
		{ 0, 0x0CF3E2, 0, kMoves | kBridge, 60 },  // BoSR04
		{ 0, 0x0D1EB2, 0, kMoves | kBridge, 60 },  // BoSR05
		{ 0, 0x0DDFDB, 0, kDebug, 0 },  // Min301 [a quickstart]
		{ 0, 0x0DDFDB, 1, kDebug, 0 },  // [a quickstart]
		{ 0, 0x0DFB3C, 0, kDebug, 0 },  // MinDestBoS [a quickstart]
		{ 0, 0x0E2058, 30, 0, 20 },  // Inst301
		{ 0, 0x0E2058, 45, 0, 3 },
		{ 0, 0x0E9914, 5, kMoves, 20 },  // DN070: MoveTo DN070_TreasuresEntry
		{ 0, 0x0E9914, 90, 0, 5 },
		{ 0, 0x0E9914, 95, 0, 20 },
		{ 0, 0x0E9914, 105, 0, 25 },
		{ 0, 0x0EBE71, 700, 0, 30 },  // MQ00MamaMurphy
		{ 0, 0x0FDC8C, 0, kMoves | kDebug, 50 },  // BoS303: MoveTo BoS303QSMarker [a quickstart]
		{ 0, 0x0FDC8C, 1, kMoves | kDebug, 50 },  // MoveTo DN084_BoS303QuickstartMarker [a quickstart]
		{ 0, 0x0FDC8C, 2, kMoves | kDebug, 30 },  // MoveTo DN084_DescentQuickstartMarker (by DN084 3) [a quickstart]
		{ 0, 0x0FDC8C, 3, kMoves | kDebug, 50 },  // MoveTo BoS303Start [a quickstart]
		{ 0, 0x0FDC8C, 40, 0, 30 },
		{ 0, 0x0FDC8C, 200, 0, 30 },
		{ 0, 0x0FDC8C, 210, 0, 30 },
		{ 0, 0x0FDC8C, 220, 0, 30 },
		{ 0, 0x0FF3C2, 600, 0, 130 },  // DN011
		{ 0, 0x1073CE, 700, 0, 20 },  // DN138
		{ 0, 0x107A1C, 0, kMoves | kDebug, 50 },  // BoS302B: MoveTo BoS302BStartMarker [a quickstart]
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
		{ 0, 0x10C64C, 10, kDebug, 0 },  // MQ302RR [a quickstart]
		{ 0, 0x10C64C, 330, 0, 52 },  // MQ302RR
		{ 0, 0x10C64C, 700, 0, 10 },
		{ 0, 0x122221, 400, 0, 1 },  // FFBunkerHill02
		{ 0, 0x134297, 0, kMoves | kBridge, 20 },  // MQ106Investigations
		{ 0, 0x136329, 0, kMoves | kBridge, 40 },  // Min207
		{ 0, 0x14191C, 10, kFree, 0 },  // FFDiamondCity02: Home Plate's price, its key and the house
		{ 0, 0x142EDC, 0, kMoves | kDebug, 20 },  // BoSM00: MoveTo BoSMQuickstartMarker [a quickstart]
		{ 0, 0x142FD3, 1, kMoves | kDebug, 0 },  // RR304: DisablePlayerControls (by RR303 1) [a quickstart]
		{ 0, 0x142FD3, 2, kDebug, 0 },  // [a quickstart]
		{ 0, 0x142FD3, 20, kMoves | kDebug, 20 },  // MoveTo temp8 [a quickstart]
		{ 0, 0x142FD3, 25, kMoves | kDebug, 30 },  // MoveTo pBoS201DanseRoofMarker [a quickstart]
		{ 0, 0x142FD3, 26, kDebug, 0 },  // [a quickstart]
		{ 0, 0x142FD3, 27, kDebug, 0 },  // [a quickstart]
		{ 0, 0x142FD3, 30, kMoves | kDebug, 25 },  // MoveTo pRR303PrydwenMarker [a quickstart]
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
		{ 0, 0x1666C5, 0, kMoves | kDebug, 40 },  // COMCaitQuest: MoveTo COMCaitQuestStart [a quickstart]
		{ 0, 0x16D036, 100, kMoves, 60 },  // InstKickOut
		{ 0, 0x173ED9, 0, kMoves | kDebug, 50 },  // BoS304: MoveTo BoS304Start [a quickstart]
		{ 0, 0x173ED9, 1, kMoves | kDebug, 50 },  // MoveTo BoS304QuickStart [a quickstart]
		{ 0, 0x173ED9, 2, kDebug, 20 },  // [a quickstart]
		{ 0, 0x173ED9, 40, 0, 70 },
		{ 0, 0x173ED9, 45, 0, 10 },
		{ 0, 0x173ED9, 57, 0, 10 },
		{ 0, 0x173ED9, 255, 0, 20 },
		{ 0, 0x174029, 0, kMoves | kDebug, 40 },  // Inst307Fight: MoveTo Inst307PlayerAirportMarkerRef [a quickstart]
		{ 0, 0x174029, 1, kMoves | kDebug, 0 },  // MoveTo temp30 (by Inst307 35) [a quickstart]
		{ 0, 0x174029, 2, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartTerminalEntrance [a quickstart]
		{ 0, 0x174029, 3, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartJammer1 [a quickstart]
		{ 0, 0x174029, 4, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartJammer2 [a quickstart]
		{ 0, 0x174029, 5, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartJammer3 [a quickstart]
		{ 0, 0x174029, 6, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartPhase2 [a quickstart]
		{ 0, 0x174029, 7, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartPhase3 [a quickstart]
		{ 0, 0x174029, 8, kMoves | kDebug, 20 },  // MoveTo Inst307Fight_QuickstartFrontEntrance [a quickstart]
		{ 0, 0x1764DF, 40, kPays, 0 },  // FFDiamondCity10
		{ 0, 0x17CA39, 10, kMoves | kBridge, 60 },  // Inst307ChangeLocation
		{ 0, 0x182EAE, 0, kDebug, 20 },  // BoS305 [a quickstart]
		{ 0, 0x1851A0, 10, 0, 20 },  // BoSEnable
		{ 0, 0x1851A0, 20, 0, 50 },
		{ 0, 0x1870D0, 0, 0, 40 },  // UFOCrashQuest
		{ 0, 0x1870D0, 5, 0, 40 },
		{ 0, 0x219B92, 0, kDebug, 0 },  // DN120Misc [a quickstart]
		{ 1, 0x001B3F, 500, 0, 10 },  // DLC03MQ01
		{ 1, 0x001B40, 0, kMoves | kBridge, 20 },  // DLC03MQ02
		{ 1, 0x001B40, 480, kPays, 0 },
		{ 1, 0x001B41, 210, kMoves, 40 },  // DLC03MQ03
		{ 1, 0x001B41, 220, kMoves, 60 },
		{ 1, 0x001B42, 1, kMoves | kDebug, 20 },  // DLC03MQ04: MoveTo temp3 [a quickstart]
		{ 1, 0x001B43, 200, kMoves, 55 },  // DLC03MQ05
		{ 1, 0x001B44, 1, kDebug, 20 },  // DLC03MQ06 [a quickstart]
		{ 1, 0x001B44, 2, kDebug, 20 },  // [a quickstart]
		{ 1, 0x001B44, 3, kDebug, 20 },  // [a quickstart]
		{ 1, 0x001B44, 120, 0, 40 },
		{ 1, 0x001B44, 190, 0, 80 },
		{ 1, 0x004C8E, 1, kMoves, 20 },  // DLC03CoA_DialogueNucleus: MoveTo DebugMarker (its 70, the Children of Atom's kick-out, is HOSTILE's)
		{ 1, 0x005BD7, 90, kPays, 0 },   // DLC03DialogueFarHarbor: the charge card sold for caps
		{ 1, 0x00A990, 0, kDebug, 0 },  // DLC03FarHarborFF03a [a quickstart]
		{ 1, 0x00A991, 0, kDebug, 0 },  // DLC03FarHarborS01 [a quickstart]
		{ 1, 0x00AEC3, 0, kDebug, 0 },  // DLC03FarHarborS02 [a quickstart]
		{ 1, 0x00AFB1, 0, kMoves | kDebug, 20 },  // DLC03AtomM01: MoveTo DebugMarker [a quickstart]
		{ 1, 0x00AFB1, 70, 0, 5 },
		{ 1, 0x01055C, 30, 0, 45 },  // DLC03CoA_FFNucleus01
		{ 1, 0x013F3A, 250, 0, 6 },  // DLC03Acadia_Institute (its 200, Acadia turned on the player, is HOSTILE's)
		{ 1, 0x013F3A, 460, 0, 32 },
		{ 1, 0x01CBFA, 1, kMoves, 50 },  // DLC03MQ04_01: into DiMA's memories (MQ04Script.EnterVR)
		{ 1, 0x01E3FE, 350, kMoves, 50 },  // DLC03MQ04_05: out of them
		{ 1, 0x02127F, 1, kMoves | kDebug, 75 },  // DLC03MQ06a: MoveTo DebugMarker [a quickstart]
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
		{ 1, 0x043822, 5, kMoves | kDebug, 20 },  // DLC03MQ02Fight: MoveTo DLC03MQ02Fight_PlayerQuickstartMarker2 [a quickstart]
		{ 1, 0x04B129, 0, kDebug, 0 },  // DLC03_V118_Quest2 [a quickstart]
		{ 1, 0x04D2F5, 200, kMoves, 60 },  // DLC03PostQuestTreasureDig: FadeOutGame
		{ 2, 0x000801, 0, kMoves | kDebug, 20 },  // DLC01MQ02: MoveTo CodsworthTalkMarker [a quickstart]
		{ 2, 0x0010F5, 0, kMoves | kDebug, 40 },  // DLC01MQ05: MoveTo CodsworthTalkMarker [a quickstart]
		{ 2, 0x0010F5, 1, kMoves, 20 },  // DLC01MQ05: MoveTo CodsworthTalkMarker (quickstarts)
		{ 2, 0x0010F5, 2, kMoves, 20 },
		{ 2, 0x002833, 0, kMoves | kDebug, 20 },  // DLC01MQ04: MoveTo CodsworthTalkMarker [a quickstart]
		{ 2, 0x002833, 1, kMoves | kDebug, 20 },  // MoveTo CodsworthTalkMarker [a quickstart]
		{ 2, 0x002833, 2, kMoves | kDebug, 20 },  // MoveTo DLC01MQ04HagenStart [a quickstart]
		{ 2, 0x002833, 3, kMoves | kDebug, 20 },  // MoveTo CodsworthTalkMarker [a quickstart]
		{ 3, 0x000801, 0, kMoves | kDebug, 20 },  // DLC04MQ01: MoveTo DLC04MQ01StartRef [a quickstart]
		{ 3, 0x000801, 1, kMoves | kDebug, 0 },  // MoveTo DLC04MQ01Gauntlet_QuickstartColaCarsEntrance (by DLC04MQ01Gauntlet 5) [a quickstart]
		{ 3, 0x000801, 2, kMoves | kDebug, 0 },  // MoveTo DLC04MQ01Gauntlet_QuickstartLockerRoomEntrance (by DLC04MQ01Gauntlet 6) [a quickstart]
		{ 3, 0x000801, 3, kMoves | kDebug, 0 },  // MoveTo DLC04MQ01Gauntlet_QuickstartPostfight (by DLC04MQ01Gauntlet 9) [a quickstart]
		{ 3, 0x000801, 217, 0, 20 },
		{ 3, 0x000801, 1000, kMoves, 0 },  // DLC04MQ01Gauntlet 2500: the player through DLC04NukaStationStairwellDoorChain
		{ 3, 0x000802, 0, kMoves | kDebug, 20 },  // DLC04MQ02: MoveTo DLC04MQ02StartMarkerRef [a quickstart]
		{ 3, 0x000803, 0, kMoves | kDebug, 20 },  // DLC04MQ03: MoveTo DLC04MQ03Start [a quickstart]
		{ 3, 0x000803, 1, kMoves | kDebug, 20 },  // MoveTo DLC04GZFlagPoleMarkerRef [a quickstart]
		{ 3, 0x000803, 2, kMoves | kDebug, 20 },  // MoveTo DLC04DRGFlagPoleMarkerRef [a quickstart]
		{ 3, 0x000803, 3, kMoves | kDebug, 20 },  // MoveTo DLC04KKFlagPoleMarkerRef [a quickstart]
		{ 3, 0x000803, 4, kMoves | kDebug, 20 },  // MoveTo DLC04SAFlagPoleMarkerRef [a quickstart]
		{ 3, 0x000803, 5, kMoves | kDebug, 20 },  // MoveTo DLC04BPFlagPoleMarkerRef [a quickstart]
		{ 3, 0x000804, 1, kMoves | kDebug, 20 },  // DLC04MQ04: MoveTo PlayerDebugMarker [a quickstart]
		{ 3, 0x000804, 2, kMoves | kDebug, 20 },  // MoveTo PlayerDebugMarker (by 1) [a quickstart]
		{ 3, 0x000804, 3, kMoves | kDebug, 100 },  // MoveTo PlayerDebugMarker [a quickstart]
		{ 3, 0x000804, 4, kMoves | kDebug, 20 },  // MoveTo PlayerDebugMarker [a quickstart]
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
		{ 3, 0x014352, 90, kPays, 0 },  // DLC04RaidChaseOff: caps
		{ 3, 0x016E18, 95, 0, 10 },     // DLC04RaidSubdue
		{ 3, 0x019088, 90, kPays, 0 },  // DLC04RaidCoerce: caps
		{ 3, 0x01AABF, 110, kPays, 0 },  // DLC04DialogueSettlement: RemoveItem(DLC04_MeatBloodworm x10)
		{ 3, 0x0295C8, 50, 0, 100 },  // DLC04GZBattlezone
		{ 3, 0x0295C8, 60, 0, 20 },
		{ 3, 0x03684F, 6, kMoves | kDebug, 50 },  // DLC04_KiddieKingdomMain: MoveTo temp6 [a quickstart]
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

	// A developer's quickstart (a kDebug row): a stage the game never sets in play, only the console does, which jumps the
	// player to a later point of its quest and sets what play would have set by then (DN084 2: the relay to Mass Fusion's
	// roof and the Brotherhood's kick-out with it). Not a player's progress and nobody's choice: never reported, left out
	// of the host's baseline, and dropped when a friend's comes all the same (an older version's report, a session file
	// that has one), as the opening's stages are; one that reached the queue is skipped (Judge). What such a stage did in
	// the game that set it is that game's own too: the stages its script set there are not reported either, while the
	// quickstart is done in that game (QUICKSTART_SETS, below, which says exactly which and what is still reported).
	inline bool IsDebug(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto rule = Find(a_file, a_quest, a_stage);
		return rule && (rule->flags & kDebug);
	}

	// What a developer's quickstart sets in the game where the console set it. A console quickstart is the typing player's
	// own, and so is everything its script did there: the stages it set, itself or through the stages it sets, the
	// functions it calls and the quests it starts (their start-up stages), as far down as that goes. While a quickstart
	// (byQuest's stage n, bit n of byStages) is done in a game, that game reports none of the stages its rows name, live
	// or in the host's baseline (QuestSync.cpp: SetByQuickstart), so no friend is kicked out of a faction or moved on
	// through a quest on the strength of a stage nobody played: DN084 2, 4, 6 and 8 set BoSKickOut 30 (and 2 InstMassFusion
	// 30, whose own script sets BoSKickOut 20); DN084 1, 3, 5, 7 and 9 set BoS303 30, which makes the Institute the
	// player's enemy and, for a player in the Institute, starts its kick-out, as BoS303 1 and 2 do (2 sets DN084 3 as
	// well); Inst307Fight 0 to 8 set BoSKickOut 20, through its 1, and so do RR304 20, 25 and 30, through its 1 and RR303
	// 1; MQ302 2, Min301 0 and 1 and MinDestBoS 0 start the Institute's kick-out too; and the Brotherhood's own
	// quickstarts (BoS101 0 to BoSM04 4) set its questline done up to their point, in a friend's game too if they were
	// sent. Every kDebug row's set is here (1777 rows). Exactly what that covers:
	//  - It goes by the quickstart's stage being done in the game, which the save keeps, not by when it was set: the
	//    rows' stages are left out of every later baseline from that save too. So a stage of the rows that play set in
	//    that game (before the console did; or later, where the script had not: next point) is not reported from it
	//    either. What the session already holds stays in it.
	//  - The rows are every stage the script can set, both sides of each branch that the game's state decides, so more
	//    than one run of it sets (DN084 3 to 9 have InstMassFusion's 40 to 70 beside BoS303's, where the script sets one
	//    quest's, by the side the quickstart took; BoS303 30 starts InstKickOut only for a player who is in the Institute).
	//  - Only what the scan follows from the scripts: a SetStage with a literal stage, or a Start, on a quest it can name
	//    (the script's own, the quest a property holds in the quest's record, one named by the property or by its script).
	//    Not followed, and so reported as any other stage when the game sets it: a stage that is no literal (the
	//    companions' affinity thresholds, under both kick-outs), and whatever is set later by something the script only
	//    starts or waits for: a scene, a timer, an event, a line of dialogue, a terminal. tools/questscan.ps1 -Quickstarts
	//    lists each call it did not follow.
	//  - Quest stages, and the "quest completed" line of a quest whose current stage is one of them. What the script did
	//    to the world of the game that ran it (who it killed, enabled or moved, a faction now that player's enemy) is that
	//    game's state, and the other sync systems carry it as they carry any.
	//  - What is played on from there is play, and is reported: DN084 2 at the console and then the building fought
	//    through sends DN084's and InstMassFusion's later stages. A friend's game sets them without the ones left out;
	//    docs/DESIGN.md ("A console quickstart is the typing player's own") says what that does there.
	// Generated with tools/questscan.ps1 -Quickstarts (QuickstartScan and MoveScan.Sets in tools/questscan.cs). Sorted by
	// file, quest and stage, then the quickstart's file and quest.
	struct QuickstartSet
	{
		std::uint8_t  file;      // File: the quest's whose stage a quickstart's script sets
		std::uint32_t quest;
		std::uint16_t stage;
		std::uint8_t  byFile;    // File: the quickstart's quest's
		std::uint32_t byQuest;
		std::uint64_t byStages;  // bit n: that quest's stage n, a kDebug row, sets it (a quickstart's stage is below 64)
	};
	inline constexpr QuickstartSet QUICKSTART_SETS[] = {
		{ 0, 0x003648, 170, 0, 0x0229FE, 0x0003 },  // DialogueDiamondCity 170 <- MS07c 0, 1
		{ 0, 0x003648, 600, 0, 0x0229FE, 0x0003 },  // DialogueDiamondCity 600 <- MS07c 0, 1
		{ 0, 0x0179F3, 100, 0, 0x0229EE, 0x0004 },  // DialogueConcordArea 100 <- MQ302 2
		{ 0, 0x0179F3, 100, 0, 0x0DDFDB, 0x0003 },  // DialogueConcordArea 100 <- Min301 0, 1
		{ 0, 0x0179F3, 100, 0, 0x0DFB3C, 0x0001 },  // DialogueConcordArea 100 <- MinDestBoS 0
		{ 0, 0x0179F3, 200, 0, 0x0229EE, 0x0004 },  // DialogueConcordArea 200 <- MQ302 2
		{ 0, 0x0179F3, 200, 0, 0x0DDFDB, 0x0003 },  // DialogueConcordArea 200 <- Min301 0, 1
		{ 0, 0x0179F3, 200, 0, 0x0DFB3C, 0x0001 },  // DialogueConcordArea 200 <- MinDestBoS 0
		{ 0, 0x0190BC, 0, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 0 <- MQ106 0
		{ 0, 0x0190BC, 0, 0, 0x0459D2, 0x0040 },  // DialogueDiamondCityEntrance 0 <- RR101 6
		{ 0, 0x0190BC, 23, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 23 <- MQ106 0
		{ 0, 0x0190BC, 23, 0, 0x0459D2, 0x0040 },  // DialogueDiamondCityEntrance 23 <- RR101 6
		{ 0, 0x0190BC, 25, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 25 <- MQ106 0
		{ 0, 0x0190BC, 25, 0, 0x0459D2, 0x0040 },  // DialogueDiamondCityEntrance 25 <- RR101 6
		{ 0, 0x0190BC, 30, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 30 <- MQ106 0
		{ 0, 0x0190BC, 30, 0, 0x0459D2, 0x0040 },  // DialogueDiamondCityEntrance 30 <- RR101 6
		{ 0, 0x0190BC, 50, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 50 <- MQ106 0
		{ 0, 0x0190BC, 1000, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityEntrance 1000 <- MQ106 0
		{ 0, 0x01CB51, 10, 0, 0x01CB51, 0x0001 },  // MS07a 10 <- MS07a 0
		{ 0, 0x01CB51, 15, 0, 0x01CB51, 0x0001 },  // MS07a 15 <- MS07a 0
		{ 0, 0x01CC2A, 10, 2, 0x000801, 0x0001 },  // MQ102 10 <- DLC01MQ02 0
		{ 0, 0x01CC2A, 10, 2, 0x0010F5, 0x0001 },  // MQ102 10 <- DLC01MQ05 0
		{ 0, 0x01CC2A, 10, 2, 0x002833, 0x000F },  // MQ102 10 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x01CC2A, 15, 0, 0x04402C, 0x03FF },  // MQ102 15 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x01CC2A, 15, 0, 0x0ADCE5, 0x001F },  // MQ102 15 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x01CC2A, 15, 0, 0x0B1D79, 0x0003 },  // MQ102 15 <- BoSM01 0, 1
		{ 0, 0x01CC2A, 15, 0, 0x142EDC, 0x0001 },  // MQ102 15 <- BoSM00 0
		{ 0, 0x01CC2A, 15, 2, 0x000801, 0x0001 },  // MQ102 15 <- DLC01MQ02 0
		{ 0, 0x01CC2A, 15, 2, 0x0010F5, 0x0001 },  // MQ102 15 <- DLC01MQ05 0
		{ 0, 0x01CC2A, 15, 2, 0x002833, 0x000F },  // MQ102 15 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x01CC2A, 20, 2, 0x000801, 0x0001 },  // MQ102 20 <- DLC01MQ02 0
		{ 0, 0x01CC2A, 20, 2, 0x0010F5, 0x0001 },  // MQ102 20 <- DLC01MQ05 0
		{ 0, 0x01CC2A, 20, 2, 0x002833, 0x000F },  // MQ102 20 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x01CC2A, 50, 2, 0x0010F5, 0x0001 },  // MQ102 50 <- DLC01MQ05 0
		{ 0, 0x01CC2A, 55, 0, 0x0229EE, 0x0004 },  // MQ102 55 <- MQ302 2
		{ 0, 0x01CC2A, 55, 0, 0x0DDFDB, 0x0003 },  // MQ102 55 <- Min301 0, 1
		{ 0, 0x01CC2A, 55, 0, 0x0DFB3C, 0x0001 },  // MQ102 55 <- MinDestBoS 0
		{ 0, 0x01CC2A, 61, 0, 0x0229EE, 0x0004 },  // MQ102 61 <- MQ302 2
		{ 0, 0x01CC2A, 61, 0, 0x0DDFDB, 0x0003 },  // MQ102 61 <- Min301 0, 1
		{ 0, 0x01CC2A, 61, 0, 0x0DFB3C, 0x0001 },  // MQ102 61 <- MinDestBoS 0
		{ 0, 0x01CC2A, 85, 0, 0x0229EE, 0x0004 },  // MQ102 85 <- MQ302 2
		{ 0, 0x01CC2A, 85, 0, 0x0DDFDB, 0x0003 },  // MQ102 85 <- Min301 0, 1
		{ 0, 0x01CC2A, 85, 0, 0x0DFB3C, 0x0001 },  // MQ102 85 <- MinDestBoS 0
		{ 0, 0x01CC2A, 90, 0, 0x0229EE, 0x0004 },  // MQ102 90 <- MQ302 2
		{ 0, 0x01CC2A, 90, 0, 0x0DDFDB, 0x0003 },  // MQ102 90 <- Min301 0, 1
		{ 0, 0x01CC2A, 90, 0, 0x0DFB3C, 0x0001 },  // MQ102 90 <- MinDestBoS 0
		{ 0, 0x01CC2A, 100, 0, 0x0229EE, 0x0004 },  // MQ102 100 <- MQ302 2
		{ 0, 0x01CC2A, 100, 0, 0x0DDFDB, 0x0003 },  // MQ102 100 <- Min301 0, 1
		{ 0, 0x01CC2A, 100, 0, 0x0DFB3C, 0x0001 },  // MQ102 100 <- MinDestBoS 0
		{ 0, 0x01CC2A, 130, 0, 0x0229EE, 0x0004 },  // MQ102 130 <- MQ302 2
		{ 0, 0x01CC2A, 130, 0, 0x0DDFDB, 0x0003 },  // MQ102 130 <- Min301 0, 1
		{ 0, 0x01CC2A, 130, 0, 0x0DFB3C, 0x0001 },  // MQ102 130 <- MinDestBoS 0
		{ 0, 0x01CC2A, 140, 0, 0x0229EE, 0x0004 },  // MQ102 140 <- MQ302 2
		{ 0, 0x01CC2A, 140, 0, 0x0DDFDB, 0x0003 },  // MQ102 140 <- Min301 0, 1
		{ 0, 0x01CC2A, 140, 0, 0x0DFB3C, 0x0001 },  // MQ102 140 <- MinDestBoS 0
		{ 0, 0x01CC2A, 150, 0, 0x0229EE, 0x0004 },  // MQ102 150 <- MQ302 2
		{ 0, 0x01CC2A, 150, 0, 0x0DDFDB, 0x0003 },  // MQ102 150 <- Min301 0, 1
		{ 0, 0x01CC2A, 150, 0, 0x0DFB3C, 0x0001 },  // MQ102 150 <- MinDestBoS 0
		{ 0, 0x01CC2A, 200, 0, 0x0229EE, 0x0004 },  // MQ102 200 <- MQ302 2
		{ 0, 0x01CC2A, 200, 0, 0x0DDFDB, 0x0003 },  // MQ102 200 <- Min301 0, 1
		{ 0, 0x01CC2A, 200, 0, 0x0DFB3C, 0x0001 },  // MQ102 200 <- MinDestBoS 0
		{ 0, 0x01D806, 1, 0, 0x0229EE, 0x0004 },  // DN092 1 <- MQ302 2
		{ 0, 0x01D806, 1, 0, 0x0DDFDB, 0x0003 },  // DN092 1 <- Min301 0, 1
		{ 0, 0x01D806, 1, 0, 0x0DFB3C, 0x0001 },  // DN092 1 <- MinDestBoS 0
		{ 0, 0x01D806, 60, 0, 0x0229EE, 0x0004 },  // DN092 60 <- MQ302 2
		{ 0, 0x01D806, 60, 0, 0x0DDFDB, 0x0003 },  // DN092 60 <- Min301 0, 1
		{ 0, 0x01D806, 60, 0, 0x0DFB3C, 0x0001 },  // DN092 60 <- MinDestBoS 0
		{ 0, 0x01E5E7, 20, 0, 0x0FDC8C, 0x000F },  // BoSDansePostGame 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x01E5E7, 20, 0, 0x107A1C, 0x0001 },  // BoSDansePostGame 20 <- BoS302B 0
		{ 0, 0x01E5E7, 20, 0, 0x173ED9, 0x0007 },  // BoSDansePostGame 20 <- BoS304 0, 1, 2
		{ 0, 0x01E64E, 5, 0, 0x0229EE, 0x0004 },  // DialogueCodsworthPostWar 5 <- MQ302 2
		{ 0, 0x01E64E, 5, 0, 0x0DDFDB, 0x0003 },  // DialogueCodsworthPostWar 5 <- Min301 0, 1
		{ 0, 0x01E64E, 5, 0, 0x0DFB3C, 0x0001 },  // DialogueCodsworthPostWar 5 <- MinDestBoS 0
		{ 0, 0x01E64E, 10, 2, 0x0010F5, 0x0001 },  // DialogueCodsworthPostWar 10 <- DLC01MQ05 0
		{ 0, 0x01E64E, 35, 2, 0x0010F5, 0x0001 },  // DialogueCodsworthPostWar 35 <- DLC01MQ05 0
		{ 0, 0x01E64E, 40, 2, 0x0010F5, 0x0001 },  // DialogueCodsworthPostWar 40 <- DLC01MQ05 0
		{ 0, 0x01E64E, 200, 2, 0x0010F5, 0x0001 },  // DialogueCodsworthPostWar 200 <- DLC01MQ05 0
		{ 0, 0x01E64E, 250, 2, 0x0010F5, 0x0001 },  // DialogueCodsworthPostWar 250 <- DLC01MQ05 0
		{ 0, 0x01ED62, 110, 2, 0x000801, 0x0001 },  // DialogueVault111 110 <- DLC01MQ02 0
		{ 0, 0x01ED62, 110, 2, 0x0010F5, 0x0001 },  // DialogueVault111 110 <- DLC01MQ05 0
		{ 0, 0x01ED62, 110, 2, 0x002833, 0x000F },  // DialogueVault111 110 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x01F25E, 10, 0, 0x0229FE, 0x0003 },  // MQ104 10 <- MS07c 0, 1
		{ 0, 0x0229E5, 10, 0, 0x0229E7, 0x0001 },  // MQ103 10 <- MQ106 0
		{ 0, 0x0229E5, 10, 0, 0x0229EE, 0x0004 },  // MQ103 10 <- MQ302 2
		{ 0, 0x0229E5, 10, 0, 0x0459D2, 0x0040 },  // MQ103 10 <- RR101 6
		{ 0, 0x0229E5, 10, 0, 0x0DDFDB, 0x0003 },  // MQ103 10 <- Min301 0, 1
		{ 0, 0x0229E5, 10, 0, 0x0DFB3C, 0x0001 },  // MQ103 10 <- MinDestBoS 0
		{ 0, 0x0229E5, 20, 0, 0x0229FE, 0x0003 },  // MQ103 20 <- MS07c 0, 1
		{ 0, 0x0229E5, 30, 0, 0x0229FE, 0x0003 },  // MQ103 30 <- MS07c 0, 1
		{ 0, 0x0229E5, 100, 0, 0x0229FE, 0x0003 },  // MQ103 100 <- MS07c 0, 1
		{ 0, 0x0229E6, 310, 0, 0x0229E7, 0x0001 },  // MQ105 310 <- MQ106 0
		{ 0, 0x0229E6, 320, 0, 0x0229E7, 0x0001 },  // MQ105 320 <- MQ106 0
		{ 0, 0x0229E6, 410, 0, 0x0229E7, 0x0001 },  // MQ105 410 <- MQ106 0
		{ 0, 0x0229E6, 510, 0, 0x0229E7, 0x0001 },  // MQ105 510 <- MQ106 0
		{ 0, 0x0229E6, 1000, 0, 0x0229E7, 0x0001 },  // MQ105 1000 <- MQ106 0
		{ 0, 0x0229E7, 10, 0, 0x0229E7, 0x0001 },  // MQ106 10 <- MQ106 0
		{ 0, 0x0229E7, 2000, 0, 0x0229E9, 0x0001 },  // MQ106 2000 <- MQ202 0
		{ 0, 0x0229E7, 2000, 0, 0x0229EA, 0x0001 },  // MQ106 2000 <- MQ203 0
		{ 0, 0x0229E8, 150, 0, 0x0229E9, 0x0001 },  // MQ201 150 <- MQ202 0
		{ 0, 0x0229E8, 150, 0, 0x0229EA, 0x0001 },  // MQ201 150 <- MQ203 0
		{ 0, 0x0229E9, 10, 0, 0x0229E9, 0x0001 },  // MQ202 10 <- MQ202 0
		{ 0, 0x0229E9, 10, 0, 0x0229EA, 0x0001 },  // MQ202 10 <- MQ203 0
		{ 0, 0x0229E9, 300, 0, 0x0229EA, 0x0001 },  // MQ202 300 <- MQ203 0
		{ 0, 0x0229E9, 310, 0, 0x0229EA, 0x01FF },  // MQ202 310 <- MQ203 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 10, 0, 0x0229EA, 0x01FF },  // MQ203 10 <- MQ203 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 20, 0, 0x0229EA, 0x01FF },  // MQ203 20 <- MQ203 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 100, 0, 0x0229EA, 0x01FF },  // MQ203 100 <- MQ203 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 200, 0, 0x0229EA, 0x01FC },  // MQ203 200 <- MQ203 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 220, 0, 0x0229EA, 0x0004 },  // MQ203 220 <- MQ203 2
		{ 0, 0x0229EA, 300, 0, 0x0229EA, 0x01F8 },  // MQ203 300 <- MQ203 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 320, 0, 0x0229EA, 0x0008 },  // MQ203 320 <- MQ203 3
		{ 0, 0x0229EA, 400, 0, 0x0229EA, 0x01F0 },  // MQ203 400 <- MQ203 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 420, 0, 0x0229EA, 0x0010 },  // MQ203 420 <- MQ203 4
		{ 0, 0x0229EA, 500, 0, 0x0229EA, 0x01E0 },  // MQ203 500 <- MQ203 5, 6, 7, 8
		{ 0, 0x0229EA, 520, 0, 0x0229EA, 0x0020 },  // MQ203 520 <- MQ203 5
		{ 0, 0x0229EA, 600, 0, 0x0229EA, 0x01C0 },  // MQ203 600 <- MQ203 6, 7, 8
		{ 0, 0x0229EA, 620, 0, 0x0229EA, 0x0040 },  // MQ203 620 <- MQ203 6
		{ 0, 0x0229EA, 700, 0, 0x0229EA, 0x0180 },  // MQ203 700 <- MQ203 7, 8
		{ 0, 0x0229EA, 720, 0, 0x0229EA, 0x0180 },  // MQ203 720 <- MQ203 7, 8
		{ 0, 0x0229EA, 1000, 0, 0x0229EA, 0x01FE },  // MQ203 1000 <- MQ203 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0229EA, 1000, 0, 0x06B500, 0x0001 },  // MQ203 1000 <- MQ204 0
		{ 0, 0x0229EB, 10, 0, 0x0229EE, 0x000F },  // MQ205 10 <- MQ302 0, 1, 2, 3
		{ 0, 0x0229EB, 10, 0, 0x0B1752, 0x0001 },  // MQ205 10 <- MQ206 0
		{ 0, 0x0229EB, 10, 0, 0x0DDFDB, 0x0003 },  // MQ205 10 <- Min301 0, 1
		{ 0, 0x0229EB, 10, 0, 0x0DFB3C, 0x0001 },  // MQ205 10 <- MinDestBoS 0
		{ 0, 0x0229EB, 100, 0, 0x0229EE, 0x000F },  // MQ205 100 <- MQ302 0, 1, 2, 3
		{ 0, 0x0229EB, 100, 0, 0x0B1752, 0x0001 },  // MQ205 100 <- MQ206 0
		{ 0, 0x0229EB, 100, 0, 0x0DDFDB, 0x0003 },  // MQ205 100 <- Min301 0, 1
		{ 0, 0x0229EB, 100, 0, 0x0DFB3C, 0x0001 },  // MQ205 100 <- MinDestBoS 0
		{ 0, 0x0229EC, 0, 0, 0x0229EE, 0x0004 },  // MQ207 0 <- MQ302 2
		{ 0, 0x0229EC, 0, 0, 0x0DDFDB, 0x0003 },  // MQ207 0 <- Min301 0, 1
		{ 0, 0x0229EC, 0, 0, 0x0DFB3C, 0x0001 },  // MQ207 0 <- MinDestBoS 0
		{ 0, 0x0229EC, 180, 0, 0x0229EE, 0x0004 },  // MQ207 180 <- MQ302 2
		{ 0, 0x0229EC, 180, 0, 0x0DDFDB, 0x0003 },  // MQ207 180 <- Min301 0, 1
		{ 0, 0x0229EC, 180, 0, 0x0DFB3C, 0x0001 },  // MQ207 180 <- MinDestBoS 0
		{ 0, 0x0229EC, 200, 0, 0x0229EE, 0x0004 },  // MQ207 200 <- MQ302 2
		{ 0, 0x0229EC, 200, 0, 0x0DDFDB, 0x0003 },  // MQ207 200 <- Min301 0, 1
		{ 0, 0x0229EC, 200, 0, 0x0DFB3C, 0x0001 },  // MQ207 200 <- MinDestBoS 0
		{ 0, 0x0229EE, 10, 0, 0x0229EE, 0x000F },  // MQ302 10 <- MQ302 0, 1, 2, 3
		{ 0, 0x0229EE, 10, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302 10 <- MQ00 50, 60
		{ 0, 0x0229EE, 10, 0, 0x0DFB3C, 0x0001 },  // MQ302 10 <- MinDestBoS 0
		{ 0, 0x0229EE, 10, 0, 0x10C64C, 0x0400 },  // MQ302 10 <- MQ302RR 10
		{ 0, 0x0229EE, 100, 0, 0x0229EE, 0x000F },  // MQ302 100 <- MQ302 0, 1, 2, 3
		{ 0, 0x0229EE, 100, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302 100 <- MQ00 50, 60
		{ 0, 0x0229EE, 100, 0, 0x0DFB3C, 0x0001 },  // MQ302 100 <- MinDestBoS 0
		{ 0, 0x0229EE, 100, 0, 0x10C64C, 0x0400 },  // MQ302 100 <- MQ302RR 10
		{ 0, 0x0229EE, 900, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302 900 <- MQ00 50, 60
		{ 0, 0x0229EE, 1000, 0, 0x0DFB3C, 0x0001 },  // MQ302 1000 <- MinDestBoS 0
		{ 0, 0x0229FE, 2, 0, 0x0229FE, 0x0001 },  // MS07c 2 <- MS07c 0
		{ 0, 0x0229FE, 210, 0, 0x0229FE, 0x0002 },  // MS07c 210 <- MS07c 1
		{ 0, 0x0229FE, 214, 0, 0x0229FE, 0x0002 },  // MS07c 214 <- MS07c 1
		{ 0, 0x0229FE, 217, 0, 0x0229FE, 0x0002 },  // MS07c 217 <- MS07c 1
		{ 0, 0x022A07, 10, 0, 0x022A07, 0x0002 },  // MS16 10 <- MS16 1
		{ 0, 0x022A07, 60, 0, 0x022A07, 0x800000000000000 },  // MS16 60 <- MS16 59
		{ 0, 0x022A07, 65, 0, 0x022A07, 0x800000000000000 },  // MS16 65 <- MS16 59
		{ 0, 0x022A08, 180, 0, 0x0DFB3C, 0x0001 },  // MS17 180 <- MinDestBoS 0
		{ 0, 0x022A08, 765, 0, 0x0DFB3C, 0x0001 },  // MS17 765 <- MinDestBoS 0
		{ 0, 0x022A08, 770, 0, 0x0DFB3C, 0x0001 },  // MS17 770 <- MinDestBoS 0
		{ 0, 0x023802, 3, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityMayorWallSpeech 3 <- MQ106 0
		{ 0, 0x023802, 100, 0, 0x0229E7, 0x0001 },  // DialogueDiamondCityMayorWallSpeech 100 <- MQ106 0
		{ 0, 0x02AFE9, 10, 0, 0x0FDC8C, 0x000F },  // BoSPAMPostGame 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x02AFE9, 10, 0, 0x173ED9, 0x0007 },  // BoSPAMPostGame 10 <- BoS304 0, 1, 2
		{ 0, 0x02B4E9, 10, 0, 0x0229EE, 0x0004 },  // Inst303 10 <- MQ302 2
		{ 0, 0x02B4E9, 10, 0, 0x06D646, 0x02AA },  // Inst303 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x02B4E9, 10, 0, 0x0DDFDB, 0x0003 },  // Inst303 10 <- Min301 0, 1
		{ 0, 0x02B4E9, 10, 0, 0x0DFB3C, 0x0001 },  // Inst303 10 <- MinDestBoS 0
		{ 0, 0x02B4E9, 10, 0, 0x0FDC8C, 0x0006 },  // Inst303 10 <- BoS303 1, 2
		{ 0, 0x02B4E9, 1500, 0, 0x0229EE, 0x0004 },  // Inst303 1500 <- MQ302 2
		{ 0, 0x02B4E9, 1500, 0, 0x06D646, 0x02AA },  // Inst303 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x02B4E9, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst303 1500 <- Min301 0, 1
		{ 0, 0x02B4E9, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst303 1500 <- MinDestBoS 0
		{ 0, 0x02B4E9, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst303 1500 <- BoS303 1, 2
		{ 0, 0x02BF20, 10, 0, 0x02BF20, 0x0001 },  // BoS200 10 <- BoS200 0
		{ 0, 0x02BF20, 10, 0, 0x02BF21, 0x001E },  // BoS200 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x02BF20, 10, 0, 0x04402C, 0x03FF },  // BoS200 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF20, 10, 0, 0x0537FC, 0x0001 },  // BoS200 10 <- BoS201B 0
		{ 0, 0x02BF20, 10, 0, 0x0537FF, 0x0006 },  // BoS200 10 <- BoS202 1, 2
		{ 0, 0x02BF20, 10, 0, 0x0ADCE5, 0x001F },  // BoS200 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF20, 10, 0, 0x0B1D79, 0x0003 },  // BoS200 10 <- BoSM01 0, 1
		{ 0, 0x02BF20, 10, 0, 0x0B9F9D, 0x0007 },  // BoS200 10 <- BoS302 0, 1, 2
		{ 0, 0x02BF20, 10, 0, 0x0FDC8C, 0x000F },  // BoS200 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x02BF20, 10, 0, 0x107A1C, 0x0001 },  // BoS200 10 <- BoS302B 0
		{ 0, 0x02BF20, 10, 0, 0x142EDC, 0x0001 },  // BoS200 10 <- BoSM00 0
		{ 0, 0x02BF20, 10, 0, 0x173ED9, 0x0007 },  // BoS200 10 <- BoS304 0, 1, 2
		{ 0, 0x02BF20, 20, 0, 0x04402C, 0x03FF },  // BoS200 20 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF20, 20, 0, 0x0ADCE5, 0x001F },  // BoS200 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF20, 20, 0, 0x0B1D79, 0x0003 },  // BoS200 20 <- BoSM01 0, 1
		{ 0, 0x02BF20, 40, 0, 0x04402C, 0x03FF },  // BoS200 40 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF20, 40, 0, 0x0ADCE5, 0x001F },  // BoS200 40 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF20, 40, 0, 0x0B1D79, 0x0003 },  // BoS200 40 <- BoSM01 0, 1
		{ 0, 0x02BF20, 40, 0, 0x142EDC, 0x0001 },  // BoS200 40 <- BoSM00 0
		{ 0, 0x02BF20, 200, 0, 0x04402C, 0x03FF },  // BoS200 200 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF20, 200, 0, 0x0ADCE5, 0x001F },  // BoS200 200 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF20, 200, 0, 0x0B1D79, 0x0003 },  // BoS200 200 <- BoSM01 0, 1
		{ 0, 0x02BF20, 255, 0, 0x02BF21, 0x001E },  // BoS200 255 <- BoS201 1, 2, 3, 4
		{ 0, 0x02BF20, 255, 0, 0x04402C, 0x03FF },  // BoS200 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF20, 255, 0, 0x0537FC, 0x0001 },  // BoS200 255 <- BoS201B 0
		{ 0, 0x02BF20, 255, 0, 0x0537FF, 0x0006 },  // BoS200 255 <- BoS202 1, 2
		{ 0, 0x02BF20, 255, 0, 0x0ADCE5, 0x001F },  // BoS200 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF20, 255, 0, 0x0B1D79, 0x0002 },  // BoS200 255 <- BoSM01 1
		{ 0, 0x02BF20, 255, 0, 0x0B9F9D, 0x0007 },  // BoS200 255 <- BoS302 0, 1, 2
		{ 0, 0x02BF20, 255, 0, 0x0FDC8C, 0x000F },  // BoS200 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x02BF20, 255, 0, 0x107A1C, 0x0001 },  // BoS200 255 <- BoS302B 0
		{ 0, 0x02BF20, 255, 0, 0x142EDC, 0x0001 },  // BoS200 255 <- BoSM00 0
		{ 0, 0x02BF20, 255, 0, 0x173ED9, 0x0007 },  // BoS200 255 <- BoS304 0, 1, 2
		{ 0, 0x02BF20, 355, 0, 0x0229EE, 0x0004 },  // BoS200 355 <- MQ302 2
		{ 0, 0x02BF20, 355, 0, 0x043275, 0x0002 },  // BoS200 355 <- RR303 1
		{ 0, 0x02BF20, 355, 0, 0x06D646, 0x0154 },  // BoS200 355 <- DN084 2, 4, 6, 8
		{ 0, 0x02BF20, 355, 0, 0x142FD3, 0x42100002 },  // BoS200 355 <- RR304 1, 20, 25, 30
		{ 0, 0x02BF20, 355, 0, 0x174029, 0x01FF },  // BoS200 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x02BF21, 10, 0, 0x02BF21, 0x001E },  // BoS201 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x02BF21, 20, 0, 0x02BF21, 0x001E },  // BoS201 20 <- BoS201 1, 2, 3, 4
		{ 0, 0x02BF21, 20, 0, 0x04402C, 0x03FF },  // BoS201 20 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF21, 20, 0, 0x0ADCE5, 0x001F },  // BoS201 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF21, 20, 0, 0x0B1D79, 0x0003 },  // BoS201 20 <- BoSM01 0, 1
		{ 0, 0x02BF21, 30, 0, 0x02BF21, 0x001E },  // BoS201 30 <- BoS201 1, 2, 3, 4
		{ 0, 0x02BF21, 45, 0, 0x02BF21, 0x001C },  // BoS201 45 <- BoS201 2, 3, 4
		{ 0, 0x02BF21, 80, 0, 0x02BF21, 0x001C },  // BoS201 80 <- BoS201 2, 3, 4
		{ 0, 0x02BF21, 90, 0, 0x02BF21, 0x80000001C },  // BoS201 90 <- BoS201 2, 3, 4, 35
		{ 0, 0x02BF21, 100, 0, 0x02BF21, 0x0018 },  // BoS201 100 <- BoS201 3, 4
		{ 0, 0x02BF21, 110, 0, 0x02BF21, 0x0018 },  // BoS201 110 <- BoS201 3, 4
		{ 0, 0x02BF21, 115, 0, 0x02BF21, 0x0018 },  // BoS201 115 <- BoS201 3, 4
		{ 0, 0x02BF21, 120, 0, 0x02BF21, 0x0018 },  // BoS201 120 <- BoS201 3, 4
		{ 0, 0x02BF21, 130, 0, 0x02BF21, 0x0010 },  // BoS201 130 <- BoS201 4
		{ 0, 0x02BF21, 140, 0, 0x02BF21, 0x0010 },  // BoS201 140 <- BoS201 4
		{ 0, 0x02BF21, 150, 0, 0x02BF21, 0x0010 },  // BoS201 150 <- BoS201 4
		{ 0, 0x02BF21, 150, 0, 0x0537FC, 0x0001 },  // BoS201 150 <- BoS201B 0
		{ 0, 0x02BF21, 150, 0, 0x0537FF, 0x0006 },  // BoS201 150 <- BoS202 1, 2
		{ 0, 0x02BF21, 150, 0, 0x0B9F9D, 0x0007 },  // BoS201 150 <- BoS302 0, 1, 2
		{ 0, 0x02BF21, 150, 0, 0x0FDC8C, 0x000F },  // BoS201 150 <- BoS303 0, 1, 2, 3
		{ 0, 0x02BF21, 150, 0, 0x107A1C, 0x0001 },  // BoS201 150 <- BoS302B 0
		{ 0, 0x02BF21, 150, 0, 0x173ED9, 0x0007 },  // BoS201 150 <- BoS304 0, 1, 2
		{ 0, 0x02BF21, 170, 0, 0x04402C, 0x03FF },  // BoS201 170 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF21, 170, 0, 0x0ADCE5, 0x001F },  // BoS201 170 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF21, 170, 0, 0x0B1D79, 0x0002 },  // BoS201 170 <- BoSM01 1
		{ 0, 0x02BF21, 255, 0, 0x04402C, 0x03FF },  // BoS201 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x02BF21, 255, 0, 0x0537FC, 0x0001 },  // BoS201 255 <- BoS201B 0
		{ 0, 0x02BF21, 255, 0, 0x0537FF, 0x0006 },  // BoS201 255 <- BoS202 1, 2
		{ 0, 0x02BF21, 255, 0, 0x0ADCE5, 0x001F },  // BoS201 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x02BF21, 255, 0, 0x0B1D79, 0x0002 },  // BoS201 255 <- BoSM01 1
		{ 0, 0x02BF21, 255, 0, 0x0B9F9D, 0x0007 },  // BoS201 255 <- BoS302 0, 1, 2
		{ 0, 0x02BF21, 255, 0, 0x0FDC8C, 0x000F },  // BoS201 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x02BF21, 255, 0, 0x107A1C, 0x0001 },  // BoS201 255 <- BoS302B 0
		{ 0, 0x02BF21, 255, 0, 0x142EDC, 0x0001 },  // BoS201 255 <- BoSM00 0
		{ 0, 0x02BF21, 255, 0, 0x173ED9, 0x0007 },  // BoS201 255 <- BoS304 0, 1, 2
		{ 0, 0x02BF21, 355, 0, 0x0229EE, 0x0004 },  // BoS201 355 <- MQ302 2
		{ 0, 0x02BF21, 355, 0, 0x043275, 0x0002 },  // BoS201 355 <- RR303 1
		{ 0, 0x02BF21, 355, 0, 0x06D646, 0x0154 },  // BoS201 355 <- DN084 2, 4, 6, 8
		{ 0, 0x02BF21, 355, 0, 0x142FD3, 0x42100002 },  // BoS201 355 <- RR304 1, 20, 25, 30
		{ 0, 0x02BF21, 355, 0, 0x174029, 0x01FF },  // BoS201 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x02C8CB, 0, 0, 0x043275, 0x0006 },  // RR302 0 <- RR303 1, 2
		{ 0, 0x02C8CB, 0, 0, 0x0DFB3C, 0x0001 },  // RR302 0 <- MinDestBoS 0
		{ 0, 0x02C8CB, 0, 0, 0x142FD3, 0x4A100002 },  // RR302 0 <- RR304 1, 20, 25, 27, 30
		{ 0, 0x02C8CB, 10100, 0, 0x043275, 0x0002 },  // RR302 10100 <- RR303 1
		{ 0, 0x02C8CB, 10100, 0, 0x142FD3, 0x42100002 },  // RR302 10100 <- RR304 1, 20, 25, 30
		{ 0, 0x02C8CB, 10200, 0, 0x142FD3, 0x2100000 },  // RR302 10200 <- RR304 20, 25
		{ 0, 0x02C8CB, 10300, 0, 0x142FD3, 0x2100000 },  // RR302 10300 <- RR304 20, 25
		{ 0, 0x02C8CB, 10400, 0, 0x142FD3, 0x2100000 },  // RR302 10400 <- RR304 20, 25
		{ 0, 0x02C8CB, 10500, 0, 0x142FD3, 0x2100000 },  // RR302 10500 <- RR304 20, 25
		{ 0, 0x02C8CB, 10600, 0, 0x142FD3, 0x2100000 },  // RR302 10600 <- RR304 20, 25
		{ 0, 0x02C8CB, 10700, 0, 0x142FD3, 0x2100000 },  // RR302 10700 <- RR304 20, 25
		{ 0, 0x02C8CB, 10800, 0, 0x142FD3, 0x4A100000 },  // RR302 10800 <- RR304 20, 25, 27, 30
		{ 0, 0x02C8CB, 10900, 0, 0x142FD3, 0x2000000 },  // RR302 10900 <- RR304 25
		{ 0, 0x02C8CB, 20000, 0, 0x0DFB3C, 0x0001 },  // RR302 20000 <- MinDestBoS 0
		{ 0, 0x03A457, 0, 0, 0x0229EE, 0x0004 },  // Min02 0 <- MQ302 2
		{ 0, 0x03A457, 0, 0, 0x0DDFDB, 0x0003 },  // Min02 0 <- Min301 0, 1
		{ 0, 0x03A457, 0, 0, 0x0DFB3C, 0x0001 },  // Min02 0 <- MinDestBoS 0
		{ 0, 0x03A457, 10, 0, 0x0229EE, 0x0004 },  // Min02 10 <- MQ302 2
		{ 0, 0x03A457, 10, 0, 0x0DDFDB, 0x0003 },  // Min02 10 <- Min301 0, 1
		{ 0, 0x03A457, 10, 0, 0x0DFB3C, 0x0001 },  // Min02 10 <- MinDestBoS 0
		{ 0, 0x03A457, 20, 0, 0x0229EE, 0x0004 },  // Min02 20 <- MQ302 2
		{ 0, 0x03A457, 20, 0, 0x0DDFDB, 0x0003 },  // Min02 20 <- Min301 0, 1
		{ 0, 0x03A457, 20, 0, 0x0DFB3C, 0x0001 },  // Min02 20 <- MinDestBoS 0
		{ 0, 0x03A457, 50, 0, 0x0229EE, 0x0004 },  // Min02 50 <- MQ302 2
		{ 0, 0x03A457, 50, 0, 0x0DDFDB, 0x0003 },  // Min02 50 <- Min301 0, 1
		{ 0, 0x03A457, 50, 0, 0x0DFB3C, 0x0001 },  // Min02 50 <- MinDestBoS 0
		{ 0, 0x03A457, 500, 0, 0x0229EE, 0x0004 },  // Min02 500 <- MQ302 2
		{ 0, 0x03A457, 500, 0, 0x0DDFDB, 0x0003 },  // Min02 500 <- Min301 0, 1
		{ 0, 0x03A457, 500, 0, 0x0DFB3C, 0x0001 },  // Min02 500 <- MinDestBoS 0
		{ 0, 0x03A457, 520, 0, 0x0229EE, 0x0004 },  // Min02 520 <- MQ302 2
		{ 0, 0x03A457, 520, 0, 0x0DDFDB, 0x0003 },  // Min02 520 <- Min301 0, 1
		{ 0, 0x03A457, 520, 0, 0x0DFB3C, 0x0001 },  // Min02 520 <- MinDestBoS 0
		{ 0, 0x03A457, 570, 0, 0x0229EE, 0x0004 },  // Min02 570 <- MQ302 2
		{ 0, 0x03A457, 570, 0, 0x0DDFDB, 0x0003 },  // Min02 570 <- Min301 0, 1
		{ 0, 0x03A457, 570, 0, 0x0DFB3C, 0x0001 },  // Min02 570 <- MinDestBoS 0
		{ 0, 0x03A457, 600, 0, 0x0229EE, 0x0004 },  // Min02 600 <- MQ302 2
		{ 0, 0x03A457, 600, 0, 0x0DDFDB, 0x0003 },  // Min02 600 <- Min301 0, 1
		{ 0, 0x03A457, 600, 0, 0x0DFB3C, 0x0001 },  // Min02 600 <- MinDestBoS 0
		{ 0, 0x03A457, 1000, 0, 0x0229EE, 0x0004 },  // Min02 1000 <- MQ302 2
		{ 0, 0x03A457, 1000, 0, 0x0DDFDB, 0x0003 },  // Min02 1000 <- Min301 0, 1
		{ 0, 0x03A457, 1000, 0, 0x0DFB3C, 0x0001 },  // Min02 1000 <- MinDestBoS 0
		{ 0, 0x0423D5, 0, 0, 0x0537FF, 0x0006 },  // DN046 0 <- BoS202 1, 2
		{ 0, 0x0423D5, 50, 0, 0x0537FF, 0x0006 },  // DN046 50 <- BoS202 1, 2
		{ 0, 0x043275, 0, 0, 0x043275, 0x001E },  // RR303 0 <- RR303 1, 2, 3, 4
		{ 0, 0x043275, 0, 0, 0x0DFB3C, 0x0001 },  // RR303 0 <- MinDestBoS 0
		{ 0, 0x043275, 0, 0, 0x142FD3, 0x4A100002 },  // RR303 0 <- RR304 1, 20, 25, 27, 30
		{ 0, 0x043275, 50, 0, 0x043275, 0x0002 },  // RR303 50 <- RR303 1
		{ 0, 0x043275, 50, 0, 0x142FD3, 0x42100002 },  // RR303 50 <- RR304 1, 20, 25, 30
		{ 0, 0x043275, 400, 0, 0x142FD3, 0x2000000 },  // RR303 400 <- RR304 25
		{ 0, 0x043275, 725, 0, 0x142FD3, 0x40000000 },  // RR303 725 <- RR304 30
		{ 0, 0x043275, 750, 0, 0x142FD3, 0x40000000 },  // RR303 750 <- RR304 30
		{ 0, 0x043275, 800, 0, 0x142FD3, 0x40000000 },  // RR303 800 <- RR304 30
		{ 0, 0x043275, 803, 0, 0x142FD3, 0x40000000 },  // RR303 803 <- RR304 30
		{ 0, 0x043275, 1100, 0, 0x043275, 0x0004 },  // RR303 1100 <- RR303 2
		{ 0, 0x043275, 10100, 0, 0x043275, 0x0002 },  // RR303 10100 <- RR303 1
		{ 0, 0x043275, 10100, 0, 0x142FD3, 0x42100002 },  // RR303 10100 <- RR304 1, 20, 25, 30
		{ 0, 0x043275, 10200, 0, 0x142FD3, 0x2100000 },  // RR303 10200 <- RR304 20, 25
		{ 0, 0x043275, 10300, 0, 0x142FD3, 0x2100000 },  // RR303 10300 <- RR304 20, 25
		{ 0, 0x043275, 10400, 0, 0x142FD3, 0x2100000 },  // RR303 10400 <- RR304 20, 25
		{ 0, 0x043275, 10500, 0, 0x142FD3, 0x2100000 },  // RR303 10500 <- RR304 20, 25
		{ 0, 0x043275, 10600, 0, 0x142FD3, 0x2100000 },  // RR303 10600 <- RR304 20, 25
		{ 0, 0x043275, 11100, 0, 0x142FD3, 0x8000000 },  // RR303 11100 <- RR304 27
		{ 0, 0x043275, 11200, 0, 0x142FD3, 0x40000000 },  // RR303 11200 <- RR304 30
		{ 0, 0x043275, 20000, 0, 0x0DFB3C, 0x0001 },  // RR303 20000 <- MinDestBoS 0
		{ 0, 0x04402C, 10, 0, 0x043275, 0x0002 },  // BoSM02 10 <- RR303 1
		{ 0, 0x04402C, 10, 0, 0x04402C, 0x03FF },  // BoSM02 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 10, 0, 0x06D646, 0x0154 },  // BoSM02 10 <- DN084 2, 4, 6, 8
		{ 0, 0x04402C, 10, 0, 0x0ADCE5, 0x001F },  // BoSM02 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x04402C, 10, 0, 0x142FD3, 0x42100002 },  // BoSM02 10 <- RR304 1, 20, 25, 30
		{ 0, 0x04402C, 10, 0, 0x174029, 0x01FF },  // BoSM02 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x04402C, 11, 0, 0x043275, 0x0002 },  // BoSM02 11 <- RR303 1
		{ 0, 0x04402C, 11, 0, 0x06D646, 0x0154 },  // BoSM02 11 <- DN084 2, 4, 6, 8
		{ 0, 0x04402C, 11, 0, 0x142FD3, 0x42100002 },  // BoSM02 11 <- RR304 1, 20, 25, 30
		{ 0, 0x04402C, 11, 0, 0x174029, 0x01FF },  // BoSM02 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x04402C, 12, 0, 0x0ADCE5, 0x001F },  // BoSM02 12 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x04402C, 30, 0, 0x04402C, 0x03FC },  // BoSM02 30 <- BoSM02 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 30, 0, 0x0ADCE5, 0x001F },  // BoSM02 30 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x04402C, 50, 0, 0x04402C, 0x03F8 },  // BoSM02 50 <- BoSM02 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 59, 0, 0x04402C, 0x03F0 },  // BoSM02 59 <- BoSM02 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 69, 0, 0x04402C, 0x03F0 },  // BoSM02 69 <- BoSM02 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 70, 0, 0x04402C, 0x03F0 },  // BoSM02 70 <- BoSM02 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 75, 0, 0x04402C, 0x03F0 },  // BoSM02 75 <- BoSM02 4, 5, 6, 7, 8, 9
		{ 0, 0x04402C, 90, 0, 0x04402C, 0x03E0 },  // BoSM02 90 <- BoSM02 5, 6, 7, 8, 9
		{ 0, 0x04402C, 150, 0, 0x04402C, 0x03C0 },  // BoSM02 150 <- BoSM02 6, 7, 8, 9
		{ 0, 0x04402C, 155, 0, 0x04402C, 0x0380 },  // BoSM02 155 <- BoSM02 7, 8, 9
		{ 0, 0x04402C, 159, 0, 0x04402C, 0x0380 },  // BoSM02 159 <- BoSM02 7, 8, 9
		{ 0, 0x04402C, 160, 0, 0x04402C, 0x0380 },  // BoSM02 160 <- BoSM02 7, 8, 9
		{ 0, 0x04402C, 199, 0, 0x04402C, 0x0380 },  // BoSM02 199 <- BoSM02 7, 8, 9
		{ 0, 0x04402C, 200, 0, 0x04402C, 0x0300 },  // BoSM02 200 <- BoSM02 8, 9
		{ 0, 0x04402C, 210, 0, 0x04402C, 0x0300 },  // BoSM02 210 <- BoSM02 8, 9
		{ 0, 0x04402C, 220, 0, 0x04402C, 0x0300 },  // BoSM02 220 <- BoSM02 8, 9
		{ 0, 0x04402C, 350, 0, 0x04402C, 0x0200 },  // BoSM02 350 <- BoSM02 9
		{ 0, 0x04402C, 351, 0, 0x04402C, 0x0200 },  // BoSM02 351 <- BoSM02 9
		{ 0, 0x04402C, 430, 0, 0x0ADCE5, 0x001F },  // BoSM02 430 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x04402C, 499, 0, 0x043275, 0x0002 },  // BoSM02 499 <- RR303 1
		{ 0, 0x04402C, 499, 0, 0x06D646, 0x0154 },  // BoSM02 499 <- DN084 2, 4, 6, 8
		{ 0, 0x04402C, 499, 0, 0x142FD3, 0x42100002 },  // BoSM02 499 <- RR304 1, 20, 25, 30
		{ 0, 0x04402C, 499, 0, 0x174029, 0x01FF },  // BoSM02 499 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x04402C, 500, 0, 0x043275, 0x0002 },  // BoSM02 500 <- RR303 1
		{ 0, 0x04402C, 500, 0, 0x06D646, 0x0154 },  // BoSM02 500 <- DN084 2, 4, 6, 8
		{ 0, 0x04402C, 500, 0, 0x142FD3, 0x42100002 },  // BoSM02 500 <- RR304 1, 20, 25, 30
		{ 0, 0x04402C, 500, 0, 0x174029, 0x01FF },  // BoSM02 500 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0456E8, 17, 0, 0x0229E9, 0x0001 },  // FFDiamondCity07 17 <- MQ202 0
		{ 0, 0x0456E8, 17, 0, 0x0229EA, 0x0001 },  // FFDiamondCity07 17 <- MQ203 0
		{ 0, 0x0459D2, 50, 0, 0x0459D2, 0x0040 },  // RR101 50 <- RR101 6
		{ 0, 0x0459D2, 600, 0, 0x043275, 0x0002 },  // RR101 600 <- RR303 1
		{ 0, 0x0459D2, 600, 0, 0x142FD3, 0x42100002 },  // RR101 600 <- RR304 1, 20, 25, 30
		{ 0, 0x0459D2, 615, 0, 0x043275, 0x0002 },  // RR101 615 <- RR303 1
		{ 0, 0x0459D2, 615, 0, 0x142FD3, 0x42100002 },  // RR101 615 <- RR304 1, 20, 25, 30
		{ 0, 0x0459D2, 1100, 0, 0x043275, 0x0002 },  // RR101 1100 <- RR303 1
		{ 0, 0x0459D2, 1100, 0, 0x07CC61, 0x0400 },  // RR101 1100 <- RR301 10
		{ 0, 0x0459D2, 1100, 0, 0x142FD3, 0x42100002 },  // RR101 1100 <- RR304 1, 20, 25, 30
		{ 0, 0x0459D2, 1200, 0, 0x043275, 0x0002 },  // RR101 1200 <- RR303 1
		{ 0, 0x0459D2, 1200, 0, 0x0459D2, 0x0020 },  // RR101 1200 <- RR101 5
		{ 0, 0x0459D2, 1200, 0, 0x07CC61, 0x0400 },  // RR101 1200 <- RR301 10
		{ 0, 0x0459D2, 1200, 0, 0x142FD3, 0x42100002 },  // RR101 1200 <- RR304 1, 20, 25, 30
		{ 0, 0x0498B5, 5, 0, 0x0498B5, 0x0001 },  // InstM01_OLD 5 <- InstM01_OLD 0
		{ 0, 0x04AF6C, 10, 0, 0x02BF21, 0x001E },  // BoS201Radio 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x0537FC, 10, 0, 0x04402C, 0x03FF },  // BoS201B 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0537FC, 10, 0, 0x0537FC, 0x0001 },  // BoS201B 10 <- BoS201B 0
		{ 0, 0x0537FC, 10, 0, 0x0537FF, 0x0006 },  // BoS201B 10 <- BoS202 1, 2
		{ 0, 0x0537FC, 10, 0, 0x0ADCE5, 0x001F },  // BoS201B 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0537FC, 10, 0, 0x0B1D79, 0x0002 },  // BoS201B 10 <- BoSM01 1
		{ 0, 0x0537FC, 10, 0, 0x0B9F9D, 0x0007 },  // BoS201B 10 <- BoS302 0, 1, 2
		{ 0, 0x0537FC, 10, 0, 0x0FDC8C, 0x000F },  // BoS201B 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FC, 10, 0, 0x107A1C, 0x0001 },  // BoS201B 10 <- BoS302B 0
		{ 0, 0x0537FC, 10, 0, 0x142EDC, 0x0001 },  // BoS201B 10 <- BoSM00 0
		{ 0, 0x0537FC, 10, 0, 0x173ED9, 0x0007 },  // BoS201B 10 <- BoS304 0, 1, 2
		{ 0, 0x0537FC, 255, 0, 0x0537FF, 0x0006 },  // BoS201B 255 <- BoS202 1, 2
		{ 0, 0x0537FC, 255, 0, 0x0B9F9D, 0x0007 },  // BoS201B 255 <- BoS302 0, 1, 2
		{ 0, 0x0537FC, 255, 0, 0x0FDC8C, 0x000F },  // BoS201B 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FC, 255, 0, 0x107A1C, 0x0001 },  // BoS201B 255 <- BoS302B 0
		{ 0, 0x0537FC, 255, 0, 0x173ED9, 0x0007 },  // BoS201B 255 <- BoS304 0, 1, 2
		{ 0, 0x0537FC, 355, 0, 0x0229EE, 0x0004 },  // BoS201B 355 <- MQ302 2
		{ 0, 0x0537FC, 355, 0, 0x043275, 0x0002 },  // BoS201B 355 <- RR303 1
		{ 0, 0x0537FC, 355, 0, 0x06D646, 0x0154 },  // BoS201B 355 <- DN084 2, 4, 6, 8
		{ 0, 0x0537FC, 355, 0, 0x142FD3, 0x42100002 },  // BoS201B 355 <- RR304 1, 20, 25, 30
		{ 0, 0x0537FC, 355, 0, 0x174029, 0x01FF },  // BoS201B 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0537FF, 5, 0, 0x0229EE, 0x0004 },  // BoS202 5 <- MQ302 2
		{ 0, 0x0537FF, 5, 0, 0x043275, 0x0002 },  // BoS202 5 <- RR303 1
		{ 0, 0x0537FF, 5, 0, 0x04402C, 0x03FF },  // BoS202 5 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0537FF, 5, 0, 0x0537FF, 0x0006 },  // BoS202 5 <- BoS202 1, 2
		{ 0, 0x0537FF, 5, 0, 0x06D646, 0x0154 },  // BoS202 5 <- DN084 2, 4, 6, 8
		{ 0, 0x0537FF, 5, 0, 0x0ADCE5, 0x001F },  // BoS202 5 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0537FF, 5, 0, 0x0B1D79, 0x0002 },  // BoS202 5 <- BoSM01 1
		{ 0, 0x0537FF, 5, 0, 0x0B9F9D, 0x0007 },  // BoS202 5 <- BoS302 0, 1, 2
		{ 0, 0x0537FF, 5, 0, 0x0FDC8C, 0x000F },  // BoS202 5 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FF, 5, 0, 0x107A1C, 0x0001 },  // BoS202 5 <- BoS302B 0
		{ 0, 0x0537FF, 5, 0, 0x142EDC, 0x0001 },  // BoS202 5 <- BoSM00 0
		{ 0, 0x0537FF, 5, 0, 0x142FD3, 0x42100002 },  // BoS202 5 <- RR304 1, 20, 25, 30
		{ 0, 0x0537FF, 5, 0, 0x173ED9, 0x0007 },  // BoS202 5 <- BoS304 0, 1, 2
		{ 0, 0x0537FF, 5, 0, 0x174029, 0x01FF },  // BoS202 5 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0537FF, 10, 0, 0x0537FF, 0x0006 },  // BoS202 10 <- BoS202 1, 2
		{ 0, 0x0537FF, 10, 0, 0x0B9F9D, 0x0007 },  // BoS202 10 <- BoS302 0, 1, 2
		{ 0, 0x0537FF, 10, 0, 0x0FDC8C, 0x000F },  // BoS202 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FF, 10, 0, 0x107A1C, 0x0001 },  // BoS202 10 <- BoS302B 0
		{ 0, 0x0537FF, 10, 0, 0x173ED9, 0x0007 },  // BoS202 10 <- BoS304 0, 1, 2
		{ 0, 0x0537FF, 20, 0, 0x0537FF, 0x0006 },  // BoS202 20 <- BoS202 1, 2
		{ 0, 0x0537FF, 30, 0, 0x0537FF, 0x0002 },  // BoS202 30 <- BoS202 1
		{ 0, 0x0537FF, 40, 0, 0x0537FF, 0x0002 },  // BoS202 40 <- BoS202 1
		{ 0, 0x0537FF, 60, 0, 0x0537FF, 0x0002 },  // BoS202 60 <- BoS202 1
		{ 0, 0x0537FF, 70, 0, 0x0537FF, 0x0002 },  // BoS202 70 <- BoS202 1
		{ 0, 0x0537FF, 75, 0, 0x0537FF, 0x0002 },  // BoS202 75 <- BoS202 1
		{ 0, 0x0537FF, 100, 0, 0x0537FF, 0x0002 },  // BoS202 100 <- BoS202 1
		{ 0, 0x0537FF, 255, 0, 0x04402C, 0x03FF },  // BoS202 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0537FF, 255, 0, 0x0ADCE5, 0x001F },  // BoS202 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0537FF, 255, 0, 0x0B1D79, 0x0002 },  // BoS202 255 <- BoSM01 1
		{ 0, 0x0537FF, 255, 0, 0x0B9F9D, 0x0007 },  // BoS202 255 <- BoS302 0, 1, 2
		{ 0, 0x0537FF, 255, 0, 0x0FDC8C, 0x000F },  // BoS202 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FF, 255, 0, 0x107A1C, 0x0001 },  // BoS202 255 <- BoS302B 0
		{ 0, 0x0537FF, 255, 0, 0x142EDC, 0x0001 },  // BoS202 255 <- BoSM00 0
		{ 0, 0x0537FF, 255, 0, 0x173ED9, 0x0007 },  // BoS202 255 <- BoS304 0, 1, 2
		{ 0, 0x0537FF, 300, 0, 0x0B9F9D, 0x0007 },  // BoS202 300 <- BoS302 0, 1, 2
		{ 0, 0x0537FF, 300, 0, 0x0FDC8C, 0x000F },  // BoS202 300 <- BoS303 0, 1, 2, 3
		{ 0, 0x0537FF, 300, 0, 0x107A1C, 0x0001 },  // BoS202 300 <- BoS302B 0
		{ 0, 0x0537FF, 300, 0, 0x173ED9, 0x0007 },  // BoS202 300 <- BoS304 0, 1, 2
		{ 0, 0x0537FF, 355, 0, 0x0229EE, 0x0004 },  // BoS202 355 <- MQ302 2
		{ 0, 0x0537FF, 355, 0, 0x043275, 0x0002 },  // BoS202 355 <- RR303 1
		{ 0, 0x0537FF, 355, 0, 0x06D646, 0x0154 },  // BoS202 355 <- DN084 2, 4, 6, 8
		{ 0, 0x0537FF, 355, 0, 0x142FD3, 0x42100002 },  // BoS202 355 <- RR304 1, 20, 25, 30
		{ 0, 0x0537FF, 355, 0, 0x174029, 0x01FF },  // BoS202 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x055BE4, 10, 0, 0x055BE4, 0x0003 },  // MinVsInst 10 <- MinVsInst 0, 1
		{ 0, 0x055BE4, 1500, 0, 0x0229EE, 0x0004 },  // MinVsInst 1500 <- MQ302 2
		{ 0, 0x055BE4, 1500, 0, 0x06D646, 0x02AA },  // MinVsInst 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x055BE4, 1500, 0, 0x0DDFDB, 0x0003 },  // MinVsInst 1500 <- Min301 0, 1
		{ 0, 0x055BE4, 1500, 0, 0x0DFB3C, 0x0001 },  // MinVsInst 1500 <- MinDestBoS 0
		{ 0, 0x055BE4, 1500, 0, 0x0FDC8C, 0x0006 },  // MinVsInst 1500 <- BoS303 1, 2
		{ 0, 0x05BC93, 700, 0, 0x0229EE, 0x000F },  // DialogueRailroad 700 <- MQ302 0, 1, 2, 3
		{ 0, 0x05BC93, 700, 0, 0x0B1752, 0x0001 },  // DialogueRailroad 700 <- MQ206 0
		{ 0, 0x05BC93, 700, 0, 0x0DDFDB, 0x0003 },  // DialogueRailroad 700 <- Min301 0, 1
		{ 0, 0x05BC93, 700, 0, 0x0DFB3C, 0x0001 },  // DialogueRailroad 700 <- MinDestBoS 0
		{ 0, 0x05DDAB, 40, 0, 0x02BF20, 0x0001 },  // BoS100 40 <- BoS200 0
		{ 0, 0x05DDAB, 40, 0, 0x02BF21, 0x001E },  // BoS100 40 <- BoS201 1, 2, 3, 4
		{ 0, 0x05DDAB, 40, 0, 0x04402C, 0x03FF },  // BoS100 40 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x05DDAB, 40, 0, 0x0537FC, 0x0001 },  // BoS100 40 <- BoS201B 0
		{ 0, 0x05DDAB, 40, 0, 0x0537FF, 0x0006 },  // BoS100 40 <- BoS202 1, 2
		{ 0, 0x05DDAB, 40, 0, 0x05DDAB, 0x0001 },  // BoS100 40 <- BoS100 0
		{ 0, 0x05DDAB, 40, 0, 0x06F5C1, 0x0007 },  // BoS100 40 <- BoS101 0, 1, 2
		{ 0, 0x05DDAB, 40, 0, 0x0ADCE5, 0x001F },  // BoS100 40 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x05DDAB, 40, 0, 0x0B1D79, 0x0003 },  // BoS100 40 <- BoSM01 0, 1
		{ 0, 0x05DDAB, 40, 0, 0x0B9F9D, 0x0007 },  // BoS100 40 <- BoS302 0, 1, 2
		{ 0, 0x05DDAB, 40, 0, 0x0FDC8C, 0x000F },  // BoS100 40 <- BoS303 0, 1, 2, 3
		{ 0, 0x05DDAB, 40, 0, 0x107A1C, 0x0001 },  // BoS100 40 <- BoS302B 0
		{ 0, 0x05DDAB, 40, 0, 0x142EDC, 0x0001 },  // BoS100 40 <- BoSM00 0
		{ 0, 0x05DDAB, 40, 0, 0x173ED9, 0x0007 },  // BoS100 40 <- BoS304 0, 1, 2
		{ 0, 0x05DDAB, 255, 0, 0x02BF20, 0x0001 },  // BoS100 255 <- BoS200 0
		{ 0, 0x05DDAB, 255, 0, 0x02BF21, 0x001E },  // BoS100 255 <- BoS201 1, 2, 3, 4
		{ 0, 0x05DDAB, 255, 0, 0x04402C, 0x03FF },  // BoS100 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x05DDAB, 255, 0, 0x0537FC, 0x0001 },  // BoS100 255 <- BoS201B 0
		{ 0, 0x05DDAB, 255, 0, 0x0537FF, 0x0006 },  // BoS100 255 <- BoS202 1, 2
		{ 0, 0x05DDAB, 255, 0, 0x06F5C1, 0x0007 },  // BoS100 255 <- BoS101 0, 1, 2
		{ 0, 0x05DDAB, 255, 0, 0x0ADCE5, 0x001F },  // BoS100 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x05DDAB, 255, 0, 0x0B1D79, 0x0003 },  // BoS100 255 <- BoSM01 0, 1
		{ 0, 0x05DDAB, 255, 0, 0x0B9F9D, 0x0007 },  // BoS100 255 <- BoS302 0, 1, 2
		{ 0, 0x05DDAB, 255, 0, 0x0FDC8C, 0x000F },  // BoS100 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x05DDAB, 255, 0, 0x107A1C, 0x0001 },  // BoS100 255 <- BoS302B 0
		{ 0, 0x05DDAB, 255, 0, 0x142EDC, 0x0001 },  // BoS100 255 <- BoSM00 0
		{ 0, 0x05DDAB, 255, 0, 0x173ED9, 0x0007 },  // BoS100 255 <- BoS304 0, 1, 2
		{ 0, 0x05DDAB, 355, 0, 0x0229EE, 0x0004 },  // BoS100 355 <- MQ302 2
		{ 0, 0x05DDAB, 355, 0, 0x043275, 0x0002 },  // BoS100 355 <- RR303 1
		{ 0, 0x05DDAB, 355, 0, 0x06D646, 0x0154 },  // BoS100 355 <- DN084 2, 4, 6, 8
		{ 0, 0x05DDAB, 355, 0, 0x142FD3, 0x42100002 },  // BoS100 355 <- RR304 1, 20, 25, 30
		{ 0, 0x05DDAB, 355, 0, 0x174029, 0x01FF },  // BoS100 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x05DEE4, 0, 0, 0x0229EE, 0x0004 },  // Min01 0 <- MQ302 2
		{ 0, 0x05DEE4, 0, 0, 0x0DDFDB, 0x0003 },  // Min01 0 <- Min301 0, 1
		{ 0, 0x05DEE4, 0, 0, 0x0DFB3C, 0x0001 },  // Min01 0 <- MinDestBoS 0
		{ 0, 0x05DEE4, 10, 0, 0x0229EE, 0x0004 },  // Min01 10 <- MQ302 2
		{ 0, 0x05DEE4, 10, 0, 0x0DDFDB, 0x0003 },  // Min01 10 <- Min301 0, 1
		{ 0, 0x05DEE4, 10, 0, 0x0DFB3C, 0x0001 },  // Min01 10 <- MinDestBoS 0
		{ 0, 0x05DEE4, 50, 0, 0x0229EE, 0x0004 },  // Min01 50 <- MQ302 2
		{ 0, 0x05DEE4, 50, 0, 0x0DDFDB, 0x0003 },  // Min01 50 <- Min301 0, 1
		{ 0, 0x05DEE4, 50, 0, 0x0DFB3C, 0x0001 },  // Min01 50 <- MinDestBoS 0
		{ 0, 0x05DEE4, 100, 0, 0x0229EE, 0x0004 },  // Min01 100 <- MQ302 2
		{ 0, 0x05DEE4, 100, 0, 0x0DDFDB, 0x0003 },  // Min01 100 <- Min301 0, 1
		{ 0, 0x05DEE4, 100, 0, 0x0DFB3C, 0x0001 },  // Min01 100 <- MinDestBoS 0
		{ 0, 0x05DEE4, 110, 0, 0x0229EE, 0x0004 },  // Min01 110 <- MQ302 2
		{ 0, 0x05DEE4, 110, 0, 0x0DDFDB, 0x0003 },  // Min01 110 <- Min301 0, 1
		{ 0, 0x05DEE4, 110, 0, 0x0DFB3C, 0x0001 },  // Min01 110 <- MinDestBoS 0
		{ 0, 0x05DEE4, 200, 0, 0x0229EE, 0x0004 },  // Min01 200 <- MQ302 2
		{ 0, 0x05DEE4, 200, 0, 0x0DDFDB, 0x0003 },  // Min01 200 <- Min301 0, 1
		{ 0, 0x05DEE4, 200, 0, 0x0DFB3C, 0x0001 },  // Min01 200 <- MinDestBoS 0
		{ 0, 0x05DEE4, 250, 0, 0x0229EE, 0x0004 },  // Min01 250 <- MQ302 2
		{ 0, 0x05DEE4, 250, 0, 0x0DDFDB, 0x0003 },  // Min01 250 <- Min301 0, 1
		{ 0, 0x05DEE4, 250, 0, 0x0DFB3C, 0x0001 },  // Min01 250 <- MinDestBoS 0
		{ 0, 0x05DEE4, 1000, 0, 0x0229EE, 0x0004 },  // Min01 1000 <- MQ302 2
		{ 0, 0x05DEE4, 1000, 0, 0x0DDFDB, 0x0003 },  // Min01 1000 <- Min301 0, 1
		{ 0, 0x05DEE4, 1000, 0, 0x0DFB3C, 0x0001 },  // Min01 1000 <- MinDestBoS 0
		{ 0, 0x05DEE4, 2000, 0, 0x0229EE, 0x0004 },  // Min01 2000 <- MQ302 2
		{ 0, 0x05DEE4, 2000, 0, 0x0DDFDB, 0x0003 },  // Min01 2000 <- Min301 0, 1
		{ 0, 0x05DEE4, 2000, 0, 0x0DFB3C, 0x0001 },  // Min01 2000 <- MinDestBoS 0
		{ 0, 0x05DEE4, 2100, 0, 0x0229EE, 0x0004 },  // Min01 2100 <- MQ302 2
		{ 0, 0x05DEE4, 2100, 0, 0x0DDFDB, 0x0003 },  // Min01 2100 <- Min301 0, 1
		{ 0, 0x05DEE4, 2100, 0, 0x0DFB3C, 0x0001 },  // Min01 2100 <- MinDestBoS 0
		{ 0, 0x05E63E, 20, 0, 0x06D646, 0x0154 },  // DialogueInstitute 20 <- DN084 2, 4, 6, 8
		{ 0, 0x05FD90, 0, 0, 0x0229EE, 0x0008 },  // RRM01 0 <- MQ302 3
		{ 0, 0x05FD90, 0, 0, 0x043275, 0x0002 },  // RRM01 0 <- RR303 1
		{ 0, 0x05FD90, 0, 0, 0x05BC93, 0x0002 },  // RRM01 0 <- DialogueRailroad 1
		{ 0, 0x05FD90, 0, 0, 0x07CC61, 0x0400 },  // RRM01 0 <- RR301 10
		{ 0, 0x05FD90, 0, 0, 0x0DFB3C, 0x0001 },  // RRM01 0 <- MinDestBoS 0
		{ 0, 0x05FD90, 0, 0, 0x142FD3, 0x42100002 },  // RRM01 0 <- RR304 1, 20, 25, 30
		{ 0, 0x05FD90, 50, 0, 0x0229EE, 0x0008 },  // RRM01 50 <- MQ302 3
		{ 0, 0x05FD90, 50, 0, 0x043275, 0x0002 },  // RRM01 50 <- RR303 1
		{ 0, 0x05FD90, 50, 0, 0x07CC61, 0x0400 },  // RRM01 50 <- RR301 10
		{ 0, 0x05FD90, 50, 0, 0x142FD3, 0x42100002 },  // RRM01 50 <- RR304 1, 20, 25, 30
		{ 0, 0x05FD90, 800, 0, 0x05BC93, 0x0002 },  // RRM01 800 <- DialogueRailroad 1
		{ 0, 0x05FD90, 9000, 0, 0x0DFB3C, 0x0001 },  // RRM01 9000 <- MinDestBoS 0
		{ 0, 0x068D7B, 1, 0, 0x02BF20, 0x0001 },  // BoS100Fight 1 <- BoS200 0
		{ 0, 0x068D7B, 1, 0, 0x02BF21, 0x001E },  // BoS100Fight 1 <- BoS201 1, 2, 3, 4
		{ 0, 0x068D7B, 1, 0, 0x04402C, 0x03FF },  // BoS100Fight 1 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 1, 0, 0x0537FC, 0x0001 },  // BoS100Fight 1 <- BoS201B 0
		{ 0, 0x068D7B, 1, 0, 0x0537FF, 0x0006 },  // BoS100Fight 1 <- BoS202 1, 2
		{ 0, 0x068D7B, 1, 0, 0x06F5C1, 0x0007 },  // BoS100Fight 1 <- BoS101 0, 1, 2
		{ 0, 0x068D7B, 1, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 1 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 1, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 1 <- BoSM01 0, 1
		{ 0, 0x068D7B, 1, 0, 0x0B9F9D, 0x0007 },  // BoS100Fight 1 <- BoS302 0, 1, 2
		{ 0, 0x068D7B, 1, 0, 0x0FDC8C, 0x000F },  // BoS100Fight 1 <- BoS303 0, 1, 2, 3
		{ 0, 0x068D7B, 1, 0, 0x107A1C, 0x0001 },  // BoS100Fight 1 <- BoS302B 0
		{ 0, 0x068D7B, 1, 0, 0x142EDC, 0x0001 },  // BoS100Fight 1 <- BoSM00 0
		{ 0, 0x068D7B, 1, 0, 0x173ED9, 0x0007 },  // BoS100Fight 1 <- BoS304 0, 1, 2
		{ 0, 0x068D7B, 2, 0, 0x04402C, 0x03FF },  // BoS100Fight 2 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 2, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 2 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 2, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 2 <- BoSM01 0, 1
		{ 0, 0x068D7B, 2, 0, 0x142EDC, 0x0001 },  // BoS100Fight 2 <- BoSM00 0
		{ 0, 0x068D7B, 10, 0, 0x05DDAB, 0x0001 },  // BoS100Fight 10 <- BoS100 0
		{ 0, 0x068D7B, 20, 0, 0x02BF20, 0x0001 },  // BoS100Fight 20 <- BoS200 0
		{ 0, 0x068D7B, 20, 0, 0x02BF21, 0x001E },  // BoS100Fight 20 <- BoS201 1, 2, 3, 4
		{ 0, 0x068D7B, 20, 0, 0x04402C, 0x03FF },  // BoS100Fight 20 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 20, 0, 0x0537FC, 0x0001 },  // BoS100Fight 20 <- BoS201B 0
		{ 0, 0x068D7B, 20, 0, 0x0537FF, 0x0006 },  // BoS100Fight 20 <- BoS202 1, 2
		{ 0, 0x068D7B, 20, 0, 0x06F5C1, 0x0007 },  // BoS100Fight 20 <- BoS101 0, 1, 2
		{ 0, 0x068D7B, 20, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 20, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 20 <- BoSM01 0, 1
		{ 0, 0x068D7B, 20, 0, 0x0B9F9D, 0x0007 },  // BoS100Fight 20 <- BoS302 0, 1, 2
		{ 0, 0x068D7B, 20, 0, 0x0FDC8C, 0x000F },  // BoS100Fight 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x068D7B, 20, 0, 0x107A1C, 0x0001 },  // BoS100Fight 20 <- BoS302B 0
		{ 0, 0x068D7B, 20, 0, 0x142EDC, 0x0001 },  // BoS100Fight 20 <- BoSM00 0
		{ 0, 0x068D7B, 20, 0, 0x173ED9, 0x0007 },  // BoS100Fight 20 <- BoS304 0, 1, 2
		{ 0, 0x068D7B, 90, 0, 0x02BF20, 0x0001 },  // BoS100Fight 90 <- BoS200 0
		{ 0, 0x068D7B, 90, 0, 0x02BF21, 0x001E },  // BoS100Fight 90 <- BoS201 1, 2, 3, 4
		{ 0, 0x068D7B, 90, 0, 0x04402C, 0x03FF },  // BoS100Fight 90 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 90, 0, 0x0537FC, 0x0001 },  // BoS100Fight 90 <- BoS201B 0
		{ 0, 0x068D7B, 90, 0, 0x0537FF, 0x0006 },  // BoS100Fight 90 <- BoS202 1, 2
		{ 0, 0x068D7B, 90, 0, 0x06F5C1, 0x0007 },  // BoS100Fight 90 <- BoS101 0, 1, 2
		{ 0, 0x068D7B, 90, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 90 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 90, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 90 <- BoSM01 0, 1
		{ 0, 0x068D7B, 90, 0, 0x0B9F9D, 0x0007 },  // BoS100Fight 90 <- BoS302 0, 1, 2
		{ 0, 0x068D7B, 90, 0, 0x0FDC8C, 0x000F },  // BoS100Fight 90 <- BoS303 0, 1, 2, 3
		{ 0, 0x068D7B, 90, 0, 0x107A1C, 0x0001 },  // BoS100Fight 90 <- BoS302B 0
		{ 0, 0x068D7B, 90, 0, 0x142EDC, 0x0001 },  // BoS100Fight 90 <- BoSM00 0
		{ 0, 0x068D7B, 90, 0, 0x173ED9, 0x0007 },  // BoS100Fight 90 <- BoS304 0, 1, 2
		{ 0, 0x068D7B, 99, 0, 0x02BF20, 0x0001 },  // BoS100Fight 99 <- BoS200 0
		{ 0, 0x068D7B, 99, 0, 0x02BF21, 0x001E },  // BoS100Fight 99 <- BoS201 1, 2, 3, 4
		{ 0, 0x068D7B, 99, 0, 0x04402C, 0x03FF },  // BoS100Fight 99 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 99, 0, 0x0537FC, 0x0001 },  // BoS100Fight 99 <- BoS201B 0
		{ 0, 0x068D7B, 99, 0, 0x0537FF, 0x0006 },  // BoS100Fight 99 <- BoS202 1, 2
		{ 0, 0x068D7B, 99, 0, 0x06F5C1, 0x0007 },  // BoS100Fight 99 <- BoS101 0, 1, 2
		{ 0, 0x068D7B, 99, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 99 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 99, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 99 <- BoSM01 0, 1
		{ 0, 0x068D7B, 99, 0, 0x0B9F9D, 0x0007 },  // BoS100Fight 99 <- BoS302 0, 1, 2
		{ 0, 0x068D7B, 99, 0, 0x0FDC8C, 0x000F },  // BoS100Fight 99 <- BoS303 0, 1, 2, 3
		{ 0, 0x068D7B, 99, 0, 0x107A1C, 0x0001 },  // BoS100Fight 99 <- BoS302B 0
		{ 0, 0x068D7B, 99, 0, 0x142EDC, 0x0001 },  // BoS100Fight 99 <- BoSM00 0
		{ 0, 0x068D7B, 99, 0, 0x173ED9, 0x0007 },  // BoS100Fight 99 <- BoS304 0, 1, 2
		{ 0, 0x068D7B, 100, 0, 0x02BF20, 0x0001 },  // BoS100Fight 100 <- BoS200 0
		{ 0, 0x068D7B, 100, 0, 0x02BF21, 0x001E },  // BoS100Fight 100 <- BoS201 1, 2, 3, 4
		{ 0, 0x068D7B, 100, 0, 0x04402C, 0x03FF },  // BoS100Fight 100 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x068D7B, 100, 0, 0x0537FC, 0x0001 },  // BoS100Fight 100 <- BoS201B 0
		{ 0, 0x068D7B, 100, 0, 0x0537FF, 0x0006 },  // BoS100Fight 100 <- BoS202 1, 2
		{ 0, 0x068D7B, 100, 0, 0x06F5C1, 0x0007 },  // BoS100Fight 100 <- BoS101 0, 1, 2
		{ 0, 0x068D7B, 100, 0, 0x0ADCE5, 0x001F },  // BoS100Fight 100 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x068D7B, 100, 0, 0x0B1D79, 0x0003 },  // BoS100Fight 100 <- BoSM01 0, 1
		{ 0, 0x068D7B, 100, 0, 0x0B9F9D, 0x0007 },  // BoS100Fight 100 <- BoS302 0, 1, 2
		{ 0, 0x068D7B, 100, 0, 0x0FDC8C, 0x000F },  // BoS100Fight 100 <- BoS303 0, 1, 2, 3
		{ 0, 0x068D7B, 100, 0, 0x107A1C, 0x0001 },  // BoS100Fight 100 <- BoS302B 0
		{ 0, 0x068D7B, 100, 0, 0x142EDC, 0x0001 },  // BoS100Fight 100 <- BoSM00 0
		{ 0, 0x068D7B, 100, 0, 0x173ED9, 0x0007 },  // BoS100Fight 100 <- BoS304 0, 1, 2
		{ 0, 0x06B500, 10, 0, 0x0229EA, 0x01FE },  // MQ204 10 <- MQ203 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06B500, 10, 0, 0x06B500, 0x0001 },  // MQ204 10 <- MQ204 0
		{ 0, 0x06D41F, 500, 0, 0x0229E9, 0x0001 },  // MQ201Piper 500 <- MQ202 0
		{ 0, 0x06D41F, 500, 0, 0x0229EA, 0x0001 },  // MQ201Piper 500 <- MQ203 0
		{ 0, 0x06D646, 0, 0, 0x06D646, 0x03FE },  // DN084 0 <- DN084 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 0, 0, 0x0FDC8C, 0x0004 },  // DN084 0 <- BoS303 2
		{ 0, 0x06D646, 10, 0, 0x0229EE, 0x0004 },  // DN084 10 <- MQ302 2
		{ 0, 0x06D646, 10, 0, 0x043275, 0x0002 },  // DN084 10 <- RR303 1
		{ 0, 0x06D646, 10, 0, 0x06D646, 0x03FE },  // DN084 10 <- DN084 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 10, 0, 0x0DDFDB, 0x0003 },  // DN084 10 <- Min301 0, 1
		{ 0, 0x06D646, 10, 0, 0x0DFB3C, 0x0001 },  // DN084 10 <- MinDestBoS 0
		{ 0, 0x06D646, 10, 0, 0x0FDC8C, 0x000E },  // DN084 10 <- BoS303 1, 2, 3
		{ 0, 0x06D646, 10, 0, 0x142FD3, 0x42100002 },  // DN084 10 <- RR304 1, 20, 25, 30
		{ 0, 0x06D646, 10, 0, 0x174029, 0x01FF },  // DN084 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06D646, 11, 0, 0x06D646, 0x02AA },  // DN084 11 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x06D646, 11, 0, 0x0FDC8C, 0x000E },  // DN084 11 <- BoS303 1, 2, 3
		{ 0, 0x06D646, 12, 0, 0x06D646, 0x0154 },  // DN084 12 <- DN084 2, 4, 6, 8
		{ 0, 0x06D646, 13, 0, 0x043275, 0x0002 },  // DN084 13 <- RR303 1
		{ 0, 0x06D646, 13, 0, 0x06D646, 0x0154 },  // DN084 13 <- DN084 2, 4, 6, 8
		{ 0, 0x06D646, 13, 0, 0x142FD3, 0x42100002 },  // DN084 13 <- RR304 1, 20, 25, 30
		{ 0, 0x06D646, 13, 0, 0x174029, 0x01FF },  // DN084 13 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06D646, 14, 0, 0x0229EE, 0x0004 },  // DN084 14 <- MQ302 2
		{ 0, 0x06D646, 14, 0, 0x06D646, 0x02AA },  // DN084 14 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x06D646, 14, 0, 0x0DDFDB, 0x0003 },  // DN084 14 <- Min301 0, 1
		{ 0, 0x06D646, 14, 0, 0x0DFB3C, 0x0001 },  // DN084 14 <- MinDestBoS 0
		{ 0, 0x06D646, 14, 0, 0x0FDC8C, 0x0006 },  // DN084 14 <- BoS303 1, 2
		{ 0, 0x06D646, 20, 0, 0x06D646, 0x03F8 },  // DN084 20 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 20, 0, 0x0FDC8C, 0x0004 },  // DN084 20 <- BoS303 2
		{ 0, 0x06D646, 100, 0, 0x06D646, 0x03F8 },  // DN084 100 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 100, 0, 0x0FDC8C, 0x0004 },  // DN084 100 <- BoS303 2
		{ 0, 0x06D646, 110, 0, 0x06D646, 0x03F8 },  // DN084 110 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 110, 0, 0x0FDC8C, 0x0004 },  // DN084 110 <- BoS303 2
		{ 0, 0x06D646, 120, 0, 0x06D646, 0x03F8 },  // DN084 120 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06D646, 120, 0, 0x0FDC8C, 0x0004 },  // DN084 120 <- BoS303 2
		{ 0, 0x06D646, 200, 0, 0x06D646, 0x01F0 },  // DN084 200 <- DN084 4, 5, 6, 7, 8
		{ 0, 0x06D646, 220, 0, 0x06D646, 0x01E0 },  // DN084 220 <- DN084 5, 6, 7, 8
		{ 0, 0x06D646, 229, 0, 0x06D646, 0x0010 },  // DN084 229 <- DN084 4
		{ 0, 0x06D646, 260, 0, 0x06D646, 0x01E0 },  // DN084 260 <- DN084 5, 6, 7, 8
		{ 0, 0x06D646, 280, 0, 0x06D646, 0x01E0 },  // DN084 280 <- DN084 5, 6, 7, 8
		{ 0, 0x06D646, 290, 0, 0x06D646, 0x0180 },  // DN084 290 <- DN084 7, 8
		{ 0, 0x06D646, 300, 0, 0x06D646, 0x0180 },  // DN084 300 <- DN084 7, 8
		{ 0, 0x06D646, 310, 0, 0x06D646, 0x0180 },  // DN084 310 <- DN084 7, 8
		{ 0, 0x06D646, 320, 0, 0x06D646, 0x0180 },  // DN084 320 <- DN084 7, 8
		{ 0, 0x06D646, 360, 0, 0x06D646, 0x0180 },  // DN084 360 <- DN084 7, 8
		{ 0, 0x06D646, 370, 0, 0x06D646, 0x0180 },  // DN084 370 <- DN084 7, 8
		{ 0, 0x06D646, 372, 0, 0x06D646, 0x0180 },  // DN084 372 <- DN084 7, 8
		{ 0, 0x06D646, 390, 0, 0x06D646, 0x0180 },  // DN084 390 <- DN084 7, 8
		{ 0, 0x06D646, 391, 0, 0x06D646, 0x0180 },  // DN084 391 <- DN084 7, 8
		{ 0, 0x06D646, 399, 0, 0x06D646, 0x0180 },  // DN084 399 <- DN084 7, 8
		{ 0, 0x06F08A, 10, 0, 0x02BF20, 0x0001 },  // DN130 10 <- BoS200 0
		{ 0, 0x06F08A, 10, 0, 0x02BF21, 0x001E },  // DN130 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F08A, 10, 0, 0x043275, 0x0002 },  // DN130 10 <- RR303 1
		{ 0, 0x06F08A, 10, 0, 0x04402C, 0x03FF },  // DN130 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F08A, 10, 0, 0x0537FC, 0x0001 },  // DN130 10 <- BoS201B 0
		{ 0, 0x06F08A, 10, 0, 0x0537FF, 0x0006 },  // DN130 10 <- BoS202 1, 2
		{ 0, 0x06F08A, 10, 0, 0x06D646, 0x0154 },  // DN130 10 <- DN084 2, 4, 6, 8
		{ 0, 0x06F08A, 10, 0, 0x06F5C1, 0x0007 },  // DN130 10 <- BoS101 0, 1, 2
		{ 0, 0x06F08A, 10, 0, 0x0ADCE5, 0x001F },  // DN130 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F08A, 10, 0, 0x0B1D79, 0x0003 },  // DN130 10 <- BoSM01 0, 1
		{ 0, 0x06F08A, 10, 0, 0x0B9F9D, 0x0007 },  // DN130 10 <- BoS302 0, 1, 2
		{ 0, 0x06F08A, 10, 0, 0x0FDC8C, 0x000F },  // DN130 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F08A, 10, 0, 0x107A1C, 0x0001 },  // DN130 10 <- BoS302B 0
		{ 0, 0x06F08A, 10, 0, 0x142EDC, 0x0001 },  // DN130 10 <- BoSM00 0
		{ 0, 0x06F08A, 10, 0, 0x142FD3, 0x42100002 },  // DN130 10 <- RR304 1, 20, 25, 30
		{ 0, 0x06F08A, 10, 0, 0x173ED9, 0x0007 },  // DN130 10 <- BoS304 0, 1, 2
		{ 0, 0x06F08A, 10, 0, 0x174029, 0x01FF },  // DN130 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06F08A, 11, 0, 0x043275, 0x0002 },  // DN130 11 <- RR303 1
		{ 0, 0x06F08A, 11, 0, 0x06D646, 0x0154 },  // DN130 11 <- DN084 2, 4, 6, 8
		{ 0, 0x06F08A, 11, 0, 0x142FD3, 0x42100002 },  // DN130 11 <- RR304 1, 20, 25, 30
		{ 0, 0x06F08A, 11, 0, 0x174029, 0x01FF },  // DN130 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06F08A, 20, 0, 0x02BF20, 0x0001 },  // DN130 20 <- BoS200 0
		{ 0, 0x06F08A, 20, 0, 0x02BF21, 0x001E },  // DN130 20 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F08A, 20, 0, 0x04402C, 0x03FF },  // DN130 20 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F08A, 20, 0, 0x0537FC, 0x0001 },  // DN130 20 <- BoS201B 0
		{ 0, 0x06F08A, 20, 0, 0x0537FF, 0x0006 },  // DN130 20 <- BoS202 1, 2
		{ 0, 0x06F08A, 20, 0, 0x06F5C1, 0x0007 },  // DN130 20 <- BoS101 0, 1, 2
		{ 0, 0x06F08A, 20, 0, 0x0ADCE5, 0x001F },  // DN130 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F08A, 20, 0, 0x0B1D79, 0x0003 },  // DN130 20 <- BoSM01 0, 1
		{ 0, 0x06F08A, 20, 0, 0x0B9F9D, 0x0007 },  // DN130 20 <- BoS302 0, 1, 2
		{ 0, 0x06F08A, 20, 0, 0x0FDC8C, 0x000F },  // DN130 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F08A, 20, 0, 0x107A1C, 0x0001 },  // DN130 20 <- BoS302B 0
		{ 0, 0x06F08A, 20, 0, 0x142EDC, 0x0001 },  // DN130 20 <- BoSM00 0
		{ 0, 0x06F08A, 20, 0, 0x173ED9, 0x0007 },  // DN130 20 <- BoS304 0, 1, 2
		{ 0, 0x06F08A, 30, 0, 0x02BF20, 0x0001 },  // DN130 30 <- BoS200 0
		{ 0, 0x06F08A, 30, 0, 0x02BF21, 0x0018 },  // DN130 30 <- BoS201 3, 4
		{ 0, 0x06F08A, 30, 0, 0x0ADCE5, 0x001F },  // DN130 30 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F08A, 40, 0, 0x02BF21, 0x0018 },  // DN130 40 <- BoS201 3, 4
		{ 0, 0x06F08A, 40, 0, 0x0ADCE5, 0x001F },  // DN130 40 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F08A, 50, 0, 0x02BF21, 0x0018 },  // DN130 50 <- BoS201 3, 4
		{ 0, 0x06F08A, 50, 0, 0x0ADCE5, 0x001F },  // DN130 50 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F08A, 60, 0, 0x02BF21, 0x0018 },  // DN130 60 <- BoS201 3, 4
		{ 0, 0x06F08A, 60, 0, 0x0ADCE5, 0x001F },  // DN130 60 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F102, 255, 0, 0x043275, 0x0002 },  // BoS100Radio 255 <- RR303 1
		{ 0, 0x06F102, 255, 0, 0x06D646, 0x0154 },  // BoS100Radio 255 <- DN084 2, 4, 6, 8
		{ 0, 0x06F102, 255, 0, 0x142FD3, 0x42100002 },  // BoS100Radio 255 <- RR304 1, 20, 25, 30
		{ 0, 0x06F102, 255, 0, 0x174029, 0x01FF },  // BoS100Radio 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06F5BB, 10, 0, 0x02BF20, 0x0001 },  // BoSDialoguePoliceStation 10 <- BoS200 0
		{ 0, 0x06F5BB, 10, 0, 0x02BF21, 0x001E },  // BoSDialoguePoliceStation 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F5BB, 10, 0, 0x04402C, 0x03FF },  // BoSDialoguePoliceStation 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F5BB, 10, 0, 0x0537FC, 0x0001 },  // BoSDialoguePoliceStation 10 <- BoS201B 0
		{ 0, 0x06F5BB, 10, 0, 0x0537FF, 0x0006 },  // BoSDialoguePoliceStation 10 <- BoS202 1, 2
		{ 0, 0x06F5BB, 10, 0, 0x06F5C1, 0x0007 },  // BoSDialoguePoliceStation 10 <- BoS101 0, 1, 2
		{ 0, 0x06F5BB, 10, 0, 0x0ADCE5, 0x001F },  // BoSDialoguePoliceStation 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F5BB, 10, 0, 0x0B1D79, 0x0003 },  // BoSDialoguePoliceStation 10 <- BoSM01 0, 1
		{ 0, 0x06F5BB, 10, 0, 0x0B9F9D, 0x0007 },  // BoSDialoguePoliceStation 10 <- BoS302 0, 1, 2
		{ 0, 0x06F5BB, 10, 0, 0x0FDC8C, 0x000F },  // BoSDialoguePoliceStation 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F5BB, 10, 0, 0x107A1C, 0x0001 },  // BoSDialoguePoliceStation 10 <- BoS302B 0
		{ 0, 0x06F5BB, 10, 0, 0x142EDC, 0x0001 },  // BoSDialoguePoliceStation 10 <- BoSM00 0
		{ 0, 0x06F5BB, 10, 0, 0x173ED9, 0x0007 },  // BoSDialoguePoliceStation 10 <- BoS304 0, 1, 2
		{ 0, 0x06F5BB, 40, 0, 0x02BF21, 0x001E },  // BoSDialoguePoliceStation 40 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F5BB, 40, 0, 0x04402C, 0x03FF },  // BoSDialoguePoliceStation 40 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F5BB, 40, 0, 0x0537FC, 0x0001 },  // BoSDialoguePoliceStation 40 <- BoS201B 0
		{ 0, 0x06F5BB, 40, 0, 0x0537FF, 0x0006 },  // BoSDialoguePoliceStation 40 <- BoS202 1, 2
		{ 0, 0x06F5BB, 40, 0, 0x06F5C1, 0x0004 },  // BoSDialoguePoliceStation 40 <- BoS101 2
		{ 0, 0x06F5BB, 40, 0, 0x0B1D79, 0x0003 },  // BoSDialoguePoliceStation 40 <- BoSM01 0, 1
		{ 0, 0x06F5BB, 40, 0, 0x0B9F9D, 0x0007 },  // BoSDialoguePoliceStation 40 <- BoS302 0, 1, 2
		{ 0, 0x06F5BB, 40, 0, 0x0FDC8C, 0x000F },  // BoSDialoguePoliceStation 40 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F5BB, 40, 0, 0x107A1C, 0x0001 },  // BoSDialoguePoliceStation 40 <- BoS302B 0
		{ 0, 0x06F5BB, 40, 0, 0x142EDC, 0x0001 },  // BoSDialoguePoliceStation 40 <- BoSM00 0
		{ 0, 0x06F5BB, 40, 0, 0x173ED9, 0x0007 },  // BoSDialoguePoliceStation 40 <- BoS304 0, 1, 2
		{ 0, 0x06F5BB, 355, 0, 0x043275, 0x0002 },  // BoSDialoguePoliceStation 355 <- RR303 1
		{ 0, 0x06F5BB, 355, 0, 0x06D646, 0x0154 },  // BoSDialoguePoliceStation 355 <- DN084 2, 4, 6, 8
		{ 0, 0x06F5BB, 355, 0, 0x142FD3, 0x42100002 },  // BoSDialoguePoliceStation 355 <- RR304 1, 20, 25, 30
		{ 0, 0x06F5BB, 355, 0, 0x174029, 0x01FF },  // BoSDialoguePoliceStation 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06F5C1, 10, 0, 0x02BF20, 0x0001 },  // BoS101 10 <- BoS200 0
		{ 0, 0x06F5C1, 10, 0, 0x02BF21, 0x001E },  // BoS101 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F5C1, 10, 0, 0x04402C, 0x03FF },  // BoS101 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F5C1, 10, 0, 0x0537FC, 0x0001 },  // BoS101 10 <- BoS201B 0
		{ 0, 0x06F5C1, 10, 0, 0x0537FF, 0x0006 },  // BoS101 10 <- BoS202 1, 2
		{ 0, 0x06F5C1, 10, 0, 0x06F5C1, 0x0007 },  // BoS101 10 <- BoS101 0, 1, 2
		{ 0, 0x06F5C1, 10, 0, 0x0ADCE5, 0x001F },  // BoS101 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x06F5C1, 10, 0, 0x0B1D79, 0x0003 },  // BoS101 10 <- BoSM01 0, 1
		{ 0, 0x06F5C1, 10, 0, 0x0B9F9D, 0x0007 },  // BoS101 10 <- BoS302 0, 1, 2
		{ 0, 0x06F5C1, 10, 0, 0x0FDC8C, 0x000F },  // BoS101 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F5C1, 10, 0, 0x107A1C, 0x0001 },  // BoS101 10 <- BoS302B 0
		{ 0, 0x06F5C1, 10, 0, 0x142EDC, 0x0001 },  // BoS101 10 <- BoSM00 0
		{ 0, 0x06F5C1, 10, 0, 0x173ED9, 0x0007 },  // BoS101 10 <- BoS304 0, 1, 2
		{ 0, 0x06F5C1, 20, 0, 0x06F5C1, 0x0002 },  // BoS101 20 <- BoS101 1
		{ 0, 0x06F5C1, 30, 0, 0x06F5C1, 0x0006 },  // BoS101 30 <- BoS101 1, 2
		{ 0, 0x06F5C1, 40, 0, 0x06F5C1, 0x0004 },  // BoS101 40 <- BoS101 2
		{ 0, 0x06F5C1, 50, 0, 0x06F5C1, 0x0004 },  // BoS101 50 <- BoS101 2
		{ 0, 0x06F5C1, 60, 0, 0x06F5C1, 0x0004 },  // BoS101 60 <- BoS101 2
		{ 0, 0x06F5C1, 70, 0, 0x06F5C1, 0x0004 },  // BoS101 70 <- BoS101 2
		{ 0, 0x06F5C1, 80, 0, 0x06F5C1, 0x0004 },  // BoS101 80 <- BoS101 2
		{ 0, 0x06F5C1, 90, 0, 0x06F5C1, 0x0004 },  // BoS101 90 <- BoS101 2
		{ 0, 0x06F5C1, 100, 0, 0x06F5C1, 0x0004 },  // BoS101 100 <- BoS101 2
		{ 0, 0x06F5C1, 110, 0, 0x06F5C1, 0x0004 },  // BoS101 110 <- BoS101 2
		{ 0, 0x06F5C1, 120, 0, 0x06F5C1, 0x0004 },  // BoS101 120 <- BoS101 2
		{ 0, 0x06F5C1, 130, 0, 0x06F5C1, 0x0004 },  // BoS101 130 <- BoS101 2
		{ 0, 0x06F5C1, 140, 0, 0x06F5C1, 0x0004 },  // BoS101 140 <- BoS101 2
		{ 0, 0x06F5C1, 150, 0, 0x06F5C1, 0x0004 },  // BoS101 150 <- BoS101 2
		{ 0, 0x06F5C1, 160, 0, 0x06F5C1, 0x0004 },  // BoS101 160 <- BoS101 2
		{ 0, 0x06F5C1, 170, 0, 0x06F5C1, 0x0004 },  // BoS101 170 <- BoS101 2
		{ 0, 0x06F5C1, 180, 0, 0x06F5C1, 0x0004 },  // BoS101 180 <- BoS101 2
		{ 0, 0x06F5C1, 190, 0, 0x06F5C1, 0x0004 },  // BoS101 190 <- BoS101 2
		{ 0, 0x06F5C1, 200, 0, 0x06F5C1, 0x0004 },  // BoS101 200 <- BoS101 2
		{ 0, 0x06F5C1, 205, 0, 0x06F5C1, 0x0004 },  // BoS101 205 <- BoS101 2
		{ 0, 0x06F5C1, 320, 0, 0x02BF20, 0x0001 },  // BoS101 320 <- BoS200 0
		{ 0, 0x06F5C1, 455, 0, 0x02BF20, 0x0001 },  // BoS101 455 <- BoS200 0
		{ 0, 0x06F5C1, 555, 0, 0x02BF21, 0x001E },  // BoS101 555 <- BoS201 1, 2, 3, 4
		{ 0, 0x06F5C1, 555, 0, 0x04402C, 0x03FF },  // BoS101 555 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x06F5C1, 555, 0, 0x0537FC, 0x0001 },  // BoS101 555 <- BoS201B 0
		{ 0, 0x06F5C1, 555, 0, 0x0537FF, 0x0006 },  // BoS101 555 <- BoS202 1, 2
		{ 0, 0x06F5C1, 555, 0, 0x0B1D79, 0x0003 },  // BoS101 555 <- BoSM01 0, 1
		{ 0, 0x06F5C1, 555, 0, 0x0B9F9D, 0x0007 },  // BoS101 555 <- BoS302 0, 1, 2
		{ 0, 0x06F5C1, 555, 0, 0x0FDC8C, 0x000F },  // BoS101 555 <- BoS303 0, 1, 2, 3
		{ 0, 0x06F5C1, 555, 0, 0x107A1C, 0x0001 },  // BoS101 555 <- BoS302B 0
		{ 0, 0x06F5C1, 555, 0, 0x142EDC, 0x0001 },  // BoS101 555 <- BoSM00 0
		{ 0, 0x06F5C1, 555, 0, 0x173ED9, 0x0007 },  // BoS101 555 <- BoS304 0, 1, 2
		{ 0, 0x06F5C1, 655, 0, 0x0229EE, 0x0004 },  // BoS101 655 <- MQ302 2
		{ 0, 0x06F5C1, 655, 0, 0x043275, 0x0002 },  // BoS101 655 <- RR303 1
		{ 0, 0x06F5C1, 655, 0, 0x06D646, 0x0154 },  // BoS101 655 <- DN084 2, 4, 6, 8
		{ 0, 0x06F5C1, 655, 0, 0x142FD3, 0x42100002 },  // BoS101 655 <- RR304 1, 20, 25, 30
		{ 0, 0x06F5C1, 655, 0, 0x174029, 0x01FF },  // BoS101 655 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x06FA37, 1, 0, 0x0229EE, 0x0008 },  // RR102 1 <- MQ302 3
		{ 0, 0x06FA37, 1, 0, 0x043275, 0x0002 },  // RR102 1 <- RR303 1
		{ 0, 0x06FA37, 1, 0, 0x07CC61, 0x0400 },  // RR102 1 <- RR301 10
		{ 0, 0x06FA37, 1, 0, 0x0DFB3C, 0x0001 },  // RR102 1 <- MinDestBoS 0
		{ 0, 0x06FA37, 1, 0, 0x142FD3, 0x42100002 },  // RR102 1 <- RR304 1, 20, 25, 30
		{ 0, 0x06FA37, 50, 0, 0x043275, 0x0002 },  // RR102 50 <- RR303 1
		{ 0, 0x06FA37, 50, 0, 0x07CC61, 0x0400 },  // RR102 50 <- RR301 10
		{ 0, 0x06FA37, 50, 0, 0x142FD3, 0x42100002 },  // RR102 50 <- RR304 1, 20, 25, 30
		{ 0, 0x06FA37, 800, 0, 0x0229EE, 0x0008 },  // RR102 800 <- MQ302 3
		{ 0, 0x06FA37, 800, 0, 0x043275, 0x0002 },  // RR102 800 <- RR303 1
		{ 0, 0x06FA37, 800, 0, 0x07CC61, 0x0400 },  // RR102 800 <- RR301 10
		{ 0, 0x06FA37, 800, 0, 0x142FD3, 0x42100002 },  // RR102 800 <- RR304 1, 20, 25, 30
		{ 0, 0x06FA37, 900, 0, 0x0229EE, 0x0008 },  // RR102 900 <- MQ302 3
		{ 0, 0x06FA37, 900, 0, 0x043275, 0x0002 },  // RR102 900 <- RR303 1
		{ 0, 0x06FA37, 900, 0, 0x07CC61, 0x0400 },  // RR102 900 <- RR301 10
		{ 0, 0x06FA37, 900, 0, 0x142FD3, 0x42100002 },  // RR102 900 <- RR304 1, 20, 25, 30
		{ 0, 0x06FA37, 1000, 0, 0x07CC61, 0x0400 },  // RR102 1000 <- RR301 10
		{ 0, 0x06FA37, 1100, 0, 0x0DFB3C, 0x0001 },  // RR102 1100 <- MinDestBoS 0
		{ 0, 0x0747C9, 10, 0, 0x022A07, 0x0002 },  // MS16MiscObj 10 <- MS16 1
		{ 0, 0x07CC61, 0, 0, 0x043275, 0x0002 },  // RR301 0 <- RR303 1
		{ 0, 0x07CC61, 0, 0, 0x07CC61, 0x0400 },  // RR301 0 <- RR301 10
		{ 0, 0x07CC61, 0, 0, 0x0DFB3C, 0x0001 },  // RR301 0 <- MinDestBoS 0
		{ 0, 0x07CC61, 0, 0, 0x142FD3, 0x42100002 },  // RR301 0 <- RR304 1, 20, 25, 30
		{ 0, 0x07CC61, 50, 0, 0x07CC61, 0x0400 },  // RR301 50 <- RR301 10
		{ 0, 0x07CC61, 9000, 0, 0x043275, 0x0002 },  // RR301 9000 <- RR303 1
		{ 0, 0x07CC61, 9000, 0, 0x0DFB3C, 0x0001 },  // RR301 9000 <- MinDestBoS 0
		{ 0, 0x07CC61, 9000, 0, 0x142FD3, 0x42100002 },  // RR301 9000 <- RR304 1, 20, 25, 30
		{ 0, 0x08672B, 10, 0, 0x06F5C1, 0x0007 },  // BoS101ArcJet 10 <- BoS101 0, 1, 2
		{ 0, 0x087304, 10, 0, 0x043275, 0x0002 },  // BoSM01Pointer 10 <- RR303 1
		{ 0, 0x087304, 10, 0, 0x04402C, 0x03FF },  // BoSM01Pointer 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x087304, 10, 0, 0x06D646, 0x0154 },  // BoSM01Pointer 10 <- DN084 2, 4, 6, 8
		{ 0, 0x087304, 10, 0, 0x0ADCE5, 0x001F },  // BoSM01Pointer 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x087304, 10, 0, 0x0B1D79, 0x0001 },  // BoSM01Pointer 10 <- BoSM01 0
		{ 0, 0x087304, 10, 0, 0x142FD3, 0x42100002 },  // BoSM01Pointer 10 <- RR304 1, 20, 25, 30
		{ 0, 0x087304, 10, 0, 0x174029, 0x01FF },  // BoSM01Pointer 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x087304, 11, 0, 0x043275, 0x0002 },  // BoSM01Pointer 11 <- RR303 1
		{ 0, 0x087304, 11, 0, 0x06D646, 0x0154 },  // BoSM01Pointer 11 <- DN084 2, 4, 6, 8
		{ 0, 0x087304, 11, 0, 0x142FD3, 0x42100002 },  // BoSM01Pointer 11 <- RR304 1, 20, 25, 30
		{ 0, 0x087304, 11, 0, 0x174029, 0x01FF },  // BoSM01Pointer 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x087304, 20, 0, 0x04402C, 0x03FF },  // BoSM01Pointer 20 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x087304, 20, 0, 0x0ADCE5, 0x001F },  // BoSM01Pointer 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x087304, 255, 0, 0x04402C, 0x03FF },  // BoSM01Pointer 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x087304, 255, 0, 0x0ADCE5, 0x001F },  // BoSM01Pointer 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0986C7, 5, 0, 0x0229EE, 0x000F },  // InstM01 5 <- MQ302 0, 1, 2, 3
		{ 0, 0x0986C7, 5, 0, 0x06D646, 0x02AA },  // InstM01 5 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0986C7, 5, 0, 0x0986C7, 0x0001 },  // InstM01 5 <- InstM01 0
		{ 0, 0x0986C7, 5, 0, 0x0C9B0B, 0x1004000000000000 },  // InstM01 5 <- MQ00 50, 60
		{ 0, 0x0986C7, 5, 0, 0x0DDFDB, 0x0003 },  // InstM01 5 <- Min301 0, 1
		{ 0, 0x0986C7, 5, 0, 0x0DFB3C, 0x0001 },  // InstM01 5 <- MinDestBoS 0
		{ 0, 0x0986C7, 5, 0, 0x0FDC8C, 0x0006 },  // InstM01 5 <- BoS303 1, 2
		{ 0, 0x0986C7, 5, 0, 0x10C64C, 0x0400 },  // InstM01 5 <- MQ302RR 10
		{ 0, 0x0986C7, 1500, 0, 0x0229EE, 0x000F },  // InstM01 1500 <- MQ302 0, 1, 2, 3
		{ 0, 0x0986C7, 1500, 0, 0x06D646, 0x02AA },  // InstM01 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0986C7, 1500, 0, 0x0C9B0B, 0x1004000000000000 },  // InstM01 1500 <- MQ00 50, 60
		{ 0, 0x0986C7, 1500, 0, 0x0DDFDB, 0x0003 },  // InstM01 1500 <- Min301 0, 1
		{ 0, 0x0986C7, 1500, 0, 0x0DFB3C, 0x0001 },  // InstM01 1500 <- MinDestBoS 0
		{ 0, 0x0986C7, 1500, 0, 0x0FDC8C, 0x0006 },  // InstM01 1500 <- BoS303 1, 2
		{ 0, 0x0986C7, 1500, 0, 0x10C64C, 0x0400 },  // InstM01 1500 <- MQ302RR 10
		{ 0, 0x09FF4E, 10, 0, 0x04402C, 0x03FF },  // BoS203 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x09FF4E, 10, 0, 0x0ADCE5, 0x001F },  // BoS203 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x09FF4E, 10, 0, 0x0B1D79, 0x0002 },  // BoS203 10 <- BoSM01 1
		{ 0, 0x09FF4E, 10, 0, 0x0B9F9D, 0x0007 },  // BoS203 10 <- BoS302 0, 1, 2
		{ 0, 0x09FF4E, 10, 0, 0x0FDC8C, 0x000F },  // BoS203 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x09FF4E, 10, 0, 0x107A1C, 0x0001 },  // BoS203 10 <- BoS302B 0
		{ 0, 0x09FF4E, 10, 0, 0x142EDC, 0x0001 },  // BoS203 10 <- BoSM00 0
		{ 0, 0x09FF4E, 10, 0, 0x173ED9, 0x0007 },  // BoS203 10 <- BoS304 0, 1, 2
		{ 0, 0x09FF4E, 255, 0, 0x0B9F9D, 0x0007 },  // BoS203 255 <- BoS302 0, 1, 2
		{ 0, 0x09FF4E, 255, 0, 0x0FDC8C, 0x000F },  // BoS203 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x09FF4E, 255, 0, 0x107A1C, 0x0001 },  // BoS203 255 <- BoS302B 0
		{ 0, 0x09FF4E, 255, 0, 0x173ED9, 0x0007 },  // BoS203 255 <- BoS304 0, 1, 2
		{ 0, 0x09FF4E, 355, 0, 0x0229EE, 0x0004 },  // BoS203 355 <- MQ302 2
		{ 0, 0x09FF4E, 355, 0, 0x043275, 0x0002 },  // BoS203 355 <- RR303 1
		{ 0, 0x09FF4E, 355, 0, 0x06D646, 0x0154 },  // BoS203 355 <- DN084 2, 4, 6, 8
		{ 0, 0x09FF4E, 355, 0, 0x142FD3, 0x42100002 },  // BoS203 355 <- RR304 1, 20, 25, 30
		{ 0, 0x09FF4E, 355, 0, 0x174029, 0x01FF },  // BoS203 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0A274B, 10, 0, 0x0229EE, 0x0004 },  // Inst305 10 <- MQ302 2
		{ 0, 0x0A274B, 10, 0, 0x06D646, 0x02AA },  // Inst305 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0A274B, 10, 0, 0x0A274B, 0x0001 },  // Inst305 10 <- Inst305 0
		{ 0, 0x0A274B, 10, 0, 0x0DDFDB, 0x0003 },  // Inst305 10 <- Min301 0, 1
		{ 0, 0x0A274B, 10, 0, 0x0DFB3C, 0x0001 },  // Inst305 10 <- MinDestBoS 0
		{ 0, 0x0A274B, 10, 0, 0x0FDC8C, 0x0006 },  // Inst305 10 <- BoS303 1, 2
		{ 0, 0x0A274B, 1500, 0, 0x0229EE, 0x0004 },  // Inst305 1500 <- MQ302 2
		{ 0, 0x0A274B, 1500, 0, 0x06D646, 0x02AA },  // Inst305 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0A274B, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst305 1500 <- Min301 0, 1
		{ 0, 0x0A274B, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst305 1500 <- MinDestBoS 0
		{ 0, 0x0A274B, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst305 1500 <- BoS303 1, 2
		{ 0, 0x0A8257, 2500, 0, 0x0229EE, 0x000F },  // InstM02 2500 <- MQ302 0, 1, 2, 3
		{ 0, 0x0A8257, 2500, 0, 0x06D646, 0x02AA },  // InstM02 2500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0A8257, 2500, 0, 0x0C9B0B, 0x1004000000000000 },  // InstM02 2500 <- MQ00 50, 60
		{ 0, 0x0A8257, 2500, 0, 0x0DDFDB, 0x0003 },  // InstM02 2500 <- Min301 0, 1
		{ 0, 0x0A8257, 2500, 0, 0x0DFB3C, 0x0001 },  // InstM02 2500 <- MinDestBoS 0
		{ 0, 0x0A8257, 2500, 0, 0x0FDC8C, 0x0006 },  // InstM02 2500 <- BoS303 1, 2
		{ 0, 0x0A8257, 2500, 0, 0x10C64C, 0x0400 },  // InstM02 2500 <- MQ302RR 10
		{ 0, 0x0A8258, 10, 0, 0x0229EE, 0x0004 },  // Inst302 10 <- MQ302 2
		{ 0, 0x0A8258, 10, 0, 0x06D646, 0x02AA },  // Inst302 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0A8258, 10, 0, 0x0A8257, 0x0001 },  // Inst302 10 <- InstM02 0
		{ 0, 0x0A8258, 10, 0, 0x0DDFDB, 0x0003 },  // Inst302 10 <- Min301 0, 1
		{ 0, 0x0A8258, 10, 0, 0x0DFB3C, 0x0001 },  // Inst302 10 <- MinDestBoS 0
		{ 0, 0x0A8258, 10, 0, 0x0FDC8C, 0x0006 },  // Inst302 10 <- BoS303 1, 2
		{ 0, 0x0A8258, 1500, 0, 0x0229EE, 0x0004 },  // Inst302 1500 <- MQ302 2
		{ 0, 0x0A8258, 1500, 0, 0x06D646, 0x02AA },  // Inst302 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0A8258, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst302 1500 <- Min301 0, 1
		{ 0, 0x0A8258, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst302 1500 <- MinDestBoS 0
		{ 0, 0x0A8258, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst302 1500 <- BoS303 1, 2
		{ 0, 0x0A8BAF, 0, 0, 0x0A8BAF, 0x0020 },  // RRM02 0 <- RRM02 5
		{ 0, 0x0A8BAF, 0, 0, 0x0DFB3C, 0x0001 },  // RRM02 0 <- MinDestBoS 0
		{ 0, 0x0A8BAF, 50, 0, 0x0A8BAF, 0x0020 },  // RRM02 50 <- RRM02 5
		{ 0, 0x0A8BAF, 9000, 0, 0x0DFB3C, 0x0001 },  // RRM02 9000 <- MinDestBoS 0
		{ 0, 0x0AA778, 0, 0, 0x0229EE, 0x0004 },  // Min03 0 <- MQ302 2
		{ 0, 0x0AA778, 0, 0, 0x0DDFDB, 0x0003 },  // Min03 0 <- Min301 0, 1
		{ 0, 0x0AA778, 0, 0, 0x0DFB3C, 0x0001 },  // Min03 0 <- MinDestBoS 0
		{ 0, 0x0AA778, 5, 0, 0x0229EE, 0x0004 },  // Min03 5 <- MQ302 2
		{ 0, 0x0AA778, 5, 0, 0x0DDFDB, 0x0003 },  // Min03 5 <- Min301 0, 1
		{ 0, 0x0AA778, 5, 0, 0x0DFB3C, 0x0001 },  // Min03 5 <- MinDestBoS 0
		{ 0, 0x0AA778, 10, 0, 0x0229EE, 0x0004 },  // Min03 10 <- MQ302 2
		{ 0, 0x0AA778, 10, 0, 0x0DDFDB, 0x0003 },  // Min03 10 <- Min301 0, 1
		{ 0, 0x0AA778, 10, 0, 0x0DFB3C, 0x0001 },  // Min03 10 <- MinDestBoS 0
		{ 0, 0x0AA778, 20, 0, 0x0229EE, 0x0004 },  // Min03 20 <- MQ302 2
		{ 0, 0x0AA778, 20, 0, 0x0DDFDB, 0x0003 },  // Min03 20 <- Min301 0, 1
		{ 0, 0x0AA778, 20, 0, 0x0DFB3C, 0x0001 },  // Min03 20 <- MinDestBoS 0
		{ 0, 0x0AA778, 50, 0, 0x0229EE, 0x0004 },  // Min03 50 <- MQ302 2
		{ 0, 0x0AA778, 50, 0, 0x0DDFDB, 0x0003 },  // Min03 50 <- Min301 0, 1
		{ 0, 0x0AA778, 50, 0, 0x0DFB3C, 0x0001 },  // Min03 50 <- MinDestBoS 0
		{ 0, 0x0AA778, 150, 0, 0x0229EE, 0x0004 },  // Min03 150 <- MQ302 2
		{ 0, 0x0AA778, 150, 0, 0x0DDFDB, 0x0003 },  // Min03 150 <- Min301 0, 1
		{ 0, 0x0AA778, 150, 0, 0x0DFB3C, 0x0001 },  // Min03 150 <- MinDestBoS 0
		{ 0, 0x0AA778, 550, 0, 0x0229EE, 0x0004 },  // Min03 550 <- MQ302 2
		{ 0, 0x0AA778, 550, 0, 0x0DDFDB, 0x0003 },  // Min03 550 <- Min301 0, 1
		{ 0, 0x0AA778, 550, 0, 0x0DFB3C, 0x0001 },  // Min03 550 <- MinDestBoS 0
		{ 0, 0x0AA778, 560, 0, 0x0229EE, 0x0004 },  // Min03 560 <- MQ302 2
		{ 0, 0x0AA778, 560, 0, 0x0DDFDB, 0x0003 },  // Min03 560 <- Min301 0, 1
		{ 0, 0x0AA778, 560, 0, 0x0DFB3C, 0x0001 },  // Min03 560 <- MinDestBoS 0
		{ 0, 0x0AA778, 570, 0, 0x0229EE, 0x0004 },  // Min03 570 <- MQ302 2
		{ 0, 0x0AA778, 570, 0, 0x0DDFDB, 0x0003 },  // Min03 570 <- Min301 0, 1
		{ 0, 0x0AA778, 570, 0, 0x0DFB3C, 0x0001 },  // Min03 570 <- MinDestBoS 0
		{ 0, 0x0AC7B3, 10, 0, 0x0AC7B3, 0x0001 },  // Inst306 10 <- Inst306 0
		{ 0, 0x0AC7B3, 1500, 0, 0x0229EE, 0x000F },  // Inst306 1500 <- MQ302 0, 1, 2, 3
		{ 0, 0x0AC7B3, 1500, 0, 0x06D646, 0x02AA },  // Inst306 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0AC7B3, 1500, 0, 0x0C9B0B, 0x1004000000000000 },  // Inst306 1500 <- MQ00 50, 60
		{ 0, 0x0AC7B3, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst306 1500 <- Min301 0, 1
		{ 0, 0x0AC7B3, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst306 1500 <- MinDestBoS 0
		{ 0, 0x0AC7B3, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst306 1500 <- BoS303 1, 2
		{ 0, 0x0AC7B3, 1500, 0, 0x10C64C, 0x0400 },  // Inst306 1500 <- MQ302RR 10
		{ 0, 0x0AD463, 100, 0, 0x0229EE, 0x0004 },  // Min01Misc 100 <- MQ302 2
		{ 0, 0x0AD463, 100, 0, 0x0DDFDB, 0x0003 },  // Min01Misc 100 <- Min301 0, 1
		{ 0, 0x0AD463, 100, 0, 0x0DFB3C, 0x0001 },  // Min01Misc 100 <- MinDestBoS 0
		{ 0, 0x0ADCE5, 10, 0, 0x043275, 0x0002 },  // BoSM04 10 <- RR303 1
		{ 0, 0x0ADCE5, 10, 0, 0x06D646, 0x0154 },  // BoSM04 10 <- DN084 2, 4, 6, 8
		{ 0, 0x0ADCE5, 10, 0, 0x0ADCE5, 0x001F },  // BoSM04 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0ADCE5, 10, 0, 0x142FD3, 0x42100002 },  // BoSM04 10 <- RR304 1, 20, 25, 30
		{ 0, 0x0ADCE5, 10, 0, 0x174029, 0x01FF },  // BoSM04 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0ADCE5, 11, 0, 0x043275, 0x0002 },  // BoSM04 11 <- RR303 1
		{ 0, 0x0ADCE5, 11, 0, 0x06D646, 0x0154 },  // BoSM04 11 <- DN084 2, 4, 6, 8
		{ 0, 0x0ADCE5, 11, 0, 0x142FD3, 0x42100002 },  // BoSM04 11 <- RR304 1, 20, 25, 30
		{ 0, 0x0ADCE5, 11, 0, 0x174029, 0x01FF },  // BoSM04 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0ADCE5, 254, 0, 0x043275, 0x0002 },  // BoSM04 254 <- RR303 1
		{ 0, 0x0ADCE5, 254, 0, 0x06D646, 0x0154 },  // BoSM04 254 <- DN084 2, 4, 6, 8
		{ 0, 0x0ADCE5, 254, 0, 0x142FD3, 0x42100002 },  // BoSM04 254 <- RR304 1, 20, 25, 30
		{ 0, 0x0ADCE5, 254, 0, 0x174029, 0x01FF },  // BoSM04 254 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0ADCE5, 255, 0, 0x043275, 0x0002 },  // BoSM04 255 <- RR303 1
		{ 0, 0x0ADCE5, 255, 0, 0x06D646, 0x0154 },  // BoSM04 255 <- DN084 2, 4, 6, 8
		{ 0, 0x0ADCE5, 255, 0, 0x142FD3, 0x42100002 },  // BoSM04 255 <- RR304 1, 20, 25, 30
		{ 0, 0x0ADCE5, 255, 0, 0x174029, 0x01FF },  // BoSM04 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0AE51C, 10, 0, 0x0B9F9D, 0x0007 },  // BoS301 10 <- BoS302 0, 1, 2
		{ 0, 0x0AE51C, 10, 0, 0x0FDC8C, 0x000F },  // BoS301 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0AE51C, 10, 0, 0x107A1C, 0x0001 },  // BoS301 10 <- BoS302B 0
		{ 0, 0x0AE51C, 10, 0, 0x173ED9, 0x0007 },  // BoS301 10 <- BoS304 0, 1, 2
		{ 0, 0x0AE51C, 20, 0, 0x0B9F9D, 0x0007 },  // BoS301 20 <- BoS302 0, 1, 2
		{ 0, 0x0AE51C, 20, 0, 0x0FDC8C, 0x000F },  // BoS301 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x0AE51C, 20, 0, 0x107A1C, 0x0001 },  // BoS301 20 <- BoS302B 0
		{ 0, 0x0AE51C, 20, 0, 0x173ED9, 0x0007 },  // BoS301 20 <- BoS304 0, 1, 2
		{ 0, 0x0AE51C, 90, 0, 0x0ADCE5, 0x001F },  // BoS301 90 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0AE51C, 255, 0, 0x0B9F9D, 0x0007 },  // BoS301 255 <- BoS302 0, 1, 2
		{ 0, 0x0AE51C, 255, 0, 0x0FDC8C, 0x000F },  // BoS301 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x0AE51C, 255, 0, 0x107A1C, 0x0001 },  // BoS301 255 <- BoS302B 0
		{ 0, 0x0AE51C, 255, 0, 0x173ED9, 0x0007 },  // BoS301 255 <- BoS304 0, 1, 2
		{ 0, 0x0AE51C, 355, 0, 0x0229EE, 0x0004 },  // BoS301 355 <- MQ302 2
		{ 0, 0x0AE51C, 355, 0, 0x043275, 0x0002 },  // BoS301 355 <- RR303 1
		{ 0, 0x0AE51C, 355, 0, 0x06D646, 0x0154 },  // BoS301 355 <- DN084 2, 4, 6, 8
		{ 0, 0x0AE51C, 355, 0, 0x142FD3, 0x42100002 },  // BoS301 355 <- RR304 1, 20, 25, 30
		{ 0, 0x0AE51C, 355, 0, 0x174029, 0x01FF },  // BoS301 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B1752, 10, 0, 0x0229EE, 0x000F },  // MQ206 10 <- MQ302 0, 1, 2, 3
		{ 0, 0x0B1752, 10, 0, 0x0B1752, 0x0001 },  // MQ206 10 <- MQ206 0
		{ 0, 0x0B1752, 10, 0, 0x0DDFDB, 0x0003 },  // MQ206 10 <- Min301 0, 1
		{ 0, 0x0B1752, 10, 0, 0x0DFB3C, 0x0001 },  // MQ206 10 <- MinDestBoS 0
		{ 0, 0x0B1752, 100, 0, 0x0229EE, 0x000F },  // MQ206 100 <- MQ302 0, 1, 2, 3
		{ 0, 0x0B1752, 100, 0, 0x0DDFDB, 0x0003 },  // MQ206 100 <- Min301 0, 1
		{ 0, 0x0B1752, 100, 0, 0x0DFB3C, 0x0001 },  // MQ206 100 <- MinDestBoS 0
		{ 0, 0x0B1752, 200, 0, 0x0229EE, 0x000F },  // MQ206 200 <- MQ302 0, 1, 2, 3
		{ 0, 0x0B1752, 200, 0, 0x0DDFDB, 0x0003 },  // MQ206 200 <- Min301 0, 1
		{ 0, 0x0B1752, 200, 0, 0x0DFB3C, 0x0001 },  // MQ206 200 <- MinDestBoS 0
		{ 0, 0x0B1752, 300, 0, 0x0229EE, 0x000E },  // MQ206 300 <- MQ302 1, 2, 3
		{ 0, 0x0B1752, 300, 0, 0x0DDFDB, 0x0003 },  // MQ206 300 <- Min301 0, 1
		{ 0, 0x0B1752, 300, 0, 0x0DFB3C, 0x0001 },  // MQ206 300 <- MinDestBoS 0
		{ 0, 0x0B1752, 320, 0, 0x0229EE, 0x000A },  // MQ206 320 <- MQ302 1, 3
		{ 0, 0x0B1752, 375, 0, 0x0229EE, 0x000A },  // MQ206 375 <- MQ302 1, 3
		{ 0, 0x0B1752, 600, 0, 0x0B2D48, 0x2108420 },  // MQ206 600 <- RR201 5, 10, 15, 20, 25
		{ 0, 0x0B1752, 1200, 0, 0x0ADCE5, 0x001F },  // MQ206 1200 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0B1752, 2000, 0, 0x0ADCE5, 0x001F },  // MQ206 2000 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0B1D79, 2, 0, 0x0B1D79, 0x0001 },  // BoSM01 2 <- BoSM01 0
		{ 0, 0x0B1D79, 10, 0, 0x0229EE, 0x0004 },  // BoSM01 10 <- MQ302 2
		{ 0, 0x0B1D79, 10, 0, 0x043275, 0x0002 },  // BoSM01 10 <- RR303 1
		{ 0, 0x0B1D79, 10, 0, 0x04402C, 0x03FF },  // BoSM01 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0B1D79, 10, 0, 0x06D646, 0x0154 },  // BoSM01 10 <- DN084 2, 4, 6, 8
		{ 0, 0x0B1D79, 10, 0, 0x0ADCE5, 0x001F },  // BoSM01 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0B1D79, 10, 0, 0x0B1D79, 0x0003 },  // BoSM01 10 <- BoSM01 0, 1
		{ 0, 0x0B1D79, 10, 0, 0x0B9F9D, 0x0007 },  // BoSM01 10 <- BoS302 0, 1, 2
		{ 0, 0x0B1D79, 10, 0, 0x0FDC8C, 0x000F },  // BoSM01 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0B1D79, 10, 0, 0x107A1C, 0x0001 },  // BoSM01 10 <- BoS302B 0
		{ 0, 0x0B1D79, 10, 0, 0x142EDC, 0x0001 },  // BoSM01 10 <- BoSM00 0
		{ 0, 0x0B1D79, 10, 0, 0x142FD3, 0x42100002 },  // BoSM01 10 <- RR304 1, 20, 25, 30
		{ 0, 0x0B1D79, 10, 0, 0x173ED9, 0x0007 },  // BoSM01 10 <- BoS304 0, 1, 2
		{ 0, 0x0B1D79, 10, 0, 0x174029, 0x01FF },  // BoSM01 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B1D79, 10, 2, 0x000801, 0x0001 },  // BoSM01 10 <- DLC01MQ02 0
		{ 0, 0x0B1D79, 10, 2, 0x0010F5, 0x0001 },  // BoSM01 10 <- DLC01MQ05 0
		{ 0, 0x0B1D79, 10, 2, 0x002833, 0x000F },  // BoSM01 10 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x0B1D79, 11, 0, 0x043275, 0x0002 },  // BoSM01 11 <- RR303 1
		{ 0, 0x0B1D79, 11, 0, 0x06D646, 0x0154 },  // BoSM01 11 <- DN084 2, 4, 6, 8
		{ 0, 0x0B1D79, 11, 0, 0x142FD3, 0x42100002 },  // BoSM01 11 <- RR304 1, 20, 25, 30
		{ 0, 0x0B1D79, 11, 0, 0x174029, 0x01FF },  // BoSM01 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B1D79, 12, 0, 0x0229EE, 0x0004 },  // BoSM01 12 <- MQ302 2
		{ 0, 0x0B1D79, 12, 0, 0x043275, 0x0002 },  // BoSM01 12 <- RR303 1
		{ 0, 0x0B1D79, 12, 0, 0x06D646, 0x0154 },  // BoSM01 12 <- DN084 2, 4, 6, 8
		{ 0, 0x0B1D79, 12, 0, 0x142FD3, 0x42100002 },  // BoSM01 12 <- RR304 1, 20, 25, 30
		{ 0, 0x0B1D79, 12, 0, 0x174029, 0x01FF },  // BoSM01 12 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B1D79, 13, 0, 0x0229EE, 0x0004 },  // BoSM01 13 <- MQ302 2
		{ 0, 0x0B1D79, 14, 0, 0x0B9F9D, 0x0006 },  // BoSM01 14 <- BoS302 1, 2
		{ 0, 0x0B1D79, 30, 0, 0x04402C, 0x03FF },  // BoSM01 30 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0B1D79, 30, 0, 0x0ADCE5, 0x001F },  // BoSM01 30 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0B1D79, 200, 0, 0x04402C, 0x03FF },  // BoSM01 200 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0B1D79, 200, 0, 0x0ADCE5, 0x001F },  // BoSM01 200 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0B1D79, 255, 0, 0x04402C, 0x03FF },  // BoSM01 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0B26BC, 1000, 0, 0x0229EE, 0x0008 },  // MQ206RR 1000 <- MQ302 3
		{ 0, 0x0B26BC, 1050, 0, 0x0229EE, 0x0008 },  // MQ206RR 1050 <- MQ302 3
		{ 0, 0x0B26BC, 1100, 0, 0x0229EE, 0x0008 },  // MQ206RR 1100 <- MQ302 3
		{ 0, 0x0B26BD, 1000, 0, 0x0229EE, 0x0002 },  // MQ206BoS 1000 <- MQ302 1
		{ 0, 0x0B26BD, 1050, 0, 0x0229EE, 0x0002 },  // MQ206BoS 1050 <- MQ302 1
		{ 0, 0x0B26BD, 1100, 0, 0x0229EE, 0x0002 },  // MQ206BoS 1100 <- MQ302 1
		{ 0, 0x0B26BE, 1000, 0, 0x0229EE, 0x0004 },  // MQ206Min 1000 <- MQ302 2
		{ 0, 0x0B26BE, 1000, 0, 0x0DDFDB, 0x0003 },  // MQ206Min 1000 <- Min301 0, 1
		{ 0, 0x0B26BE, 1000, 0, 0x0DFB3C, 0x0001 },  // MQ206Min 1000 <- MinDestBoS 0
		{ 0, 0x0B26BE, 1450, 0, 0x0229EE, 0x0004 },  // MQ206Min 1450 <- MQ302 2
		{ 0, 0x0B26BE, 1450, 0, 0x0DDFDB, 0x0003 },  // MQ206Min 1450 <- Min301 0, 1
		{ 0, 0x0B26BE, 1450, 0, 0x0DFB3C, 0x0001 },  // MQ206Min 1450 <- MinDestBoS 0
		{ 0, 0x0B2D47, 5, 0, 0x0229EE, 0x000F },  // InstM03 5 <- MQ302 0, 1, 2, 3
		{ 0, 0x0B2D47, 5, 0, 0x06D646, 0x02AA },  // InstM03 5 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0B2D47, 5, 0, 0x0B2D47, 0x0001 },  // InstM03 5 <- InstM03 0
		{ 0, 0x0B2D47, 5, 0, 0x0C9B0B, 0x1004000000000000 },  // InstM03 5 <- MQ00 50, 60
		{ 0, 0x0B2D47, 5, 0, 0x0DDFDB, 0x0003 },  // InstM03 5 <- Min301 0, 1
		{ 0, 0x0B2D47, 5, 0, 0x0DFB3C, 0x0001 },  // InstM03 5 <- MinDestBoS 0
		{ 0, 0x0B2D47, 5, 0, 0x0FDC8C, 0x0006 },  // InstM03 5 <- BoS303 1, 2
		{ 0, 0x0B2D47, 5, 0, 0x10C64C, 0x0400 },  // InstM03 5 <- MQ302RR 10
		{ 0, 0x0B2D47, 100, 0, 0x0B2D47, 0x0001 },  // InstM03 100 <- InstM03 0
		{ 0, 0x0B2D47, 2500, 0, 0x0229EE, 0x000F },  // InstM03 2500 <- MQ302 0, 1, 2, 3
		{ 0, 0x0B2D47, 2500, 0, 0x06D646, 0x02AA },  // InstM03 2500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0B2D47, 2500, 0, 0x0C9B0B, 0x1004000000000000 },  // InstM03 2500 <- MQ00 50, 60
		{ 0, 0x0B2D47, 2500, 0, 0x0DDFDB, 0x0003 },  // InstM03 2500 <- Min301 0, 1
		{ 0, 0x0B2D47, 2500, 0, 0x0DFB3C, 0x0001 },  // InstM03 2500 <- MinDestBoS 0
		{ 0, 0x0B2D47, 2500, 0, 0x0FDC8C, 0x0006 },  // InstM03 2500 <- BoS303 1, 2
		{ 0, 0x0B2D47, 2500, 0, 0x10C64C, 0x0400 },  // InstM03 2500 <- MQ302RR 10
		{ 0, 0x0B2D48, 0, 0, 0x043275, 0x0002 },  // RR201 0 <- RR303 1
		{ 0, 0x0B2D48, 0, 0, 0x0B2D48, 0x2108420 },  // RR201 0 <- RR201 5, 10, 15, 20, 25
		{ 0, 0x0B2D48, 0, 0, 0x0DFB3C, 0x0001 },  // RR201 0 <- MinDestBoS 0
		{ 0, 0x0B2D48, 0, 0, 0x142FD3, 0x42100002 },  // RR201 0 <- RR304 1, 20, 25, 30
		{ 0, 0x0B2D48, 100, 0, 0x0B2D48, 0x2108420 },  // RR201 100 <- RR201 5, 10, 15, 20, 25
		{ 0, 0x0B2D48, 150, 0, 0x0B2D48, 0x2108400 },  // RR201 150 <- RR201 10, 15, 20, 25
		{ 0, 0x0B2D48, 200, 0, 0x0B2D48, 0x2108400 },  // RR201 200 <- RR201 10, 15, 20, 25
		{ 0, 0x0B2D48, 300, 0, 0x0B2D48, 0x2108000 },  // RR201 300 <- RR201 15, 20, 25
		{ 0, 0x0B2D48, 350, 0, 0x0B2D48, 0x2100000 },  // RR201 350 <- RR201 20, 25
		{ 0, 0x0B2D48, 375, 0, 0x0B2D48, 0x2100000 },  // RR201 375 <- RR201 20, 25
		{ 0, 0x0B2D48, 400, 0, 0x0B2D48, 0x2000000 },  // RR201 400 <- RR201 25
		{ 0, 0x0B2D48, 410, 0, 0x0B2D48, 0x2000000 },  // RR201 410 <- RR201 25
		{ 0, 0x0B2D48, 425, 0, 0x0B2D48, 0x2000000 },  // RR201 425 <- RR201 25
		{ 0, 0x0B2D48, 450, 0, 0x0B2D48, 0x2000000 },  // RR201 450 <- RR201 25
		{ 0, 0x0B2D48, 500, 0, 0x0B2D48, 0x2000000 },  // RR201 500 <- RR201 25
		{ 0, 0x0B2D48, 550, 0, 0x0B2D48, 0x2000000 },  // RR201 550 <- RR201 25
		{ 0, 0x0B2D48, 1000, 0, 0x0B2D48, 0x2108000 },  // RR201 1000 <- RR201 15, 20, 25
		{ 0, 0x0B2D48, 1050, 0, 0x043275, 0x0002 },  // RR201 1050 <- RR303 1
		{ 0, 0x0B2D48, 1050, 0, 0x142FD3, 0x42100002 },  // RR201 1050 <- RR304 1, 20, 25, 30
		{ 0, 0x0B2D48, 9000, 0, 0x0DFB3C, 0x0001 },  // RR201 9000 <- MinDestBoS 0
		{ 0, 0x0B3562, 10, 0, 0x043275, 0x0002 },  // BoSM01Postquest 10 <- RR303 1
		{ 0, 0x0B3562, 10, 0, 0x06D646, 0x0154 },  // BoSM01Postquest 10 <- DN084 2, 4, 6, 8
		{ 0, 0x0B3562, 10, 0, 0x142FD3, 0x42100002 },  // BoSM01Postquest 10 <- RR304 1, 20, 25, 30
		{ 0, 0x0B3562, 10, 0, 0x174029, 0x01FF },  // BoSM01Postquest 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B3562, 11, 0, 0x043275, 0x0002 },  // BoSM01Postquest 11 <- RR303 1
		{ 0, 0x0B3562, 11, 0, 0x06D646, 0x0154 },  // BoSM01Postquest 11 <- DN084 2, 4, 6, 8
		{ 0, 0x0B3562, 11, 0, 0x142FD3, 0x42100002 },  // BoSM01Postquest 11 <- RR304 1, 20, 25, 30
		{ 0, 0x0B3562, 11, 0, 0x174029, 0x01FF },  // BoSM01Postquest 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B3562, 15, 0, 0x043275, 0x0002 },  // BoSM01Postquest 15 <- RR303 1
		{ 0, 0x0B3562, 15, 0, 0x06D646, 0x0154 },  // BoSM01Postquest 15 <- DN084 2, 4, 6, 8
		{ 0, 0x0B3562, 15, 0, 0x142FD3, 0x42100002 },  // BoSM01Postquest 15 <- RR304 1, 20, 25, 30
		{ 0, 0x0B3562, 15, 0, 0x174029, 0x01FF },  // BoSM01Postquest 15 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B3562, 19, 0, 0x043275, 0x0002 },  // BoSM01Postquest 19 <- RR303 1
		{ 0, 0x0B3562, 19, 0, 0x06D646, 0x0154 },  // BoSM01Postquest 19 <- DN084 2, 4, 6, 8
		{ 0, 0x0B3562, 19, 0, 0x142FD3, 0x42100002 },  // BoSM01Postquest 19 <- RR304 1, 20, 25, 30
		{ 0, 0x0B3562, 19, 0, 0x174029, 0x01FF },  // BoSM01Postquest 19 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0B9F9D, 10, 0, 0x0B9F9D, 0x0007 },  // BoS302 10 <- BoS302 0, 1, 2
		{ 0, 0x0B9F9D, 10, 0, 0x0FDC8C, 0x000F },  // BoS302 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0B9F9D, 10, 0, 0x107A1C, 0x0001 },  // BoS302 10 <- BoS302B 0
		{ 0, 0x0B9F9D, 10, 0, 0x173ED9, 0x0007 },  // BoS302 10 <- BoS304 0, 1, 2
		{ 0, 0x0B9F9D, 20, 0, 0x0B9F9D, 0x0006 },  // BoS302 20 <- BoS302 1, 2
		{ 0, 0x0B9F9D, 30, 0, 0x0B9F9D, 0x0006 },  // BoS302 30 <- BoS302 1, 2
		{ 0, 0x0B9F9D, 40, 0, 0x0B9F9D, 0x0006 },  // BoS302 40 <- BoS302 1, 2
		{ 0, 0x0B9F9D, 50, 0, 0x0B9F9D, 0x0006 },  // BoS302 50 <- BoS302 1, 2
		{ 0, 0x0B9F9D, 60, 0, 0x0B9F9D, 0x0006 },  // BoS302 60 <- BoS302 1, 2
		{ 0, 0x0B9F9D, 70, 0, 0x0B9F9D, 0x0004 },  // BoS302 70 <- BoS302 2
		{ 0, 0x0B9F9D, 85, 0, 0x0229EE, 0x0004 },  // BoS302 85 <- MQ302 2
		{ 0, 0x0B9F9D, 90, 0, 0x0229EE, 0x0004 },  // BoS302 90 <- MQ302 2
		{ 0, 0x0B9F9D, 120, 0, 0x0B9F9D, 0x0004 },  // BoS302 120 <- BoS302 2
		{ 0, 0x0B9F9D, 255, 0, 0x0FDC8C, 0x000F },  // BoS302 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x0B9F9D, 255, 0, 0x107A1C, 0x0001 },  // BoS302 255 <- BoS302B 0
		{ 0, 0x0B9F9D, 255, 0, 0x173ED9, 0x0007 },  // BoS302 255 <- BoS304 0, 1, 2
		{ 0, 0x0B9F9D, 355, 0, 0x0229EE, 0x0004 },  // BoS302 355 <- MQ302 2
		{ 0, 0x0B9F9D, 355, 0, 0x043275, 0x0002 },  // BoS302 355 <- RR303 1
		{ 0, 0x0B9F9D, 355, 0, 0x06D646, 0x0154 },  // BoS302 355 <- DN084 2, 4, 6, 8
		{ 0, 0x0B9F9D, 355, 0, 0x142FD3, 0x42100002 },  // BoS302 355 <- RR304 1, 20, 25, 30
		{ 0, 0x0B9F9D, 355, 0, 0x174029, 0x01FF },  // BoS302 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACE9, 10, 0, 0x0229EE, 0x0004 },  // BoSM01DistressSignals 10 <- MQ302 2
		{ 0, 0x0BACE9, 10, 0, 0x043275, 0x0002 },  // BoSM01DistressSignals 10 <- RR303 1
		{ 0, 0x0BACE9, 10, 0, 0x04402C, 0x03FF },  // BoSM01DistressSignals 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0BACE9, 10, 0, 0x06D646, 0x0154 },  // BoSM01DistressSignals 10 <- DN084 2, 4, 6, 8
		{ 0, 0x0BACE9, 10, 0, 0x0ADCE5, 0x001F },  // BoSM01DistressSignals 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0BACE9, 10, 0, 0x0B1D79, 0x0003 },  // BoSM01DistressSignals 10 <- BoSM01 0, 1
		{ 0, 0x0BACE9, 10, 0, 0x0B9F9D, 0x0007 },  // BoSM01DistressSignals 10 <- BoS302 0, 1, 2
		{ 0, 0x0BACE9, 10, 0, 0x0FDC8C, 0x000F },  // BoSM01DistressSignals 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0BACE9, 10, 0, 0x107A1C, 0x0001 },  // BoSM01DistressSignals 10 <- BoS302B 0
		{ 0, 0x0BACE9, 10, 0, 0x142EDC, 0x0001 },  // BoSM01DistressSignals 10 <- BoSM00 0
		{ 0, 0x0BACE9, 10, 0, 0x142FD3, 0x42100002 },  // BoSM01DistressSignals 10 <- RR304 1, 20, 25, 30
		{ 0, 0x0BACE9, 10, 0, 0x173ED9, 0x0007 },  // BoSM01DistressSignals 10 <- BoS304 0, 1, 2
		{ 0, 0x0BACE9, 10, 0, 0x174029, 0x01FF },  // BoSM01DistressSignals 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACE9, 10, 2, 0x000801, 0x0001 },  // BoSM01DistressSignals 10 <- DLC01MQ02 0
		{ 0, 0x0BACE9, 10, 2, 0x0010F5, 0x0001 },  // BoSM01DistressSignals 10 <- DLC01MQ05 0
		{ 0, 0x0BACE9, 10, 2, 0x002833, 0x000F },  // BoSM01DistressSignals 10 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x0BACFF, 10, 0, 0x174029, 0x01FF },  // Inst307 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACFF, 15, 0, 0x174029, 0x01FF },  // Inst307 15 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACFF, 30, 0, 0x174029, 0x01FF },  // Inst307 30 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACFF, 35, 0, 0x174029, 0x01FF },  // Inst307 35 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACFF, 36, 0, 0x174029, 0x01FF },  // Inst307 36 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x0BACFF, 37, 0, 0x174029, 0x00C0 },  // Inst307 37 <- Inst307Fight 6, 7
		{ 0, 0x0BACFF, 38, 0, 0x174029, 0x00C0 },  // Inst307 38 <- Inst307Fight 6, 7
		{ 0, 0x0BACFF, 39, 0, 0x174029, 0x00C0 },  // Inst307 39 <- Inst307Fight 6, 7
		{ 0, 0x0BACFF, 60, 0, 0x174029, 0x00C0 },  // Inst307 60 <- Inst307Fight 6, 7
		{ 0, 0x0BACFF, 1500, 0, 0x0229EE, 0x0004 },  // Inst307 1500 <- MQ302 2
		{ 0, 0x0BACFF, 1500, 0, 0x06D646, 0x02AA },  // Inst307 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0BACFF, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst307 1500 <- Min301 0, 1
		{ 0, 0x0BACFF, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst307 1500 <- MinDestBoS 0
		{ 0, 0x0BACFF, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst307 1500 <- BoS303 1, 2
		{ 0, 0x0BAD00, 40, 0, 0x0C9B0B, 0x1004000000000000 },  // Inst308 40 <- MQ00 50, 60
		{ 0, 0x0BAD00, 50, 0, 0x0C9B0B, 0x1004000000000000 },  // Inst308 50 <- MQ00 50, 60
		{ 0, 0x0BAD00, 200, 0, 0x0C9B0B, 0x1004000000000000 },  // Inst308 200 <- MQ00 50, 60
		{ 0, 0x0BDC04, 10, 0, 0x0ADCE5, 0x001F },  // BoSM02Postquest 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0BDC04, 20, 0, 0x0ADCE5, 0x001F },  // BoSM02Postquest 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0BDC04, 30, 0, 0x0ADCE5, 0x001F },  // BoSM02Postquest 30 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x0DDFDB, 10, 0, 0x0229EE, 0x0004 },  // Min301 10 <- MQ302 2
		{ 0, 0x0DDFDB, 10, 0, 0x0DDFDB, 0x0003 },  // Min301 10 <- Min301 0, 1
		{ 0, 0x0DDFDB, 10, 0, 0x0DFB3C, 0x0001 },  // Min301 10 <- MinDestBoS 0
		{ 0, 0x0DDFDB, 100, 0, 0x0DFB3C, 0x0001 },  // Min301 100 <- MinDestBoS 0
		{ 0, 0x0DFB3C, 10, 0, 0x0229EE, 0x0004 },  // MinDestBoS 10 <- MQ302 2
		{ 0, 0x0DFB3C, 10, 0, 0x0DDFDB, 0x0003 },  // MinDestBoS 10 <- Min301 0, 1
		{ 0, 0x0DFB3C, 10, 0, 0x0DFB3C, 0x0001 },  // MinDestBoS 10 <- MinDestBoS 0
		{ 0, 0x0E2058, 5, 0, 0x0229EE, 0x0004 },  // Inst301 5 <- MQ302 2
		{ 0, 0x0E2058, 5, 0, 0x06D646, 0x02AA },  // Inst301 5 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0E2058, 5, 0, 0x0DDFDB, 0x0003 },  // Inst301 5 <- Min301 0, 1
		{ 0, 0x0E2058, 5, 0, 0x0DFB3C, 0x0001 },  // Inst301 5 <- MinDestBoS 0
		{ 0, 0x0E2058, 5, 0, 0x0FDC8C, 0x0006 },  // Inst301 5 <- BoS303 1, 2
		{ 0, 0x0E2058, 1500, 0, 0x0229EE, 0x0004 },  // Inst301 1500 <- MQ302 2
		{ 0, 0x0E2058, 1500, 0, 0x06D646, 0x02AA },  // Inst301 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0E2058, 1500, 0, 0x0DDFDB, 0x0003 },  // Inst301 1500 <- Min301 0, 1
		{ 0, 0x0E2058, 1500, 0, 0x0DFB3C, 0x0001 },  // Inst301 1500 <- MinDestBoS 0
		{ 0, 0x0E2058, 1500, 0, 0x0FDC8C, 0x0006 },  // Inst301 1500 <- BoS303 1, 2
		{ 0, 0x0EBE71, 10, 0, 0x0229EE, 0x0004 },  // MQ00MamaMurphy 10 <- MQ302 2
		{ 0, 0x0EBE71, 10, 0, 0x0DDFDB, 0x0003 },  // MQ00MamaMurphy 10 <- Min301 0, 1
		{ 0, 0x0EBE71, 10, 0, 0x0DFB3C, 0x0001 },  // MQ00MamaMurphy 10 <- MinDestBoS 0
		{ 0, 0x0F8D39, 10, 0, 0x06D646, 0x0180 },  // DN084_BoS303Battle 10 <- DN084 7, 8
		{ 0, 0x0F8D3A, 10, 0, 0x06D646, 0x0180 },  // DN084_Inst303Battle 10 <- DN084 7, 8
		{ 0, 0x0FDC8C, 10, 0, 0x06D646, 0x02AA },  // BoS303 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0FDC8C, 10, 0, 0x0FDC8C, 0x000F },  // BoS303 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x0FDC8C, 10, 0, 0x173ED9, 0x0007 },  // BoS303 10 <- BoS304 0, 1, 2
		{ 0, 0x0FDC8C, 15, 0, 0x06D646, 0x02AA },  // BoS303 15 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0FDC8C, 15, 0, 0x0FDC8C, 0x000E },  // BoS303 15 <- BoS303 1, 2, 3
		{ 0, 0x0FDC8C, 20, 0, 0x06D646, 0x02AA },  // BoS303 20 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0FDC8C, 20, 0, 0x0FDC8C, 0x000E },  // BoS303 20 <- BoS303 1, 2, 3
		{ 0, 0x0FDC8C, 30, 0, 0x06D646, 0x02AA },  // BoS303 30 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0FDC8C, 30, 0, 0x0FDC8C, 0x0006 },  // BoS303 30 <- BoS303 1, 2
		{ 0, 0x0FDC8C, 35, 0, 0x06D646, 0x02AA },  // BoS303 35 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x0FDC8C, 35, 0, 0x0FDC8C, 0x0006 },  // BoS303 35 <- BoS303 1, 2
		{ 0, 0x0FDC8C, 40, 0, 0x06D646, 0x03F8 },  // BoS303 40 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0FDC8C, 40, 0, 0x0FDC8C, 0x0006 },  // BoS303 40 <- BoS303 1, 2
		{ 0, 0x0FDC8C, 50, 0, 0x06D646, 0x03F8 },  // BoS303 50 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0FDC8C, 50, 0, 0x0FDC8C, 0x0004 },  // BoS303 50 <- BoS303 2
		{ 0, 0x0FDC8C, 60, 0, 0x06D646, 0x03F8 },  // BoS303 60 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0FDC8C, 60, 0, 0x0FDC8C, 0x0004 },  // BoS303 60 <- BoS303 2
		{ 0, 0x0FDC8C, 70, 0, 0x06D646, 0x03F8 },  // BoS303 70 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x0FDC8C, 70, 0, 0x0FDC8C, 0x0004 },  // BoS303 70 <- BoS303 2
		{ 0, 0x0FDC8C, 80, 0, 0x06D646, 0x00A0 },  // BoS303 80 <- DN084 5, 7
		{ 0, 0x0FDC8C, 90, 0, 0x06D646, 0x00B0 },  // BoS303 90 <- DN084 4, 5, 7
		{ 0, 0x0FDC8C, 100, 0, 0x06D646, 0x01E0 },  // BoS303 100 <- DN084 5, 6, 7, 8
		{ 0, 0x0FDC8C, 120, 0, 0x06D646, 0x0180 },  // BoS303 120 <- DN084 7, 8
		{ 0, 0x0FDC8C, 130, 0, 0x06D646, 0x0080 },  // BoS303 130 <- DN084 7
		{ 0, 0x0FDC8C, 140, 0, 0x06D646, 0x0080 },  // BoS303 140 <- DN084 7
		{ 0, 0x0FDC8C, 150, 0, 0x06D646, 0x0180 },  // BoS303 150 <- DN084 7, 8
		{ 0, 0x0FDC8C, 160, 0, 0x06D646, 0x0180 },  // BoS303 160 <- DN084 7, 8
		{ 0, 0x0FDC8C, 170, 0, 0x06D646, 0x0080 },  // BoS303 170 <- DN084 7
		{ 0, 0x0FDC8C, 255, 0, 0x173ED9, 0x0007 },  // BoS303 255 <- BoS304 0, 1, 2
		{ 0, 0x0FDC8C, 355, 0, 0x0229EE, 0x0004 },  // BoS303 355 <- MQ302 2
		{ 0, 0x0FDC8C, 355, 0, 0x043275, 0x0002 },  // BoS303 355 <- RR303 1
		{ 0, 0x0FDC8C, 355, 0, 0x06D646, 0x0154 },  // BoS303 355 <- DN084 2, 4, 6, 8
		{ 0, 0x0FDC8C, 355, 0, 0x142FD3, 0x42100002 },  // BoS303 355 <- RR304 1, 20, 25, 30
		{ 0, 0x0FDC8C, 355, 0, 0x174029, 0x01FF },  // BoS303 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x107A1C, 10, 0, 0x0FDC8C, 0x000F },  // BoS302B 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x107A1C, 10, 0, 0x107A1C, 0x0001 },  // BoS302B 10 <- BoS302B 0
		{ 0, 0x107A1C, 10, 0, 0x173ED9, 0x0007 },  // BoS302B 10 <- BoS304 0, 1, 2
		{ 0, 0x107A1C, 255, 0, 0x0FDC8C, 0x000F },  // BoS302B 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x107A1C, 255, 0, 0x173ED9, 0x0007 },  // BoS302B 255 <- BoS304 0, 1, 2
		{ 0, 0x107A1C, 355, 0, 0x0229EE, 0x0004 },  // BoS302B 355 <- MQ302 2
		{ 0, 0x107A1C, 355, 0, 0x043275, 0x0002 },  // BoS302B 355 <- RR303 1
		{ 0, 0x107A1C, 355, 0, 0x06D646, 0x0154 },  // BoS302B 355 <- DN084 2, 4, 6, 8
		{ 0, 0x107A1C, 355, 0, 0x142FD3, 0x42100002 },  // BoS302B 355 <- RR304 1, 20, 25, 30
		{ 0, 0x107A1C, 355, 0, 0x174029, 0x01FF },  // BoS302B 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x10C64A, 10, 0, 0x0229EE, 0x0004 },  // MQ302Min 10 <- MQ302 2
		{ 0, 0x10C64A, 1900, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302Min 1900 <- MQ00 50, 60
		{ 0, 0x10C64B, 1, 0, 0x0229EE, 0x0002 },  // MQ302BoS 1 <- MQ302 1
		{ 0, 0x10C64B, 1, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302BoS 1 <- MQ00 50, 60
		{ 0, 0x10C64B, 900, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302BoS 900 <- MQ00 50, 60
		{ 0, 0x10C64C, 100, 0, 0x0229EE, 0x0008 },  // MQ302RR 100 <- MQ302 3
		{ 0, 0x10C64C, 100, 0, 0x10C64C, 0x0400 },  // MQ302RR 100 <- MQ302RR 10
		{ 0, 0x10C64C, 900, 0, 0x0C9B0B, 0x1004000000000000 },  // MQ302RR 900 <- MQ00 50, 60
		{ 0, 0x10C64C, 9000, 0, 0x0229EE, 0x0004 },  // MQ302RR 9000 <- MQ302 2
		{ 0, 0x10C64C, 9000, 0, 0x06D646, 0x02AA },  // MQ302RR 9000 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x10C64C, 9000, 0, 0x0DDFDB, 0x0003 },  // MQ302RR 9000 <- Min301 0, 1
		{ 0, 0x10C64C, 9000, 0, 0x0DFB3C, 0x0001 },  // MQ302RR 9000 <- MinDestBoS 0
		{ 0, 0x10C64C, 9000, 0, 0x0FDC8C, 0x0006 },  // MQ302RR 9000 <- BoS303 1, 2
		{ 0, 0x112D10, 10, 0, 0x043275, 0x0002 },  // DN165 10 <- RR303 1
		{ 0, 0x112D10, 10, 0, 0x04402C, 0x03FF },  // DN165 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x112D10, 10, 0, 0x06D646, 0x0154 },  // DN165 10 <- DN084 2, 4, 6, 8
		{ 0, 0x112D10, 10, 0, 0x0ADCE5, 0x001F },  // DN165 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x112D10, 10, 0, 0x142FD3, 0x42100002 },  // DN165 10 <- RR304 1, 20, 25, 30
		{ 0, 0x112D10, 10, 0, 0x174029, 0x01FF },  // DN165 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x112D10, 11, 0, 0x043275, 0x0002 },  // DN165 11 <- RR303 1
		{ 0, 0x112D10, 11, 0, 0x06D646, 0x0154 },  // DN165 11 <- DN084 2, 4, 6, 8
		{ 0, 0x112D10, 11, 0, 0x142FD3, 0x42100002 },  // DN165 11 <- RR304 1, 20, 25, 30
		{ 0, 0x112D10, 11, 0, 0x174029, 0x01FF },  // DN165 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x112D10, 30, 0, 0x043275, 0x0002 },  // DN165 30 <- RR303 1
		{ 0, 0x112D10, 30, 0, 0x04402C, 0x03FF },  // DN165 30 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x112D10, 30, 0, 0x06D646, 0x0154 },  // DN165 30 <- DN084 2, 4, 6, 8
		{ 0, 0x112D10, 30, 0, 0x0ADCE5, 0x001F },  // DN165 30 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x112D10, 30, 0, 0x142FD3, 0x42100002 },  // DN165 30 <- RR304 1, 20, 25, 30
		{ 0, 0x112D10, 30, 0, 0x174029, 0x01FF },  // DN165 30 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x128CFB, 255, 0, 0x0B9F9D, 0x0007 },  // BoS202LinkMQ 255 <- BoS302 0, 1, 2
		{ 0, 0x128CFB, 255, 0, 0x0FDC8C, 0x000F },  // BoS202LinkMQ 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x128CFB, 255, 0, 0x107A1C, 0x0001 },  // BoS202LinkMQ 255 <- BoS302B 0
		{ 0, 0x128CFB, 255, 0, 0x173ED9, 0x0007 },  // BoS202LinkMQ 255 <- BoS304 0, 1, 2
		{ 0, 0x128CFB, 355, 0, 0x0229EE, 0x0004 },  // BoS202LinkMQ 355 <- MQ302 2
		{ 0, 0x134297, 1, 0, 0x0229E7, 0x0001 },  // MQ106Investigations 1 <- MQ106 0
		{ 0, 0x136329, 5, 0, 0x0229EE, 0x0004 },  // Min207 5 <- MQ302 2
		{ 0, 0x136329, 5, 0, 0x0DDFDB, 0x0003 },  // Min207 5 <- Min301 0, 1
		{ 0, 0x136329, 5, 0, 0x0DFB3C, 0x0001 },  // Min207 5 <- MinDestBoS 0
		{ 0, 0x136329, 10, 0, 0x0229EE, 0x0004 },  // Min207 10 <- MQ302 2
		{ 0, 0x136329, 10, 0, 0x0DDFDB, 0x0003 },  // Min207 10 <- Min301 0, 1
		{ 0, 0x136329, 10, 0, 0x0DFB3C, 0x0001 },  // Min207 10 <- MinDestBoS 0
		{ 0, 0x136329, 20, 0, 0x0229EE, 0x0004 },  // Min207 20 <- MQ302 2
		{ 0, 0x136329, 20, 0, 0x0DDFDB, 0x0003 },  // Min207 20 <- Min301 0, 1
		{ 0, 0x136329, 20, 0, 0x0DFB3C, 0x0001 },  // Min207 20 <- MinDestBoS 0
		{ 0, 0x136329, 100, 0, 0x0229EE, 0x0004 },  // Min207 100 <- MQ302 2
		{ 0, 0x136329, 100, 0, 0x0DDFDB, 0x0003 },  // Min207 100 <- Min301 0, 1
		{ 0, 0x136329, 100, 0, 0x0DFB3C, 0x0001 },  // Min207 100 <- MinDestBoS 0
		{ 0, 0x136329, 200, 0, 0x0DDFDB, 0x0002 },  // Min207 200 <- Min301 1
		{ 0, 0x136329, 200, 0, 0x0DFB3C, 0x0001 },  // Min207 200 <- MinDestBoS 0
		{ 0, 0x13C640, 255, 0, 0x0B9F9D, 0x0007 },  // BoS204 255 <- BoS302 0, 1, 2
		{ 0, 0x13C640, 255, 0, 0x0FDC8C, 0x000F },  // BoS204 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x13C640, 255, 0, 0x107A1C, 0x0001 },  // BoS204 255 <- BoS302B 0
		{ 0, 0x13C640, 255, 0, 0x173ED9, 0x0007 },  // BoS204 255 <- BoS304 0, 1, 2
		{ 0, 0x13C640, 355, 0, 0x0229EE, 0x0004 },  // BoS204 355 <- MQ302 2
		{ 0, 0x13C640, 355, 0, 0x043275, 0x0002 },  // BoS204 355 <- RR303 1
		{ 0, 0x13C640, 355, 0, 0x06D646, 0x0154 },  // BoS204 355 <- DN084 2, 4, 6, 8
		{ 0, 0x13C640, 355, 0, 0x142FD3, 0x42100002 },  // BoS204 355 <- RR304 1, 20, 25, 30
		{ 0, 0x13C640, 355, 0, 0x174029, 0x01FF },  // BoS204 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x142EDC, 10, 0, 0x043275, 0x0002 },  // BoSM00 10 <- RR303 1
		{ 0, 0x142EDC, 10, 0, 0x04402C, 0x03FF },  // BoSM00 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x142EDC, 10, 0, 0x06D646, 0x0154 },  // BoSM00 10 <- DN084 2, 4, 6, 8
		{ 0, 0x142EDC, 10, 0, 0x0ADCE5, 0x001F },  // BoSM00 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x142EDC, 10, 0, 0x0B1D79, 0x0002 },  // BoSM00 10 <- BoSM01 1
		{ 0, 0x142EDC, 10, 0, 0x0B9F9D, 0x0007 },  // BoSM00 10 <- BoS302 0, 1, 2
		{ 0, 0x142EDC, 10, 0, 0x0FDC8C, 0x000F },  // BoSM00 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x142EDC, 10, 0, 0x107A1C, 0x0001 },  // BoSM00 10 <- BoS302B 0
		{ 0, 0x142EDC, 10, 0, 0x142EDC, 0x0001 },  // BoSM00 10 <- BoSM00 0
		{ 0, 0x142EDC, 10, 0, 0x142FD3, 0x42100002 },  // BoSM00 10 <- RR304 1, 20, 25, 30
		{ 0, 0x142EDC, 10, 0, 0x173ED9, 0x0007 },  // BoSM00 10 <- BoS304 0, 1, 2
		{ 0, 0x142EDC, 10, 0, 0x174029, 0x01FF },  // BoSM00 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x142EDC, 11, 0, 0x043275, 0x0002 },  // BoSM00 11 <- RR303 1
		{ 0, 0x142EDC, 11, 0, 0x06D646, 0x0154 },  // BoSM00 11 <- DN084 2, 4, 6, 8
		{ 0, 0x142EDC, 11, 0, 0x142FD3, 0x42100002 },  // BoSM00 11 <- RR304 1, 20, 25, 30
		{ 0, 0x142EDC, 11, 0, 0x174029, 0x01FF },  // BoSM00 11 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x142EDC, 20, 0, 0x0ADCE5, 0x001F },  // BoSM00 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x142EDC, 255, 0, 0x043275, 0x0002 },  // BoSM00 255 <- RR303 1
		{ 0, 0x142EDC, 255, 0, 0x06D646, 0x0154 },  // BoSM00 255 <- DN084 2, 4, 6, 8
		{ 0, 0x142EDC, 255, 0, 0x142FD3, 0x42100002 },  // BoSM00 255 <- RR304 1, 20, 25, 30
		{ 0, 0x142EDC, 255, 0, 0x174029, 0x01FF },  // BoSM00 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x142FD3, 0, 0, 0x043275, 0x0006 },  // RR304 0 <- RR303 1, 2
		{ 0, 0x142FD3, 0, 0, 0x142FD3, 0x4E100006 },  // RR304 0 <- RR304 1, 2, 20, 25, 26, 27, 30
		{ 0, 0x142FD3, 100, 0, 0x043275, 0x0002 },  // RR304 100 <- RR303 1
		{ 0, 0x142FD3, 100, 0, 0x142FD3, 0x42100002 },  // RR304 100 <- RR304 1, 20, 25, 30
		{ 0, 0x142FD3, 200, 0, 0x142FD3, 0x2100000 },  // RR304 200 <- RR304 20, 25
		{ 0, 0x142FD3, 300, 0, 0x142FD3, 0x2100000 },  // RR304 300 <- RR304 20, 25
		{ 0, 0x142FD3, 400, 0, 0x142FD3, 0x2100000 },  // RR304 400 <- RR304 20, 25
		{ 0, 0x142FD3, 500, 0, 0x142FD3, 0x2100000 },  // RR304 500 <- RR304 20, 25
		{ 0, 0x142FD3, 600, 0, 0x142FD3, 0x2100000 },  // RR304 600 <- RR304 20, 25
		{ 0, 0x142FD3, 700, 0, 0x142FD3, 0x2100000 },  // RR304 700 <- RR304 20, 25
		{ 0, 0x142FD3, 800, 0, 0x142FD3, 0x42100000 },  // RR304 800 <- RR304 20, 25, 30
		{ 0, 0x142FD3, 900, 0, 0x142FD3, 0x2000000 },  // RR304 900 <- RR304 25
		{ 0, 0x142FD3, 1200, 0, 0x142FD3, 0x40000000 },  // RR304 1200 <- RR304 30
		{ 0, 0x142FD3, 1400, 0, 0x043275, 0x0004 },  // RR304 1400 <- RR303 2
		{ 0, 0x145528, 10, 0, 0x0229EE, 0x0004 },  // BoSM01DanseOverrides 10 <- MQ302 2
		{ 0, 0x145528, 10, 0, 0x043275, 0x0002 },  // BoSM01DanseOverrides 10 <- RR303 1
		{ 0, 0x145528, 10, 0, 0x04402C, 0x03FF },  // BoSM01DanseOverrides 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x145528, 10, 0, 0x06D646, 0x0154 },  // BoSM01DanseOverrides 10 <- DN084 2, 4, 6, 8
		{ 0, 0x145528, 10, 0, 0x0ADCE5, 0x001F },  // BoSM01DanseOverrides 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x145528, 10, 0, 0x0B1D79, 0x0003 },  // BoSM01DanseOverrides 10 <- BoSM01 0, 1
		{ 0, 0x145528, 10, 0, 0x0B9F9D, 0x0007 },  // BoSM01DanseOverrides 10 <- BoS302 0, 1, 2
		{ 0, 0x145528, 10, 0, 0x0FDC8C, 0x000F },  // BoSM01DanseOverrides 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x145528, 10, 0, 0x107A1C, 0x0001 },  // BoSM01DanseOverrides 10 <- BoS302B 0
		{ 0, 0x145528, 10, 0, 0x142EDC, 0x0001 },  // BoSM01DanseOverrides 10 <- BoSM00 0
		{ 0, 0x145528, 10, 0, 0x142FD3, 0x42100002 },  // BoSM01DanseOverrides 10 <- RR304 1, 20, 25, 30
		{ 0, 0x145528, 10, 0, 0x173ED9, 0x0007 },  // BoSM01DanseOverrides 10 <- BoS304 0, 1, 2
		{ 0, 0x145528, 10, 0, 0x174029, 0x01FF },  // BoSM01DanseOverrides 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x145528, 10, 2, 0x000801, 0x0001 },  // BoSM01DanseOverrides 10 <- DLC01MQ02 0
		{ 0, 0x145528, 10, 2, 0x0010F5, 0x0001 },  // BoSM01DanseOverrides 10 <- DLC01MQ05 0
		{ 0, 0x145528, 10, 2, 0x002833, 0x000F },  // BoSM01DanseOverrides 10 <- DLC01MQ04 0, 1, 2, 3
		{ 0, 0x15056E, 10, 0, 0x0C9B0B, 0x1004000000000000 },  // Inst308Post 10 <- MQ00 50, 60
		{ 0, 0x152BFD, 40, 0, 0x0C9B0B, 0x1004000000000000 },  // DialogueInstituteShaunKid 40 <- MQ00 50, 60
		{ 0, 0x152BFD, 40, 0, 0x0DFB3C, 0x0001 },  // DialogueInstituteShaunKid 40 <- MinDestBoS 0
		{ 0, 0x154AD1, 10, 0, 0x0229EE, 0x000A },  // MQ206OptionalObjectives 10 <- MQ302 1, 3
		{ 0, 0x15BD39, 5, 0, 0x0229EE, 0x0004 },  // InstMassFusion 5 <- MQ302 2
		{ 0, 0x15BD39, 5, 0, 0x06D646, 0x03FE },  // InstMassFusion 5 <- DN084 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 5, 0, 0x0DDFDB, 0x0003 },  // InstMassFusion 5 <- Min301 0, 1
		{ 0, 0x15BD39, 5, 0, 0x0DFB3C, 0x0001 },  // InstMassFusion 5 <- MinDestBoS 0
		{ 0, 0x15BD39, 5, 0, 0x0FDC8C, 0x000E },  // InstMassFusion 5 <- BoS303 1, 2, 3
		{ 0, 0x15BD39, 10, 0, 0x0229EE, 0x0004 },  // InstMassFusion 10 <- MQ302 2
		{ 0, 0x15BD39, 10, 0, 0x06D646, 0x03FE },  // InstMassFusion 10 <- DN084 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 10, 0, 0x0DDFDB, 0x0003 },  // InstMassFusion 10 <- Min301 0, 1
		{ 0, 0x15BD39, 10, 0, 0x0DFB3C, 0x0001 },  // InstMassFusion 10 <- MinDestBoS 0
		{ 0, 0x15BD39, 10, 0, 0x0FDC8C, 0x000E },  // InstMassFusion 10 <- BoS303 1, 2, 3
		{ 0, 0x15BD39, 15, 0, 0x06D646, 0x0154 },  // InstMassFusion 15 <- DN084 2, 4, 6, 8
		{ 0, 0x15BD39, 20, 0, 0x06D646, 0x0154 },  // InstMassFusion 20 <- DN084 2, 4, 6, 8
		{ 0, 0x15BD39, 30, 0, 0x06D646, 0x0004 },  // InstMassFusion 30 <- DN084 2
		{ 0, 0x15BD39, 40, 0, 0x06D646, 0x03FC },  // InstMassFusion 40 <- DN084 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 40, 0, 0x0FDC8C, 0x0004 },  // InstMassFusion 40 <- BoS303 2
		{ 0, 0x15BD39, 50, 0, 0x06D646, 0x03F8 },  // InstMassFusion 50 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 50, 0, 0x0FDC8C, 0x0004 },  // InstMassFusion 50 <- BoS303 2
		{ 0, 0x15BD39, 60, 0, 0x06D646, 0x03F8 },  // InstMassFusion 60 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 60, 0, 0x0FDC8C, 0x0004 },  // InstMassFusion 60 <- BoS303 2
		{ 0, 0x15BD39, 70, 0, 0x06D646, 0x03F8 },  // InstMassFusion 70 <- DN084 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x15BD39, 70, 0, 0x0FDC8C, 0x0004 },  // InstMassFusion 70 <- BoS303 2
		{ 0, 0x15BD39, 80, 0, 0x06D646, 0x0140 },  // InstMassFusion 80 <- DN084 6, 8
		{ 0, 0x15BD39, 90, 0, 0x06D646, 0x0150 },  // InstMassFusion 90 <- DN084 4, 6, 8
		{ 0, 0x15BD39, 100, 0, 0x06D646, 0x01E0 },  // InstMassFusion 100 <- DN084 5, 6, 7, 8
		{ 0, 0x15BD39, 120, 0, 0x06D646, 0x0180 },  // InstMassFusion 120 <- DN084 7, 8
		{ 0, 0x15BD39, 130, 0, 0x06D646, 0x0100 },  // InstMassFusion 130 <- DN084 8
		{ 0, 0x15BD39, 140, 0, 0x06D646, 0x0100 },  // InstMassFusion 140 <- DN084 8
		{ 0, 0x15BD39, 150, 0, 0x06D646, 0x0180 },  // InstMassFusion 150 <- DN084 7, 8
		{ 0, 0x15BD39, 160, 0, 0x06D646, 0x0180 },  // InstMassFusion 160 <- DN084 7, 8
		{ 0, 0x15BD39, 170, 0, 0x06D646, 0x0100 },  // InstMassFusion 170 <- DN084 8
		{ 0, 0x15BD39, 1500, 0, 0x0229EE, 0x0004 },  // InstMassFusion 1500 <- MQ302 2
		{ 0, 0x15BD39, 1500, 0, 0x06D646, 0x02AA },  // InstMassFusion 1500 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x15BD39, 1500, 0, 0x0DDFDB, 0x0003 },  // InstMassFusion 1500 <- Min301 0, 1
		{ 0, 0x15BD39, 1500, 0, 0x0DFB3C, 0x0001 },  // InstMassFusion 1500 <- MinDestBoS 0
		{ 0, 0x15BD39, 1500, 0, 0x0FDC8C, 0x0006 },  // InstMassFusion 1500 <- BoS303 1, 2
		{ 0, 0x16426D, 10, 0, 0x0229EE, 0x0004 },  // BoSKickOutSoft 10 <- MQ302 2
		{ 0, 0x16426D, 255, 0, 0x0229EE, 0x0004 },  // BoSKickOutSoft 255 <- MQ302 2
		{ 0, 0x1666C5, 10, 0, 0x1666C5, 0x0001 },  // COMCaitQuest 10 <- COMCaitQuest 0
		{ 0, 0x16C207, 50, 0, 0x0229EE, 0x0008 },  // RRKickOut 50 <- MQ302 3
		{ 0, 0x16C207, 50, 0, 0x043275, 0x0002 },  // RRKickOut 50 <- RR303 1
		{ 0, 0x16C207, 50, 0, 0x07CC61, 0x0400 },  // RRKickOut 50 <- RR301 10
		{ 0, 0x16C207, 50, 0, 0x142FD3, 0x42100002 },  // RRKickOut 50 <- RR304 1, 20, 25, 30
		{ 0, 0x16C207, 300, 0, 0x0DFB3C, 0x0001 },  // RRKickOut 300 <- MinDestBoS 0
		{ 0, 0x16D036, 0, 0, 0x0229EE, 0x0004 },  // InstKickOut 0 <- MQ302 2
		{ 0, 0x16D036, 0, 0, 0x06D646, 0x02AA },  // InstKickOut 0 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x16D036, 0, 0, 0x0DDFDB, 0x0003 },  // InstKickOut 0 <- Min301 0, 1
		{ 0, 0x16D036, 0, 0, 0x0DFB3C, 0x0001 },  // InstKickOut 0 <- MinDestBoS 0
		{ 0, 0x16D036, 0, 0, 0x0FDC8C, 0x0006 },  // InstKickOut 0 <- BoS303 1, 2
		{ 0, 0x16D036, 10, 0, 0x0229EE, 0x0004 },  // InstKickOut 10 <- MQ302 2
		{ 0, 0x16D036, 10, 0, 0x06D646, 0x02AA },  // InstKickOut 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x16D036, 10, 0, 0x0DDFDB, 0x0003 },  // InstKickOut 10 <- Min301 0, 1
		{ 0, 0x16D036, 10, 0, 0x0DFB3C, 0x0001 },  // InstKickOut 10 <- MinDestBoS 0
		{ 0, 0x16D036, 10, 0, 0x0FDC8C, 0x0006 },  // InstKickOut 10 <- BoS303 1, 2
		{ 0, 0x16D036, 20, 0, 0x0229EE, 0x0004 },  // InstKickOut 20 <- MQ302 2
		{ 0, 0x16D036, 20, 0, 0x06D646, 0x02AA },  // InstKickOut 20 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x16D036, 20, 0, 0x0DDFDB, 0x0003 },  // InstKickOut 20 <- Min301 0, 1
		{ 0, 0x16D036, 20, 0, 0x0DFB3C, 0x0001 },  // InstKickOut 20 <- MinDestBoS 0
		{ 0, 0x16D036, 20, 0, 0x0FDC8C, 0x0006 },  // InstKickOut 20 <- BoS303 1, 2
		{ 0, 0x16D036, 200, 0, 0x0229EE, 0x0004 },  // InstKickOut 200 <- MQ302 2
		{ 0, 0x16D036, 200, 0, 0x0DDFDB, 0x0003 },  // InstKickOut 200 <- Min301 0, 1
		{ 0, 0x16D036, 200, 0, 0x0DFB3C, 0x0001 },  // InstKickOut 200 <- MinDestBoS 0
		{ 0, 0x16FF04, 10, 0, 0x0229EE, 0x0008 },  // BoSKickOut 10 <- MQ302 3
		{ 0, 0x16FF04, 10, 0, 0x02BF21, 0x001E },  // BoSKickOut 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x16FF04, 10, 0, 0x04402C, 0x03FF },  // BoSKickOut 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x16FF04, 10, 0, 0x0537FC, 0x0001 },  // BoSKickOut 10 <- BoS201B 0
		{ 0, 0x16FF04, 10, 0, 0x0537FF, 0x0006 },  // BoSKickOut 10 <- BoS202 1, 2
		{ 0, 0x16FF04, 10, 0, 0x06D646, 0x02AA },  // BoSKickOut 10 <- DN084 1, 3, 5, 7, 9
		{ 0, 0x16FF04, 10, 0, 0x0ADCE5, 0x001F },  // BoSKickOut 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x16FF04, 10, 0, 0x0B1D79, 0x0003 },  // BoSKickOut 10 <- BoSM01 0, 1
		{ 0, 0x16FF04, 10, 0, 0x0B9F9D, 0x0007 },  // BoSKickOut 10 <- BoS302 0, 1, 2
		{ 0, 0x16FF04, 10, 0, 0x0FDC8C, 0x000F },  // BoSKickOut 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x16FF04, 10, 0, 0x107A1C, 0x0001 },  // BoSKickOut 10 <- BoS302B 0
		{ 0, 0x16FF04, 10, 0, 0x10C64C, 0x0400 },  // BoSKickOut 10 <- MQ302RR 10
		{ 0, 0x16FF04, 10, 0, 0x142EDC, 0x0001 },  // BoSKickOut 10 <- BoSM00 0
		{ 0, 0x16FF04, 10, 0, 0x173ED9, 0x0007 },  // BoSKickOut 10 <- BoS304 0, 1, 2
		{ 0, 0x16FF04, 20, 0, 0x043275, 0x0002 },  // BoSKickOut 20 <- RR303 1
		{ 0, 0x16FF04, 20, 0, 0x06D646, 0x0004 },  // BoSKickOut 20 <- DN084 2
		{ 0, 0x16FF04, 20, 0, 0x142FD3, 0x42100002 },  // BoSKickOut 20 <- RR304 1, 20, 25, 30
		{ 0, 0x16FF04, 20, 0, 0x174029, 0x01FF },  // BoSKickOut 20 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x16FF04, 30, 0, 0x06D646, 0x0154 },  // BoSKickOut 30 <- DN084 2, 4, 6, 8
		{ 0, 0x16FF04, 255, 0, 0x043275, 0x0002 },  // BoSKickOut 255 <- RR303 1
		{ 0, 0x16FF04, 255, 0, 0x06D646, 0x0154 },  // BoSKickOut 255 <- DN084 2, 4, 6, 8
		{ 0, 0x16FF04, 255, 0, 0x142FD3, 0x42100002 },  // BoSKickOut 255 <- RR304 1, 20, 25, 30
		{ 0, 0x16FF04, 255, 0, 0x174029, 0x01FF },  // BoSKickOut 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x173ED9, 10, 0, 0x173ED9, 0x0007 },  // BoS304 10 <- BoS304 0, 1, 2
		{ 0, 0x173ED9, 20, 0, 0x0FDC8C, 0x000F },  // BoS304 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x173ED9, 20, 0, 0x173ED9, 0x0007 },  // BoS304 20 <- BoS304 0, 1, 2
		{ 0, 0x173ED9, 25, 0, 0x173ED9, 0x0006 },  // BoS304 25 <- BoS304 1, 2
		{ 0, 0x173ED9, 30, 0, 0x173ED9, 0x0006 },  // BoS304 30 <- BoS304 1, 2
		{ 0, 0x173ED9, 40, 0, 0x173ED9, 0x0004 },  // BoS304 40 <- BoS304 2
		{ 0, 0x173ED9, 45, 0, 0x173ED9, 0x0004 },  // BoS304 45 <- BoS304 2
		{ 0, 0x173ED9, 50, 0, 0x173ED9, 0x0004 },  // BoS304 50 <- BoS304 2
		{ 0, 0x173ED9, 55, 0, 0x173ED9, 0x0004 },  // BoS304 55 <- BoS304 2
		{ 0, 0x173ED9, 59, 0, 0x0229EE, 0x0004 },  // BoS304 59 <- MQ302 2
		{ 0, 0x173ED9, 59, 0, 0x043275, 0x0002 },  // BoS304 59 <- RR303 1
		{ 0, 0x173ED9, 59, 0, 0x06D646, 0x0154 },  // BoS304 59 <- DN084 2, 4, 6, 8
		{ 0, 0x173ED9, 59, 0, 0x142FD3, 0x42100002 },  // BoS304 59 <- RR304 1, 20, 25, 30
		{ 0, 0x173ED9, 59, 0, 0x174029, 0x01FF },  // BoS304 59 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x173ED9, 355, 0, 0x0229EE, 0x0004 },  // BoS304 355 <- MQ302 2
		{ 0, 0x173ED9, 355, 0, 0x043275, 0x0002 },  // BoS304 355 <- RR303 1
		{ 0, 0x173ED9, 355, 0, 0x06D646, 0x0154 },  // BoS304 355 <- DN084 2, 4, 6, 8
		{ 0, 0x173ED9, 355, 0, 0x142FD3, 0x42100002 },  // BoS304 355 <- RR304 1, 20, 25, 30
		{ 0, 0x173ED9, 355, 0, 0x174029, 0x01FF },  // BoS304 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x174029, 10, 0, 0x174029, 0x01FF },  // Inst307Fight 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x174029, 100, 0, 0x174029, 0x00C0 },  // Inst307Fight 100 <- Inst307Fight 6, 7
		{ 0, 0x174029, 110, 0, 0x174029, 0x00C0 },  // Inst307Fight 110 <- Inst307Fight 6, 7
		{ 0, 0x174029, 120, 0, 0x174029, 0x00C0 },  // Inst307Fight 120 <- Inst307Fight 6, 7
		{ 0, 0x174029, 130, 0, 0x174029, 0x00C0 },  // Inst307Fight 130 <- Inst307Fight 6, 7
		{ 0, 0x174029, 200, 0, 0x174029, 0x00C0 },  // Inst307Fight 200 <- Inst307Fight 6, 7
		{ 0, 0x174029, 250, 0, 0x174029, 0x01FF },  // Inst307Fight 250 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x175F1D, 10, 0, 0x174029, 0x01FF },  // Inst307FightVertibirds 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x175F1D, 20, 0, 0x174029, 0x01FF },  // Inst307FightVertibirds 20 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x175F1D, 29, 0, 0x174029, 0x01FF },  // Inst307FightVertibirds 29 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x175F1D, 30, 0, 0x174029, 0x01FF },  // Inst307FightVertibirds 30 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x175F1D, 39, 0, 0x174029, 0x00C0 },  // Inst307FightVertibirds 39 <- Inst307Fight 6, 7
		{ 0, 0x175F1D, 50, 0, 0x174029, 0x00C0 },  // Inst307FightVertibirds 50 <- Inst307Fight 6, 7
		{ 0, 0x175F1D, 70, 0, 0x174029, 0x00C0 },  // Inst307FightVertibirds 70 <- Inst307Fight 6, 7
		{ 0, 0x17BEC6, 10, 0, 0x0FDC8C, 0x000F },  // BoS304Vertibird 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x17BEC6, 10, 0, 0x173ED9, 0x0007 },  // BoS304Vertibird 10 <- BoS304 0, 1, 2
		{ 0, 0x17BEC6, 20, 0, 0x173ED9, 0x0006 },  // BoS304Vertibird 20 <- BoS304 1, 2
		{ 0, 0x17BEC6, 30, 0, 0x173ED9, 0x0004 },  // BoS304Vertibird 30 <- BoS304 2
		{ 0, 0x17BEC6, 255, 0, 0x0229EE, 0x0004 },  // BoS304Vertibird 255 <- MQ302 2
		{ 0, 0x1826BF, 255, 0, 0x02BF20, 0x0001 },  // BoS000 255 <- BoS200 0
		{ 0, 0x1826BF, 255, 0, 0x02BF21, 0x001E },  // BoS000 255 <- BoS201 1, 2, 3, 4
		{ 0, 0x1826BF, 255, 0, 0x04402C, 0x03FF },  // BoS000 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x1826BF, 255, 0, 0x0537FC, 0x0001 },  // BoS000 255 <- BoS201B 0
		{ 0, 0x1826BF, 255, 0, 0x0537FF, 0x0006 },  // BoS000 255 <- BoS202 1, 2
		{ 0, 0x1826BF, 255, 0, 0x05DDAB, 0x0001 },  // BoS000 255 <- BoS100 0
		{ 0, 0x1826BF, 255, 0, 0x06F5C1, 0x0007 },  // BoS000 255 <- BoS101 0, 1, 2
		{ 0, 0x1826BF, 255, 0, 0x0ADCE5, 0x001F },  // BoS000 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1826BF, 255, 0, 0x0B1D79, 0x0003 },  // BoS000 255 <- BoSM01 0, 1
		{ 0, 0x1826BF, 255, 0, 0x0B9F9D, 0x0007 },  // BoS000 255 <- BoS302 0, 1, 2
		{ 0, 0x1826BF, 255, 0, 0x0FDC8C, 0x000F },  // BoS000 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x1826BF, 255, 0, 0x107A1C, 0x0001 },  // BoS000 255 <- BoS302B 0
		{ 0, 0x1826BF, 255, 0, 0x142EDC, 0x0001 },  // BoS000 255 <- BoSM00 0
		{ 0, 0x1826BF, 255, 0, 0x173ED9, 0x0007 },  // BoS000 255 <- BoS304 0, 1, 2
		{ 0, 0x1826BF, 355, 0, 0x0229EE, 0x0004 },  // BoS000 355 <- MQ302 2
		{ 0, 0x1826BF, 355, 0, 0x043275, 0x0002 },  // BoS000 355 <- RR303 1
		{ 0, 0x1826BF, 355, 0, 0x06D646, 0x0154 },  // BoS000 355 <- DN084 2, 4, 6, 8
		{ 0, 0x1826BF, 355, 0, 0x142FD3, 0x42100002 },  // BoS000 355 <- RR304 1, 20, 25, 30
		{ 0, 0x1826BF, 355, 0, 0x174029, 0x01FF },  // BoS000 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x182EAE, 10, 0, 0x182EAE, 0x0001 },  // BoS305 10 <- BoS305 0
		{ 0, 0x182EAE, 355, 0, 0x043275, 0x0002 },  // BoS305 355 <- RR303 1
		{ 0, 0x182EAE, 355, 0, 0x06D646, 0x0154 },  // BoS305 355 <- DN084 2, 4, 6, 8
		{ 0, 0x182EAE, 355, 0, 0x142FD3, 0x42100002 },  // BoS305 355 <- RR304 1, 20, 25, 30
		{ 0, 0x182EAE, 355, 0, 0x174029, 0x01FF },  // BoS305 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1851A0, 10, 0, 0x02BF21, 0x001E },  // BoSEnable 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x1851A0, 10, 0, 0x04402C, 0x03FF },  // BoSEnable 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x1851A0, 10, 0, 0x0537FC, 0x0001 },  // BoSEnable 10 <- BoS201B 0
		{ 0, 0x1851A0, 10, 0, 0x0537FF, 0x0006 },  // BoSEnable 10 <- BoS202 1, 2
		{ 0, 0x1851A0, 10, 0, 0x0ADCE5, 0x001F },  // BoSEnable 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1851A0, 10, 0, 0x0B1D79, 0x0002 },  // BoSEnable 10 <- BoSM01 1
		{ 0, 0x1851A0, 10, 0, 0x0B9F9D, 0x0007 },  // BoSEnable 10 <- BoS302 0, 1, 2
		{ 0, 0x1851A0, 10, 0, 0x0FDC8C, 0x000F },  // BoSEnable 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x1851A0, 10, 0, 0x107A1C, 0x0001 },  // BoSEnable 10 <- BoS302B 0
		{ 0, 0x1851A0, 10, 0, 0x142EDC, 0x0001 },  // BoSEnable 10 <- BoSM00 0
		{ 0, 0x1851A0, 10, 0, 0x142FD3, 0x42100002 },  // BoSEnable 10 <- RR304 1, 20, 25, 30
		{ 0, 0x1851A0, 10, 0, 0x173ED9, 0x0007 },  // BoSEnable 10 <- BoS304 0, 1, 2
		{ 0, 0x1851A0, 10, 0, 0x174029, 0x01FF },  // BoSEnable 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1851A0, 10, 0, 0x182EAE, 0x0001 },  // BoSEnable 10 <- BoS305 0
		{ 0, 0x1851A0, 15, 0, 0x02BF21, 0x001E },  // BoSEnable 15 <- BoS201 1, 2, 3, 4
		{ 0, 0x1851A0, 15, 0, 0x04402C, 0x03FF },  // BoSEnable 15 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x1851A0, 15, 0, 0x0537FC, 0x0001 },  // BoSEnable 15 <- BoS201B 0
		{ 0, 0x1851A0, 15, 0, 0x0537FF, 0x0006 },  // BoSEnable 15 <- BoS202 1, 2
		{ 0, 0x1851A0, 15, 0, 0x0ADCE5, 0x001F },  // BoSEnable 15 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1851A0, 15, 0, 0x0B1D79, 0x0002 },  // BoSEnable 15 <- BoSM01 1
		{ 0, 0x1851A0, 15, 0, 0x0B9F9D, 0x0007 },  // BoSEnable 15 <- BoS302 0, 1, 2
		{ 0, 0x1851A0, 15, 0, 0x0FDC8C, 0x000F },  // BoSEnable 15 <- BoS303 0, 1, 2, 3
		{ 0, 0x1851A0, 15, 0, 0x107A1C, 0x0001 },  // BoSEnable 15 <- BoS302B 0
		{ 0, 0x1851A0, 15, 0, 0x142EDC, 0x0001 },  // BoSEnable 15 <- BoSM00 0
		{ 0, 0x1851A0, 15, 0, 0x142FD3, 0x42100002 },  // BoSEnable 15 <- RR304 1, 20, 25, 30
		{ 0, 0x1851A0, 15, 0, 0x173ED9, 0x0007 },  // BoSEnable 15 <- BoS304 0, 1, 2
		{ 0, 0x1851A0, 15, 0, 0x174029, 0x01FF },  // BoSEnable 15 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1851A0, 15, 0, 0x182EAE, 0x0001 },  // BoSEnable 15 <- BoS305 0
		{ 0, 0x1851A0, 20, 0, 0x0ADCE5, 0x001F },  // BoSEnable 20 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1851A0, 20, 0, 0x0B9F9D, 0x0007 },  // BoSEnable 20 <- BoS302 0, 1, 2
		{ 0, 0x1851A0, 20, 0, 0x0FDC8C, 0x000F },  // BoSEnable 20 <- BoS303 0, 1, 2, 3
		{ 0, 0x1851A0, 20, 0, 0x107A1C, 0x0001 },  // BoSEnable 20 <- BoS302B 0
		{ 0, 0x1851A0, 20, 0, 0x173ED9, 0x0007 },  // BoSEnable 20 <- BoS304 0, 1, 2
		{ 0, 0x1851A0, 20, 0, 0x174029, 0x01FF },  // BoSEnable 20 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1851A0, 20, 0, 0x182EAE, 0x0001 },  // BoSEnable 20 <- BoS305 0
		{ 0, 0x1851A0, 30, 0, 0x0B9F9D, 0x0007 },  // BoSEnable 30 <- BoS302 0, 1, 2
		{ 0, 0x1851A0, 30, 0, 0x0FDC8C, 0x000F },  // BoSEnable 30 <- BoS303 0, 1, 2, 3
		{ 0, 0x1851A0, 30, 0, 0x173ED9, 0x0007 },  // BoSEnable 30 <- BoS304 0, 1, 2
		{ 0, 0x1851A0, 30, 0, 0x174029, 0x01FF },  // BoSEnable 30 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1851A0, 30, 0, 0x182EAE, 0x0001 },  // BoSEnable 30 <- BoS305 0
		{ 0, 0x1851A0, 40, 0, 0x0B9F9D, 0x0006 },  // BoSEnable 40 <- BoS302 1, 2
		{ 0, 0x1851A0, 40, 0, 0x0FDC8C, 0x000F },  // BoSEnable 40 <- BoS303 0, 1, 2, 3
		{ 0, 0x1851A0, 40, 0, 0x173ED9, 0x0007 },  // BoSEnable 40 <- BoS304 0, 1, 2
		{ 0, 0x1851A0, 40, 0, 0x182EAE, 0x0001 },  // BoSEnable 40 <- BoS305 0
		{ 0, 0x1A001C, 35, 0, 0x0229EE, 0x0004 },  // Min00 35 <- MQ302 2
		{ 0, 0x1A001C, 35, 0, 0x0DDFDB, 0x0003 },  // Min00 35 <- Min301 0, 1
		{ 0, 0x1A001C, 35, 0, 0x0DFB3C, 0x0001 },  // Min00 35 <- MinDestBoS 0
		{ 0, 0x1A001C, 45, 0, 0x0229EE, 0x0004 },  // Min00 45 <- MQ302 2
		{ 0, 0x1A001C, 45, 0, 0x0DDFDB, 0x0003 },  // Min00 45 <- Min301 0, 1
		{ 0, 0x1A001C, 45, 0, 0x0DFB3C, 0x0001 },  // Min00 45 <- MinDestBoS 0
		{ 0, 0x1A001C, 47, 0, 0x0229EE, 0x0004 },  // Min00 47 <- MQ302 2
		{ 0, 0x1A001C, 47, 0, 0x0DDFDB, 0x0003 },  // Min00 47 <- Min301 0, 1
		{ 0, 0x1A001C, 47, 0, 0x0DFB3C, 0x0001 },  // Min00 47 <- MinDestBoS 0
		{ 0, 0x1A001C, 50, 0, 0x0229EE, 0x0004 },  // Min00 50 <- MQ302 2
		{ 0, 0x1A001C, 50, 0, 0x0DDFDB, 0x0003 },  // Min00 50 <- Min301 0, 1
		{ 0, 0x1A001C, 50, 0, 0x0DFB3C, 0x0001 },  // Min00 50 <- MinDestBoS 0
		{ 0, 0x1A001C, 80, 0, 0x0229EE, 0x0004 },  // Min00 80 <- MQ302 2
		{ 0, 0x1A001C, 80, 0, 0x0DDFDB, 0x0003 },  // Min00 80 <- Min301 0, 1
		{ 0, 0x1A001C, 80, 0, 0x0DFB3C, 0x0001 },  // Min00 80 <- MinDestBoS 0
		{ 0, 0x1A001C, 90, 0, 0x0229EE, 0x0004 },  // Min00 90 <- MQ302 2
		{ 0, 0x1A001C, 90, 0, 0x0DDFDB, 0x0003 },  // Min00 90 <- Min301 0, 1
		{ 0, 0x1A001C, 90, 0, 0x0DFB3C, 0x0001 },  // Min00 90 <- MinDestBoS 0
		{ 0, 0x1A001C, 200, 0, 0x0229EE, 0x0004 },  // Min00 200 <- MQ302 2
		{ 0, 0x1A001C, 200, 0, 0x0DDFDB, 0x0003 },  // Min00 200 <- Min301 0, 1
		{ 0, 0x1A001C, 200, 0, 0x0DFB3C, 0x0001 },  // Min00 200 <- MinDestBoS 0
		{ 0, 0x1A7BD2, 10, 0, 0x0229EE, 0x0004 },  // BoS101POI 10 <- MQ302 2
		{ 0, 0x1A7BD2, 10, 0, 0x043275, 0x0002 },  // BoS101POI 10 <- RR303 1
		{ 0, 0x1A7BD2, 10, 0, 0x06D646, 0x0154 },  // BoS101POI 10 <- DN084 2, 4, 6, 8
		{ 0, 0x1A7BD2, 10, 0, 0x06F5C1, 0x0002 },  // BoS101POI 10 <- BoS101 1
		{ 0, 0x1A7BD2, 10, 0, 0x142FD3, 0x42100002 },  // BoS101POI 10 <- RR304 1, 20, 25, 30
		{ 0, 0x1A7BD2, 10, 0, 0x174029, 0x01FF },  // BoS101POI 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1A7BD2, 30, 0, 0x0229EE, 0x0004 },  // BoS101POI 30 <- MQ302 2
		{ 0, 0x1A7BD2, 30, 0, 0x043275, 0x0002 },  // BoS101POI 30 <- RR303 1
		{ 0, 0x1A7BD2, 30, 0, 0x06D646, 0x0154 },  // BoS101POI 30 <- DN084 2, 4, 6, 8
		{ 0, 0x1A7BD2, 30, 0, 0x142FD3, 0x42100002 },  // BoS101POI 30 <- RR304 1, 20, 25, 30
		{ 0, 0x1A7BD2, 30, 0, 0x174029, 0x01FF },  // BoS101POI 30 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1A7BD2, 255, 0, 0x0229EE, 0x0004 },  // BoS101POI 255 <- MQ302 2
		{ 0, 0x1A7BD2, 255, 0, 0x043275, 0x0002 },  // BoS101POI 255 <- RR303 1
		{ 0, 0x1A7BD2, 255, 0, 0x06D646, 0x0154 },  // BoS101POI 255 <- DN084 2, 4, 6, 8
		{ 0, 0x1A7BD2, 255, 0, 0x142FD3, 0x42100002 },  // BoS101POI 255 <- RR304 1, 20, 25, 30
		{ 0, 0x1A7BD2, 255, 0, 0x174029, 0x01FF },  // BoS101POI 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1B2B08, 355, 0, 0x043275, 0x0002 },  // BoSFFMaster 355 <- RR303 1
		{ 0, 0x1B2B08, 355, 0, 0x06D646, 0x0154 },  // BoSFFMaster 355 <- DN084 2, 4, 6, 8
		{ 0, 0x1B2B08, 355, 0, 0x142FD3, 0x42100002 },  // BoSFFMaster 355 <- RR304 1, 20, 25, 30
		{ 0, 0x1B2B08, 355, 0, 0x174029, 0x01FF },  // BoSFFMaster 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1BB216, 10, 0, 0x02BF21, 0x001E },  // DN165Exterior 10 <- BoS201 1, 2, 3, 4
		{ 0, 0x1BB216, 10, 0, 0x04402C, 0x03FF },  // DN165Exterior 10 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x1BB216, 10, 0, 0x0537FC, 0x0001 },  // DN165Exterior 10 <- BoS201B 0
		{ 0, 0x1BB216, 10, 0, 0x0537FF, 0x0006 },  // DN165Exterior 10 <- BoS202 1, 2
		{ 0, 0x1BB216, 10, 0, 0x0ADCE5, 0x001F },  // DN165Exterior 10 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1BB216, 10, 0, 0x0B1D79, 0x0002 },  // DN165Exterior 10 <- BoSM01 1
		{ 0, 0x1BB216, 10, 0, 0x0B9F9D, 0x0007 },  // DN165Exterior 10 <- BoS302 0, 1, 2
		{ 0, 0x1BB216, 10, 0, 0x0FDC8C, 0x000F },  // DN165Exterior 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x1BB216, 10, 0, 0x107A1C, 0x0001 },  // DN165Exterior 10 <- BoS302B 0
		{ 0, 0x1BB216, 10, 0, 0x142EDC, 0x0001 },  // DN165Exterior 10 <- BoSM00 0
		{ 0, 0x1BB216, 10, 0, 0x142FD3, 0x42100002 },  // DN165Exterior 10 <- RR304 1, 20, 25, 30
		{ 0, 0x1BB216, 10, 0, 0x173ED9, 0x0007 },  // DN165Exterior 10 <- BoS304 0, 1, 2
		{ 0, 0x1BB216, 10, 0, 0x174029, 0x01FF },  // DN165Exterior 10 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1BB216, 10, 0, 0x182EAE, 0x0001 },  // DN165Exterior 10 <- BoS305 0
		{ 0, 0x1BB216, 255, 0, 0x02BF21, 0x001E },  // DN165Exterior 255 <- BoS201 1, 2, 3, 4
		{ 0, 0x1BB216, 255, 0, 0x04402C, 0x03FF },  // DN165Exterior 255 <- BoSM02 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
		{ 0, 0x1BB216, 255, 0, 0x0537FC, 0x0001 },  // DN165Exterior 255 <- BoS201B 0
		{ 0, 0x1BB216, 255, 0, 0x0537FF, 0x0006 },  // DN165Exterior 255 <- BoS202 1, 2
		{ 0, 0x1BB216, 255, 0, 0x0ADCE5, 0x001F },  // DN165Exterior 255 <- BoSM04 0, 1, 2, 3, 4
		{ 0, 0x1BB216, 255, 0, 0x0B1D79, 0x0002 },  // DN165Exterior 255 <- BoSM01 1
		{ 0, 0x1BB216, 255, 0, 0x0B9F9D, 0x0007 },  // DN165Exterior 255 <- BoS302 0, 1, 2
		{ 0, 0x1BB216, 255, 0, 0x0FDC8C, 0x000F },  // DN165Exterior 255 <- BoS303 0, 1, 2, 3
		{ 0, 0x1BB216, 255, 0, 0x107A1C, 0x0001 },  // DN165Exterior 255 <- BoS302B 0
		{ 0, 0x1BB216, 255, 0, 0x142EDC, 0x0001 },  // DN165Exterior 255 <- BoSM00 0
		{ 0, 0x1BB216, 255, 0, 0x142FD3, 0x42100002 },  // DN165Exterior 255 <- RR304 1, 20, 25, 30
		{ 0, 0x1BB216, 255, 0, 0x173ED9, 0x0007 },  // DN165Exterior 255 <- BoS304 0, 1, 2
		{ 0, 0x1BB216, 255, 0, 0x174029, 0x01FF },  // DN165Exterior 255 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 0, 0x1BB216, 255, 0, 0x182EAE, 0x0001 },  // DN165Exterior 255 <- BoS305 0
		{ 0, 0x21784A, 10, 0, 0x0229EE, 0x0004 },  // BoSPostQuest 10 <- MQ302 2
		{ 0, 0x21784A, 10, 0, 0x182EAE, 0x0001 },  // BoSPostQuest 10 <- BoS305 0
		{ 0, 0x219B92, 10, 0, 0x219B92, 0x0001 },  // DN120Misc 10 <- DN120Misc 0
		{ 0, 0x2269A9, 10, 0, 0x02BF21, 0x0010 },  // BoSStoragePointer 10 <- BoS201 4
		{ 0, 0x2269A9, 10, 0, 0x0537FC, 0x0001 },  // BoSStoragePointer 10 <- BoS201B 0
		{ 0, 0x2269A9, 10, 0, 0x0537FF, 0x0006 },  // BoSStoragePointer 10 <- BoS202 1, 2
		{ 0, 0x2269A9, 10, 0, 0x0B9F9D, 0x0007 },  // BoSStoragePointer 10 <- BoS302 0, 1, 2
		{ 0, 0x2269A9, 10, 0, 0x0FDC8C, 0x000F },  // BoSStoragePointer 10 <- BoS303 0, 1, 2, 3
		{ 0, 0x2269A9, 10, 0, 0x107A1C, 0x0001 },  // BoSStoragePointer 10 <- BoS302B 0
		{ 0, 0x2269A9, 10, 0, 0x173ED9, 0x0007 },  // BoSStoragePointer 10 <- BoS304 0, 1, 2
		{ 0, 0x2269A9, 355, 0, 0x043275, 0x0002 },  // BoSStoragePointer 355 <- RR303 1
		{ 0, 0x2269A9, 355, 0, 0x06D646, 0x0154 },  // BoSStoragePointer 355 <- DN084 2, 4, 6, 8
		{ 0, 0x2269A9, 355, 0, 0x142FD3, 0x42100002 },  // BoSStoragePointer 355 <- RR304 1, 20, 25, 30
		{ 0, 0x2269A9, 355, 0, 0x174029, 0x01FF },  // BoSStoragePointer 355 <- Inst307Fight 0, 1, 2, 3, 4, 5, 6, 7, 8
		{ 1, 0x001B40, 1, 1, 0x043822, 0x0020 },  // DLC03MQ02 1 <- DLC03MQ02Fight 5
		{ 1, 0x001B40, 50, 1, 0x043822, 0x0020 },  // DLC03MQ02 50 <- DLC03MQ02Fight 5
		{ 1, 0x001B40, 100, 1, 0x043822, 0x0020 },  // DLC03MQ02 100 <- DLC03MQ02Fight 5
		{ 1, 0x001B40, 200, 1, 0x043822, 0x0020 },  // DLC03MQ02 200 <- DLC03MQ02Fight 5
		{ 1, 0x001B40, 300, 1, 0x00A990, 0x0001 },  // DLC03MQ02 300 <- DLC03FarHarborFF03a 0
		{ 1, 0x001B40, 700, 1, 0x001B44, 0x000E },  // DLC03MQ02 700 <- DLC03MQ06 1, 2, 3
		{ 1, 0x001B41, 300, 1, 0x001B42, 0x0002 },  // DLC03MQ03 300 <- DLC03MQ04 1
		{ 1, 0x001B41, 400, 1, 0x001B42, 0x0002 },  // DLC03MQ03 400 <- DLC03MQ04 1
		{ 1, 0x001B41, 500, 1, 0x001B42, 0x0002 },  // DLC03MQ03 500 <- DLC03MQ04 1
		{ 1, 0x001B41, 1000, 1, 0x001B42, 0x0002 },  // DLC03MQ03 1000 <- DLC03MQ04 1
		{ 1, 0x001B42, 10, 1, 0x001B42, 0x0002 },  // DLC03MQ04 10 <- DLC03MQ04 1
		{ 1, 0x001B42, 10, 1, 0x001B44, 0x000E },  // DLC03MQ04 10 <- DLC03MQ06 1, 2, 3
		{ 1, 0x001B42, 10, 1, 0x02127F, 0x0002 },  // DLC03MQ04 10 <- DLC03MQ06a 1
		{ 1, 0x001B42, 20, 1, 0x001B42, 0x0002 },  // DLC03MQ04 20 <- DLC03MQ04 1
		{ 1, 0x001B42, 160, 1, 0x02127F, 0x0002 },  // DLC03MQ04 160 <- DLC03MQ06a 1
		{ 1, 0x001B42, 180, 1, 0x02127F, 0x0002 },  // DLC03MQ04 180 <- DLC03MQ06a 1
		{ 1, 0x001B42, 260, 1, 0x001B44, 0x000A },  // DLC03MQ04 260 <- DLC03MQ06 1, 3
		{ 1, 0x001B42, 270, 1, 0x001B44, 0x000A },  // DLC03MQ04 270 <- DLC03MQ06 1, 3
		{ 1, 0x001B42, 360, 1, 0x001B44, 0x0006 },  // DLC03MQ04 360 <- DLC03MQ06 1, 2
		{ 1, 0x001B43, 0, 1, 0x02127F, 0x0002 },  // DLC03MQ05 0 <- DLC03MQ06a 1
		{ 1, 0x001B43, 1, 1, 0x02127F, 0x0002 },  // DLC03MQ05 1 <- DLC03MQ06a 1
		{ 1, 0x001B43, 50, 1, 0x02127F, 0x0002 },  // DLC03MQ05 50 <- DLC03MQ06a 1
		{ 1, 0x001B43, 250, 1, 0x02127F, 0x0002 },  // DLC03MQ05 250 <- DLC03MQ06a 1
		{ 1, 0x001B44, 10, 1, 0x001B44, 0x000A },  // DLC03MQ06 10 <- DLC03MQ06 1, 3
		{ 1, 0x001B44, 197, 1, 0x001B44, 0x0006 },  // DLC03MQ06 197 <- DLC03MQ06 1, 2
		{ 1, 0x004C8E, 50, 1, 0x02127F, 0x0002 },  // DLC03CoA_DialogueNucleus 50 <- DLC03MQ06a 1
		{ 1, 0x004C8E, 60, 1, 0x001B44, 0x000E },  // DLC03CoA_DialogueNucleus 60 <- DLC03MQ06 1, 2, 3
		{ 1, 0x004C8E, 60, 1, 0x02127F, 0x0002 },  // DLC03CoA_DialogueNucleus 60 <- DLC03MQ06a 1
		{ 1, 0x0052B7, 100, 1, 0x02127F, 0x0002 },  // DLC03AcadiaDialogue 100 <- DLC03MQ06a 1
		{ 1, 0x00A991, 1, 1, 0x00A991, 0x0001 },  // DLC03FarHarborS01 1 <- DLC03FarHarborS01 0
		{ 1, 0x00A991, 50, 1, 0x00A991, 0x0001 },  // DLC03FarHarborS01 50 <- DLC03FarHarborS01 0
		{ 1, 0x00AEC3, 1, 1, 0x00AEC3, 0x0001 },  // DLC03FarHarborS02 1 <- DLC03FarHarborS02 0
		{ 1, 0x00AEC3, 50, 1, 0x00AEC3, 0x0001 },  // DLC03FarHarborS02 50 <- DLC03FarHarborS02 0
		{ 1, 0x00AEC3, 200, 1, 0x00AEC3, 0x0001 },  // DLC03FarHarborS02 200 <- DLC03FarHarborS02 0
		{ 1, 0x00AFB1, 10, 1, 0x001B44, 0x000E },  // DLC03AtomM01 10 <- DLC03MQ06 1, 2, 3
		{ 1, 0x00AFB1, 10, 1, 0x00AFB1, 0x0001 },  // DLC03AtomM01 10 <- DLC03AtomM01 0
		{ 1, 0x00AFB1, 10, 1, 0x02127F, 0x0002 },  // DLC03AtomM01 10 <- DLC03MQ06a 1
		{ 1, 0x00AFB1, 55, 1, 0x001B44, 0x000E },  // DLC03AtomM01 55 <- DLC03MQ06 1, 2, 3
		{ 1, 0x00AFB1, 100, 1, 0x001B44, 0x000E },  // DLC03AtomM01 100 <- DLC03MQ06 1, 2, 3
		{ 1, 0x00AFB1, 150, 1, 0x001B44, 0x000E },  // DLC03AtomM01 150 <- DLC03MQ06 1, 2, 3
		{ 1, 0x00AFB1, 150, 1, 0x02127F, 0x0002 },  // DLC03AtomM01 150 <- DLC03MQ06a 1
		{ 1, 0x013F38, 10, 1, 0x001B42, 0x0002 },  // DLC03Acadia_Institute_Misc 10 <- DLC03MQ04 1
		{ 1, 0x013F38, 10, 1, 0x02127F, 0x0002 },  // DLC03Acadia_Institute_Misc 10 <- DLC03MQ06a 1
		{ 1, 0x013F39, 10, 1, 0x02127F, 0x0002 },  // DLC03Acadia_BoS 10 <- DLC03MQ06a 1
		{ 1, 0x013F39, 50, 1, 0x02127F, 0x0002 },  // DLC03Acadia_BoS 50 <- DLC03MQ06a 1
		{ 1, 0x013F39, 100, 1, 0x02127F, 0x0002 },  // DLC03Acadia_BoS 100 <- DLC03MQ06a 1
		{ 1, 0x013F3A, 1, 1, 0x001B42, 0x0002 },  // DLC03Acadia_Institute 1 <- DLC03MQ04 1
		{ 1, 0x013F3A, 1, 1, 0x02127F, 0x0002 },  // DLC03Acadia_Institute 1 <- DLC03MQ06a 1
		{ 1, 0x013F3A, 10, 1, 0x02127F, 0x0002 },  // DLC03Acadia_Institute 10 <- DLC03MQ06a 1
		{ 1, 0x013F3A, 50, 1, 0x02127F, 0x0002 },  // DLC03Acadia_Institute 50 <- DLC03MQ06a 1
		{ 1, 0x013F3A, 100, 1, 0x02127F, 0x0002 },  // DLC03Acadia_Institute 100 <- DLC03MQ06a 1
		{ 1, 0x0203CC, 1, 1, 0x001B42, 0x0002 },  // DLC03Acadia_RR 1 <- DLC03MQ04 1
		{ 1, 0x0203CC, 1, 1, 0x02127F, 0x0002 },  // DLC03Acadia_RR 1 <- DLC03MQ06a 1
		{ 1, 0x0203CC, 500, 1, 0x02127F, 0x0002 },  // DLC03Acadia_RR 500 <- DLC03MQ06a 1
		{ 1, 0x020BF1, 200, 1, 0x02127F, 0x0002 },  // DLC03DialogueDiMA 200 <- DLC03MQ06a 1
		{ 1, 0x0316F2, 502, 1, 0x04B129, 0x0001 },  // DLC03DialogueV118 502 <- DLC03_V118_Quest2 0
		{ 1, 0x0316F2, 1300, 1, 0x04B129, 0x0001 },  // DLC03DialogueV118 1300 <- DLC03_V118_Quest2 0
		{ 1, 0x036761, 7, 1, 0x00AEC3, 0x0001 },  // DLC03_V118_MiscObjectives 7 <- DLC03FarHarborS02 0
		{ 1, 0x036761, 7, 1, 0x02127F, 0x0002 },  // DLC03_V118_MiscObjectives 7 <- DLC03MQ06a 1
		{ 1, 0x0423AA, 10, 1, 0x001B42, 0x0002 },  // DLC03Acadia_BoS_Misc 10 <- DLC03MQ04 1
		{ 1, 0x0423AA, 10, 1, 0x02127F, 0x0002 },  // DLC03Acadia_BoS_Misc 10 <- DLC03MQ06a 1
		{ 1, 0x043822, 1, 1, 0x043822, 0x0020 },  // DLC03MQ02Fight 1 <- DLC03MQ02Fight 5
		{ 1, 0x043822, 10, 1, 0x043822, 0x0020 },  // DLC03MQ02Fight 10 <- DLC03MQ02Fight 5
		{ 1, 0x043822, 20, 1, 0x043822, 0x0020 },  // DLC03MQ02Fight 20 <- DLC03MQ02Fight 5
		{ 1, 0x043822, 90, 1, 0x043822, 0x0020 },  // DLC03MQ02Fight 90 <- DLC03MQ02Fight 5
		{ 1, 0x04A686, 10, 1, 0x02127F, 0x0002 },  // DLC03MQ05AcadiaStaging 10 <- DLC03MQ06a 1
		{ 1, 0x04E77A, 1, 1, 0x001B44, 0x000E },  // DLC03FarHarborM02 1 <- DLC03MQ06 1, 2, 3
		{ 1, 0x04E77A, 100, 1, 0x001B44, 0x000E },  // DLC03FarHarborM02 100 <- DLC03MQ06 1, 2, 3
		{ 1, 0x04E77A, 200, 1, 0x001B44, 0x000E },  // DLC03FarHarborM02 200 <- DLC03MQ06 1, 2, 3
		{ 1, 0x04E77A, 400, 1, 0x001B44, 0x000E },  // DLC03FarHarborM02 400 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 10, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 10 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 10, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 10 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 100, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 100 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 100, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 100 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 200, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 200 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 200, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 200 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 230, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 230 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 230, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 230 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 240, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 240 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 240, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 240 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 250, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 250 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 250, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 250 <- DLC03MQ06a 1
		{ 1, 0x0500E4, 600, 1, 0x001B44, 0x000E },  // DLC03CoA_QuestPointers 600 <- DLC03MQ06 1, 2, 3
		{ 1, 0x0500E4, 600, 1, 0x02127F, 0x0002 },  // DLC03CoA_QuestPointers 600 <- DLC03MQ06a 1
		{ 2, 0x000801, 10, 2, 0x000801, 0x0001 },  // DLC01MQ02 10 <- DLC01MQ02 0
		{ 2, 0x000801, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 10 <- DLC01MQ05 0
		{ 2, 0x000801, 10, 2, 0x002833, 0x000F },  // DLC01MQ02 10 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2100, 2, 0x000801, 0x0001 },  // DLC01MQ02 2100 <- DLC01MQ02 0
		{ 2, 0x000801, 2100, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2100 <- DLC01MQ05 0
		{ 2, 0x000801, 2100, 2, 0x002833, 0x000F },  // DLC01MQ02 2100 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2200, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2200 <- DLC01MQ05 0
		{ 2, 0x000801, 2200, 2, 0x002833, 0x000F },  // DLC01MQ02 2200 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2400, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2400 <- DLC01MQ05 0
		{ 2, 0x000801, 2400, 2, 0x002833, 0x000F },  // DLC01MQ02 2400 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2500, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2500 <- DLC01MQ05 0
		{ 2, 0x000801, 2500, 2, 0x002833, 0x000F },  // DLC01MQ02 2500 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2600, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2600 <- DLC01MQ05 0
		{ 2, 0x000801, 2600, 2, 0x002833, 0x000F },  // DLC01MQ02 2600 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2800, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2800 <- DLC01MQ05 0
		{ 2, 0x000801, 2800, 2, 0x002833, 0x000F },  // DLC01MQ02 2800 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2900, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2900 <- DLC01MQ05 0
		{ 2, 0x000801, 2900, 2, 0x002833, 0x000F },  // DLC01MQ02 2900 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 2950, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 2950 <- DLC01MQ05 0
		{ 2, 0x000801, 2950, 2, 0x002833, 0x000F },  // DLC01MQ02 2950 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000801, 3000, 2, 0x0010F5, 0x0001 },  // DLC01MQ02 3000 <- DLC01MQ05 0
		{ 2, 0x000801, 3000, 2, 0x002833, 0x000F },  // DLC01MQ02 3000 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 10, 2, 0x000801, 0x0001 },  // DLC01MQ01 10 <- DLC01MQ02 0
		{ 2, 0x000806, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 10 <- DLC01MQ05 0
		{ 2, 0x000806, 10, 2, 0x002833, 0x000F },  // DLC01MQ01 10 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 2100, 2, 0x000801, 0x0001 },  // DLC01MQ01 2100 <- DLC01MQ02 0
		{ 2, 0x000806, 2100, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 2100 <- DLC01MQ05 0
		{ 2, 0x000806, 2100, 2, 0x002833, 0x000F },  // DLC01MQ01 2100 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 2200, 2, 0x000801, 0x0001 },  // DLC01MQ01 2200 <- DLC01MQ02 0
		{ 2, 0x000806, 2200, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 2200 <- DLC01MQ05 0
		{ 2, 0x000806, 2200, 2, 0x002833, 0x000F },  // DLC01MQ01 2200 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 2300, 2, 0x000801, 0x0001 },  // DLC01MQ01 2300 <- DLC01MQ02 0
		{ 2, 0x000806, 2300, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 2300 <- DLC01MQ05 0
		{ 2, 0x000806, 2300, 2, 0x002833, 0x000F },  // DLC01MQ01 2300 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 2400, 2, 0x000801, 0x0001 },  // DLC01MQ01 2400 <- DLC01MQ02 0
		{ 2, 0x000806, 2400, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 2400 <- DLC01MQ05 0
		{ 2, 0x000806, 2400, 2, 0x002833, 0x000F },  // DLC01MQ01 2400 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 2430, 2, 0x000801, 0x0001 },  // DLC01MQ01 2430 <- DLC01MQ02 0
		{ 2, 0x000806, 2430, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 2430 <- DLC01MQ05 0
		{ 2, 0x000806, 2430, 2, 0x002833, 0x000F },  // DLC01MQ01 2430 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x000806, 3000, 2, 0x000801, 0x0001 },  // DLC01MQ01 3000 <- DLC01MQ02 0
		{ 2, 0x000806, 3000, 2, 0x0010F5, 0x0001 },  // DLC01MQ01 3000 <- DLC01MQ05 0
		{ 2, 0x000806, 3000, 2, 0x002833, 0x000F },  // DLC01MQ01 3000 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x0010F5, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ05 10 <- DLC01MQ05 0
		{ 2, 0x0010F5, 400, 2, 0x0010F5, 0x0001 },  // DLC01MQ05 400 <- DLC01MQ05 0
		{ 2, 0x0010F5, 500, 2, 0x0010F5, 0x0001 },  // DLC01MQ05 500 <- DLC01MQ05 0
		{ 2, 0x002833, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 10 <- DLC01MQ05 0
		{ 2, 0x002833, 10, 2, 0x002833, 0x000F },  // DLC01MQ04 10 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x002833, 20, 2, 0x002833, 0x000E },  // DLC01MQ04 20 <- DLC01MQ04 1, 2, 3
		{ 2, 0x002833, 50, 2, 0x002833, 0x000C },  // DLC01MQ04 50 <- DLC01MQ04 2, 3
		{ 2, 0x002833, 100, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 100 <- DLC01MQ05 0
		{ 2, 0x002833, 100, 2, 0x002833, 0x000E },  // DLC01MQ04 100 <- DLC01MQ04 1, 2, 3
		{ 2, 0x002833, 110, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 110 <- DLC01MQ05 0
		{ 2, 0x002833, 110, 2, 0x002833, 0x000C },  // DLC01MQ04 110 <- DLC01MQ04 2, 3
		{ 2, 0x002833, 120, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 120 <- DLC01MQ05 0
		{ 2, 0x002833, 120, 2, 0x002833, 0x000C },  // DLC01MQ04 120 <- DLC01MQ04 2, 3
		{ 2, 0x002833, 130, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 130 <- DLC01MQ05 0
		{ 2, 0x002833, 130, 2, 0x002833, 0x000C },  // DLC01MQ04 130 <- DLC01MQ04 2, 3
		{ 2, 0x002833, 140, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 140 <- DLC01MQ05 0
		{ 2, 0x002833, 140, 2, 0x002833, 0x0008 },  // DLC01MQ04 140 <- DLC01MQ04 3
		{ 2, 0x002833, 145, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 145 <- DLC01MQ05 0
		{ 2, 0x002833, 150, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 150 <- DLC01MQ05 0
		{ 2, 0x002833, 150, 2, 0x002833, 0x0008 },  // DLC01MQ04 150 <- DLC01MQ04 3
		{ 2, 0x002833, 160, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 160 <- DLC01MQ05 0
		{ 2, 0x002833, 160, 2, 0x002833, 0x0008 },  // DLC01MQ04 160 <- DLC01MQ04 3
		{ 2, 0x002833, 180, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 180 <- DLC01MQ05 0
		{ 2, 0x002833, 180, 2, 0x002833, 0x0008 },  // DLC01MQ04 180 <- DLC01MQ04 3
		{ 2, 0x002833, 190, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 190 <- DLC01MQ05 0
		{ 2, 0x002833, 190, 2, 0x002833, 0x0008 },  // DLC01MQ04 190 <- DLC01MQ04 3
		{ 2, 0x002833, 200, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 200 <- DLC01MQ05 0
		{ 2, 0x002833, 255, 2, 0x0010F5, 0x0001 },  // DLC01MQ04 255 <- DLC01MQ05 0
		{ 2, 0x00A162, 0, 2, 0x0010F5, 0x0001 },  // DLC01FortHagenSatelliteArrayQuest 0 <- DLC01MQ05 0
		{ 2, 0x00A162, 0, 2, 0x002833, 0x000C },  // DLC01FortHagenSatelliteArrayQuest 0 <- DLC01MQ04 2, 3
		{ 2, 0x00A162, 100, 2, 0x0010F5, 0x0001 },  // DLC01FortHagenSatelliteArrayQuest 100 <- DLC01MQ05 0
		{ 2, 0x00A162, 100, 2, 0x002833, 0x0008 },  // DLC01FortHagenSatelliteArrayQuest 100 <- DLC01MQ04 3
		{ 2, 0x00A162, 1000, 2, 0x0010F5, 0x0001 },  // DLC01FortHagenSatelliteArrayQuest 1000 <- DLC01MQ05 0
		{ 2, 0x00A162, 1000, 2, 0x002833, 0x0008 },  // DLC01FortHagenSatelliteArrayQuest 1000 <- DLC01MQ04 3
		{ 2, 0x00F8EA, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ04Post 10 <- DLC01MQ05 0
		{ 2, 0x00FF87, 10, 2, 0x000801, 0x0001 },  // DLC01MQ01Caravan 10 <- DLC01MQ02 0
		{ 2, 0x00FF87, 10, 2, 0x0010F5, 0x0001 },  // DLC01MQ01Caravan 10 <- DLC01MQ05 0
		{ 2, 0x00FF87, 10, 2, 0x002833, 0x000F },  // DLC01MQ01Caravan 10 <- DLC01MQ04 0, 1, 2, 3
		{ 2, 0x00FF87, 1000, 2, 0x0010F5, 0x0001 },  // DLC01MQ01Caravan 1000 <- DLC01MQ05 0
		{ 2, 0x00FF87, 1000, 2, 0x002833, 0x000F },  // DLC01MQ01Caravan 1000 <- DLC01MQ04 0, 1, 2, 3
		{ 3, 0x000800, 100, 3, 0x000801, 0x000F },  // DLC04MQ00 100 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000800, 200, 3, 0x000801, 0x000F },  // DLC04MQ00 200 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000800, 300, 3, 0x000801, 0x000F },  // DLC04MQ00 300 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000800, 305, 3, 0x000801, 0x000E },  // DLC04MQ00 305 <- DLC04MQ01 1, 2, 3
		{ 3, 0x000800, 350, 3, 0x000801, 0x000E },  // DLC04MQ00 350 <- DLC04MQ01 1, 2, 3
		{ 3, 0x000800, 400, 3, 0x000801, 0x000F },  // DLC04MQ00 400 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000800, 500, 3, 0x000801, 0x000F },  // DLC04MQ00 500 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000800, 1000, 3, 0x000801, 0x000F },  // DLC04MQ00 1000 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000801, 10, 3, 0x000801, 0x0001 },  // DLC04MQ01 10 <- DLC04MQ01 0
		{ 3, 0x000801, 200, 3, 0x000801, 0x000F },  // DLC04MQ01 200 <- DLC04MQ01 0, 1, 2, 3
		{ 3, 0x000801, 205, 3, 0x000801, 0x0002 },  // DLC04MQ01 205 <- DLC04MQ01 1
		{ 3, 0x000801, 295, 3, 0x000801, 0x0004 },  // DLC04MQ01 295 <- DLC04MQ01 2
		{ 3, 0x000801, 300, 3, 0x000801, 0x0004 },  // DLC04MQ01 300 <- DLC04MQ01 2
		{ 3, 0x000801, 320, 3, 0x000801, 0x0008 },  // DLC04MQ01 320 <- DLC04MQ01 3
		{ 3, 0x000801, 550, 3, 0x000801, 0x0008 },  // DLC04MQ01 550 <- DLC04MQ01 3
		{ 3, 0x000801, 600, 3, 0x000801, 0x0008 },  // DLC04MQ01 600 <- DLC04MQ01 3
		{ 3, 0x000801, 610, 3, 0x000801, 0x0008 },  // DLC04MQ01 610 <- DLC04MQ01 3
		{ 3, 0x000801, 700, 3, 0x000801, 0x0008 },  // DLC04MQ01 700 <- DLC04MQ01 3
		{ 3, 0x000801, 710, 3, 0x000801, 0x0008 },  // DLC04MQ01 710 <- DLC04MQ01 3
		{ 3, 0x000801, 800, 3, 0x000802, 0x0001 },  // DLC04MQ01 800 <- DLC04MQ02 0
		{ 3, 0x000801, 1000, 3, 0x000802, 0x0001 },  // DLC04MQ01 1000 <- DLC04MQ02 0
		{ 3, 0x000801, 1000, 3, 0x000804, 0x001E },  // DLC04MQ01 1000 <- DLC04MQ04 1, 2, 3, 4
		{ 3, 0x000802, 1, 3, 0x000802, 0x0001 },  // DLC04MQ02 1 <- DLC04MQ02 0
		{ 3, 0x000802, 1, 3, 0x000804, 0x001E },  // DLC04MQ02 1 <- DLC04MQ04 1, 2, 3, 4
		{ 3, 0x000803, 10, 3, 0x000803, 0x003F },  // DLC04MQ03 10 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000803, 100, 3, 0x000803, 0x003F },  // DLC04MQ03 100 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000803, 200, 3, 0x000803, 0x003F },  // DLC04MQ03 200 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000803, 300, 3, 0x000803, 0x003F },  // DLC04MQ03 300 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000803, 400, 3, 0x000803, 0x003F },  // DLC04MQ03 400 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000803, 500, 3, 0x000803, 0x003F },  // DLC04MQ03 500 <- DLC04MQ03 0, 1, 2, 3, 4, 5
		{ 3, 0x000804, 10, 3, 0x000804, 0x001E },  // DLC04MQ04 10 <- DLC04MQ04 1, 2, 3, 4
		{ 3, 0x000804, 15, 3, 0x000804, 0x001C },  // DLC04MQ04 15 <- DLC04MQ04 2, 3, 4
		{ 3, 0x000804, 20, 3, 0x000804, 0x001C },  // DLC04MQ04 20 <- DLC04MQ04 2, 3, 4
		{ 3, 0x000804, 70, 3, 0x000804, 0x001C },  // DLC04MQ04 70 <- DLC04MQ04 2, 3, 4
		{ 3, 0x000804, 90, 3, 0x000804, 0x001C },  // DLC04MQ04 90 <- DLC04MQ04 2, 3, 4
		{ 3, 0x000804, 110, 3, 0x000804, 0x0018 },  // DLC04MQ04 110 <- DLC04MQ04 3, 4
		{ 3, 0x000804, 150, 3, 0x000804, 0x0018 },  // DLC04MQ04 150 <- DLC04MQ04 3, 4
		{ 3, 0x000804, 250, 3, 0x000804, 0x0010 },  // DLC04MQ04 250 <- DLC04MQ04 4
		{ 3, 0x01AABF, 140, 3, 0x000804, 0x001E },  // DLC04DialogueSettlement 140 <- DLC04MQ04 1, 2, 3, 4
		{ 3, 0x0277CC, 10, 3, 0x000804, 0x001E },  // DLC04MQ04KeywordAssignment 10 <- DLC04MQ04 1, 2, 3, 4
		{ 3, 0x03684F, 5, 3, 0x03684F, 0x0040 },  // DLC04_KiddieKingdomMain 5 <- DLC04_KiddieKingdomMain 6
		{ 3, 0x03684F, 20, 3, 0x03684F, 0x0040 },  // DLC04_KiddieKingdomMain 20 <- DLC04_KiddieKingdomMain 6
		{ 3, 0x03684F, 130, 3, 0x03684F, 0x0040 },  // DLC04_KiddieKingdomMain 130 <- DLC04_KiddieKingdomMain 6
		{ 3, 0x03684F, 400, 3, 0x03684F, 0x0040 },  // DLC04_KiddieKingdomMain 400 <- DLC04_KiddieKingdomMain 6
		{ 3, 0x03684F, 600, 3, 0x03684F, 0x0040 },  // DLC04_KiddieKingdomMain 600 <- DLC04_KiddieKingdomMain 6
	};

	// Whether a stage is one that a quickstart done in this game sets (QUICKSTART_SETS). a_done(File, quest, stage) says
	// whether that stage, a quickstart's, is done here.
	template <class Done>
	[[nodiscard]] inline bool SetByQuickstart(File a_file, std::uint32_t a_quest, std::uint16_t a_stage, Done&& a_done)
	{
		const auto key = std::tuple{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		const auto rows = std::ranges::equal_range(QUICKSTART_SETS, key, {}, [](const QuickstartSet& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage }; });
		for (const auto& row : rows) {
			for (std::uint16_t stage = 0; stage < 64; ++stage) {
				if (((row.byStages >> stage) & 1) && a_done(static_cast<File>(row.byFile), row.byQuest, stage)) {
					return true;
				}
			}
		}
		return false;
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
		{ 0, 0x0229EE, 600 }, { 0, 0x0229EE, 700 }, { 0, 0x0229EE, 710 }, { 0, 0x0229EE, 720 }, { 0, 0x0229EE, 730 }, { 0, 0x0229EE, 740 }, { 0, 0x0229EE, 750 },
		{ 0, 0x0229EE, 800 }, { 0, 0x0229EE, 900 }, { 0, 0x0229EE, 1000 },
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
		{ 0, 0x022A07, 270 },
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
		{ 0, 0x06D646, 1 }, { 0, 0x06D646, 2 }, { 0, 0x06D646, 3 }, { 0, 0x06D646, 4 }, { 0, 0x06D646, 5 }, { 0, 0x06D646, 6 }, { 0, 0x06D646, 7 },  // DN084
		{ 0, 0x06D646, 8 }, { 0, 0x06D646, 9 }, { 0, 0x06D646, 12 }, { 0, 0x06D646, 15 }, { 0, 0x06D646, 20 }, { 0, 0x06D646, 100 }, { 0, 0x06D646, 110 },
		{ 0, 0x06D646, 120 }, { 0, 0x06D646, 210 }, { 0, 0x06D646, 229 }, { 0, 0x06D646, 260 }, { 0, 0x06D646, 300 }, { 0, 0x06D646, 340 }, { 0, 0x06D646, 345 },
		{ 0, 0x06D646, 350 }, { 0, 0x06D646, 360 }, { 0, 0x06D646, 369 }, { 0, 0x06D646, 395 }, { 0, 0x06D646, 410 }, { 0, 0x06D646, 420 }, { 0, 0x06D646, 450 },
		{ 0, 0x06D646, 500 },
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
		{ 0, 0x0B9F9D, 0 }, { 0, 0x0B9F9D, 1 }, { 0, 0x0B9F9D, 2 }, { 0, 0x0B9F9D, 20 }, { 0, 0x0B9F9D, 180 }, { 0, 0x0B9F9D, 255 },  // BoS302
		{ 0, 0x0BACE9, 60 }, { 0, 0x0BACE9, 70 }, { 0, 0x0BACE9, 80 },  // BoSM01DistressSignals
		{ 0, 0x0BACFF, 30 }, { 0, 0x0BACFF, 85 }, { 0, 0x0BACFF, 100 }, { 0, 0x0BACFF, 200 },  // Inst307
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
		{ 0, 0x0F8D39, 200 },  // DN084_BoS303Battle
		{ 0, 0x0F8D3A, 200 },  // DN084_Inst303Battle
		{ 0, 0x0FDC8C, 0 }, { 0, 0x0FDC8C, 1 }, { 0, 0x0FDC8C, 2 }, { 0, 0x0FDC8C, 3 }, { 0, 0x0FDC8C, 30 }, { 0, 0x0FDC8C, 200 }, { 0, 0x0FDC8C, 210 },  // BoS303
		{ 0, 0x0FDC8C, 220 }, { 0, 0x0FDC8C, 255 },
		{ 0, 0x0FF3C2, 600 }, { 0, 0x0FF3C2, 1000 },  // DN011
		{ 0, 0x107A1C, 0 }, { 0, 0x107A1C, 30 }, { 0, 0x107A1C, 255 },  // BoS302B
		{ 0, 0x108EC0, 100 },  // FFGoodneighbor01
		{ 0, 0x10C64A, 10 }, { 0, 0x10C64A, 350 }, { 0, 0x10C64A, 360 }, { 0, 0x10C64A, 1910 }, { 0, 0x10C64A, 2000 },  // MQ302Min
		{ 0, 0x10C64B, 1000 },  // MQ302BoS
		{ 0, 0x10C64C, 10 }, { 0, 0x10C64C, 325 }, { 0, 0x10C64C, 326 }, { 0, 0x10C64C, 950 }, { 0, 0x10C64C, 1000 },  // MQ302RR
		{ 0, 0x11B36E, 475 },  // MinRecruit00
		{ 0, 0x122221, 300 },  // FFBunkerHill02
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
		{ 0, 0x15BD39, 30 }, { 0, 0x15BD39, 300 },  // InstMassFusion
		{ 0, 0x162A3E, 10 },  // BoS303LL
		{ 0, 0x16854F, 9 }, { 0, 0x16854F, 10 },  // MS05BTapes
		{ 0, 0x168550, 20 }, { 0, 0x168550, 30 },  // MS05BPostQuest
		{ 0, 0x16C207, 100 }, { 0, 0x16C207, 300 },  // RRKickOut
		{ 0, 0x173ED9, 0 }, { 0, 0x173ED9, 1 }, { 0, 0x173ED9, 2 }, { 0, 0x173ED9, 10 }, { 0, 0x173ED9, 20 }, { 0, 0x173ED9, 25 }, { 0, 0x173ED9, 50 },  // BoS304
		{ 0, 0x173ED9, 255 },
		{ 0, 0x174029, 0 }, { 0, 0x174029, 1 }, { 0, 0x174029, 2 }, { 0, 0x174029, 3 }, { 0, 0x174029, 4 }, { 0, 0x174029, 5 }, { 0, 0x174029, 6 },  // Inst307Fight
		{ 0, 0x174029, 7 }, { 0, 0x174029, 8 }, { 0, 0x174029, 10 }, { 0, 0x174029, 110 }, { 0, 0x174029, 120 }, { 0, 0x174029, 130 }, { 0, 0x174029, 200 },
		{ 0, 0x174029, 300 }, { 0, 0x174029, 320 }, { 0, 0x174029, 350 }, { 0, 0x174029, 390 }, { 0, 0x174029, 400 }, { 0, 0x174029, 1000 },
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
		{ 1, 0x004F2C, 145 }, { 1, 0x004F2C, 150 },  // DLC03MQPostQuest
		{ 1, 0x005BD7, 700 }, { 1, 0x005BD7, 710 }, { 1, 0x005BD7, 720 }, { 1, 0x005BD7, 9000 },  // DLC03DialogueFarHarbor
		{ 1, 0x00A990, 0 }, { 1, 0x00A990, 100 }, { 1, 0x00A990, 250 }, { 1, 0x00A990, 290 }, { 1, 0x00A990, 300 }, { 1, 0x00A990, 375 },  // DLC03FarHarborFF03a
		{ 1, 0x00A991, 100 }, { 1, 0x00A991, 200 }, { 1, 0x00A991, 500 }, { 1, 0x00A991, 9000 },  // DLC03FarHarborS01
		{ 1, 0x00A992, 1 }, { 1, 0x00A992, 50 }, { 1, 0x00A992, 100 }, { 1, 0x00A992, 500 },  // DLC03FarHarborM01
		{ 1, 0x00AB19, 150 }, { 1, 0x00AB19, 200 }, { 1, 0x00AB19, 350 },  // DLC03FarHarborS01Defend
		{ 1, 0x00AEC3, 1 }, { 1, 0x00AEC3, 100 }, { 1, 0x00AEC3, 200 }, { 1, 0x00AEC3, 300 }, { 1, 0x00AEC3, 325 }, { 1, 0x00AEC3, 400 },  // DLC03FarHarborS02
		{ 1, 0x00AEC4, 50 }, { 1, 0x00AEC4, 100 }, { 1, 0x00AEC4, 300 }, { 1, 0x00AEC4, 350 },  // DLC03FarHarborFF03b
		{ 1, 0x00AFB1, 150 },  // DLC03AtomM01
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
		{ 2, 0x0010F5, 0 }, { 2, 0x0010F5, 1 }, { 2, 0x0010F5, 2 }, { 2, 0x0010F5, 680 }, { 2, 0x0010F5, 1000 }, { 2, 0x0010F5, 1100 },  // DLC01MQ05
		{ 2, 0x002833, 0 }, { 2, 0x002833, 1 }, { 2, 0x002833, 2 }, { 2, 0x002833, 3 }, { 2, 0x002833, 50 }, { 2, 0x002833, 110 }, { 2, 0x002833, 160 },  // DLC01MQ04
		{ 2, 0x002833, 190 }, { 2, 0x002833, 255 },
		{ 2, 0x00FF87, 40 }, { 2, 0x00FF87, 50 }, { 2, 0x00FF87, 100 },  // DLC01MQ01Caravan
		{ 3, 0x000800, 520 }, { 3, 0x000800, 1000 },  // DLC04MQ00
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
		{ 0, 0x01E5E7, 20, 0, 0x05DE41, kSetPlayerEnemy },  // BoSDansePostGame: SetPlayerEnemy BrotherhoodofSteelFaction (by dansepostquestboshostilityscript.MakeBoSHostile)
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
		{ 0, 0x06D646, 1, 0, 0x05E558, kSetPlayerEnemy },  // DN084: SetPlayerEnemy InstituteFaction (by BoS303 30)
		{ 0, 0x06D646, 2, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by InstMassFusion 30)
		{ 0, 0x06D646, 3, 0, 0x05E558, kSetPlayerEnemy },  // SetPlayerEnemy InstituteFaction (by BoS303 30)
		{ 0, 0x06D646, 4, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by BoSKickOut 30)
		{ 0, 0x06D646, 5, 0, 0x05E558, kSetPlayerEnemy },  // SetPlayerEnemy InstituteFaction (by BoS303 30)
		{ 0, 0x06D646, 6, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by BoSKickOut 30)
		{ 0, 0x06D646, 7, 0, 0x05E558, kSetPlayerEnemy },  // SetPlayerEnemy InstituteFaction (by BoS303 30)
		{ 0, 0x06D646, 8, 0, 0x14EED8, kSetEnemy },  // SetEnemy DN084_MassFusionLabSecurityFaction PlayerFaction (by 360)
		{ 0, 0x06D646, 9, 0, 0x05E558, kSetPlayerEnemy },  // SetPlayerEnemy InstituteFaction (by 3)
		{ 0, 0x06D646, 360, 0, 0x14EED8, kSetEnemy },  // SetEnemy DN084_MassFusionLabSecurityFaction PlayerFaction (by 370)
		{ 0, 0x06D646, 370, 0, 0x14EED8, kSetEnemy },  // SetEnemy DN084_MassFusionLabSecurityFaction PlayerFaction
		{ 0, 0x06F5C1, 1, 0, 0x091895, kSetEnemy },  // BoS101: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x06F5C1, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x06F5C1, 140, 0, 0x248414, kSetPlayerEnemy },  // SetPlayerEnemy DN017_CrimeFactionArcjet
		{ 0, 0x07CC61, 210, 0, 0x10C4F4, kSetEnemy },  // RR301: SetEnemy pRR301SynthFaction pPlayerFaction
		{ 0, 0x07E1BB, 510, 0, 0x19A782, kSetEnemy },  // DN028: SetEnemy DN028_RaiderBossRoomFaction PlayerFaction
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
		{ 0, 0x136329, 200, 0, 0x05E558, kSetEnemy },  // Min207: SetEnemy InstituteFaction PlayerFaction (by minutemencentralscript.CheckForMQAdvance)
		{ 0, 0x142FD3, 1, 0, 0x05DE41, kSetEnemy },  // RR304: SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by RR303 1)
		{ 0, 0x142FD3, 20, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x142FD3, 25, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x142FD3, 30, 0, 0x05DE41, kSetEnemy },  // SetEnemy pBrotherhoodofSteelFaction pPlayerFaction (by 1)
		{ 0, 0x144FCA, 20, 0, 0x14BF23, kSetEnemy },  // MS07cEddiePostQuestLines: SetEnemy EddieFaction PlayerFaction
		{ 0, 0x15BD39, 30, 0, 0x05DE41, kSetPlayerEnemy },  // InstMassFusion: SetPlayerEnemy BrotherhoodofSteelFaction (by BoSKickOut 20)
		{ 0, 0x16C207, 100, 0, 0x0994F6, kSetPlayerEnemy },  // RRKickOut: SetPlayerEnemy pRailroadFaction
		{ 0, 0x16D036, 0, 0, 0x05E558, kSetEnemy },  // InstKickOut: SetEnemy InstituteFaction PlayerFaction (by instkickoutquestscript.KickOut)
		{ 0, 0x16D036, 100, 0, 0x05E558, kSetEnemy },  // SetEnemy PlayerFaction temp4 (by mq00script.MQFactionKickoutPlayer) [by hand]
		{ 0, 0x16FF04, 20, 0, 0x05DE41, kSetPlayerEnemy },  // BoSKickOut: SetPlayerEnemy BrotherhoodofSteelFaction (by boskickoutscript.KickOut)
		{ 0, 0x16FF04, 30, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by boskickoutscript.KickOut)
		{ 0, 0x173ED9, 1, 0, 0x091895, kSetEnemy },  // BoS304: SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x173ED9, 2, 0, 0x091895, kSetEnemy },  // SetEnemy BoS100FeralGhoulFaction PlayerFaction (by BoS100Fight 1)
		{ 0, 0x173ED9, 255, 0, 0x05E558, kSetEnemy },  // SetEnemy InstituteFaction PlayerFaction (by InstKickOut 0)
		{ 0, 0x174029, 1, 0, 0x05DE41, kSetPlayerEnemy },  // Inst307Fight: SetPlayerEnemy BrotherhoodofSteelFaction (by BoSKickOut 20)
		{ 0, 0x174029, 2, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 3, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 4, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 5, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 6, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 7, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x174029, 8, 0, 0x05DE41, kSetPlayerEnemy },  // SetPlayerEnemy BrotherhoodofSteelFaction (by 1)
		{ 0, 0x1764DF, 150, 0, 0x106C2F, kAddToFaction },  // FFDiamondCity10: AddToFaction PlayerEnemyFaction
		{ 0, 0x1764DF, 170, 0, 0x106C2F, kAddToFaction },  // AddToFaction PlayerEnemyFaction (by 150)
		{ 1, 0x001B40, 570, 1, 0x01A213, kSetPlayerEnemy },  // DLC03MQ02: SetPlayerEnemy pDLC03MQ02LoneChildAtomFaction
		{ 1, 0x001B43, 800, 0, 0x106C2F, kAddToFaction },  // DLC03MQ05: AddToFaction pPlayerEnemyFaction
		{ 1, 0x001B43, 850, 1, 0x00463D, kSetEnemy },  // SetEnemy pDLC03AcadiaFaction pPlayerFaction (by DLC03AcadiaDefense 100)
		{ 1, 0x001B44, 1, 1, 0x030DC4, kSetEnemy },  // DLC03MQ06: SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x001B44, 2, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x001B44, 3, 1, 0x030DC4, kSetEnemy },  // SetEnemy DLC03ChildrenofAtomFaction_MaleEntrance PlayerFaction (by DLC03CoA_DialogueNucleusEntrance 3)
		{ 1, 0x004C8E, 70, 1, 0x004462, kSetEnemy },  // DLC03CoA_DialogueNucleus: SetEnemy DLC03ChildrenofAtomFaction PlayerFaction (by DLC03CoA_Kickout 700)
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
		{ 2, 0x0010F5, 885, 2, 0x0008B5, kSetEnemy },  // DLC01MQ05: SetEnemy DLC01MechanistFaction PlayerFaction (by DLC01Lair 1400)
		{ 2, 0x0010F5, 890, 2, 0x0008B5, kSetEnemy },  // SetEnemy DLC01MechanistFaction PlayerFaction (by 885)
		{ 3, 0x000800, 347, 3, 0x037BA6, kSetEnemy },  // DLC04MQ00: SetEnemy DLC04HarveyHostileFaction PlayerFaction
		{ 3, 0x000802, 1500, 3, 0x00F438, kSetEnemy },  // DLC04MQ02: SetEnemy DLC04GangDisciplesFaction PlayerFaction (by DLC04RaiderKickout 20)
		{ 3, 0x000807, 99, 3, 0x00CC95, kSetEnemy },  // DLC04MS02: SetEnemy DLC04HubologistFaction PlayerFaction
		{ 3, 0x01E34A, 10, 3, 0x04A02F, kSetEnemy },  // DLC04GZMainQuest: SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A030, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02C, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02D, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A033, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A02E, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A031, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
		{ 3, 0x01E34A, 10, 3, 0x04A032, kSetEnemy },  // SetEnemy PlayerFaction temp5 (by dlc04:dlc04gzmainquestscript.Startup) [by hand]
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

	// The two stages that move the player (kMoves, no kBridge) and are HOSTILE's as a faction's kick-out: not gone with
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
	//  - InstKickOut 100 is the teleport out of the Institute (TeleportPlayerOutSpell, Wait 4, MoveTo the exterior marker,
	//    unconditional), which the game sets only for a player inside, after the kick-out proper: the quest's start-up
	//    stage 0, InstKickOutQuestScript.Kickout(player's location): the Institute's quests failed (1500), ComX688 91 and
	//    affinity -5000 for X6-88, RemoveFromFaction, SetEnemy on the Institute, the message; then by where the player is:
	//    inside, stage 20 (leave; the relay out sets 100), outside, stage 10 with the map marker, the relay door locked and
	//    MQ00's helper. So the quest is started here (its stage 0 judges this player's place itself) and 100 is not set;
	//    a quest already started or run here was kicked out here already, and nothing is done.
	// Mass Fusion's DN084 1-9 and BoS303 1-2, Inst307Fight's 0-8 and Rockets' Red Glare's RR304 20, 25 and 30 reach a
	// kick-out too (DN084 2, 4, 6 and 8 BoSKickOut 30; its 1, 3, 5, 7 and 9 and BoS303's the Institute's, through BoS303
	// 30; Inst307Fight's BoSKickOut 20, through its 1, which only its quickstarts set: play starts the airport battle at
	// its 10, from Inst307 30; RR304's BoSKickOut 20, through its 1 and RR303 1, which only those set: play starts
	// Rockets' Red Glare at its 100, and the Railroad's player is the Brotherhood's enemy by RR303's own stages, HOSTILE
	// rows that move nobody), but they are the developers' quickstarts, which the game never sets in play: no row here,
	// since they are never shared at all (kDebug, IsDebug; up to this they had rows, RR304 20 the last, read as the
	// quest's own start, and a quickstart set from one player's console ran a kick-out in every friend's game on the
	// strength of a stage nobody played), and the kick-out stages their scripts set in the typing player's game are not
	// reported from it either (QUICKSTART_SETS). So no mover a friend's game can send is left with a bare SetPlayerEnemy
	// (the player still in the faction, its companion still at their side): tests/HostileHoldTests.cpp checks that every
	// one has a row here. A kick-out stage done here already (a load of a save past it) is not run again.
	struct KickOut
	{
		std::uint8_t  file;   // File: the mover's quest's
		std::uint32_t quest;  // the mover: a kMoves row that is HOSTILE's
		std::uint16_t stage;
		std::uint32_t kickQuest;  // the kick-out, in the same file
		std::uint16_t kickStage;  // 0: start the quest (its start-up stage is the kick-out) rather than set a stage
	};
	inline constexpr KickOut KICKOUTS[] = {
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

	// The HOSTILE stages that take the faction's own companion with them: a faction's kick-out dismisses a companion of that
	// faction who has ever followed the player, as the game has it (BoSKickOutScript.KickOut: Danse's affinity to -5000,
	// COMDanse 91, and he joins the player's enemies unless exiled already; RRKickOut 100: Deacon; InstKickOutQuestScript.Kickout:
	// X6-88), itself or through what the stage calls and sets (InstMassFusion 30 sets BoSKickOut 20; the quickstarts'
	// rows, the kDebug ones, DN084's, Inst307Fight's and the rest, are never used). A friend's such stage lands here as any
	// (the maintainer's rule, 2026-11-11: the chooser's kick-out is both players', the companion included, as in the game),
	// and the companion it dismisses is this player's loss too; the row names them (the companion quest the chain goes
	// through and its unique alias: COMDanse's Danse, COMDeacon's Deacon, ComX688's X6-88, Nuka-World's DLC04COMGage's Gage,
	// Far Harbor's Old Longfellow) so QuestSync warns the player once on the HUD, just before it lands or its kick-out is
	// run (KICKOUTS), when that companion is the one following them (Companions::Following), and never for another
	// companion or one not at their side.
	// A row is conditional when its chain passes a function that goes on only on a condition of the game it runs in (the
	// scan's CONDITIONAL: MinutemenCentralScript.CheckForMQAdvance, which every stage that moves the Minutemen on calls and
	// which starts the Institute's kick-out only when the Minutemen are the story's last way on). Such a stage dismisses
	// nobody in nearly every game, so it gives no warning and leaves the once-a-session warning unspent (DismissalWarnings):
	// a line there would be false, and would use up the one a real kick-out later needs. When its condition does hold in
	// the receiving game the companion leaves without the line; where the kick-out a friend's game ran reaches this game as
	// its own stage (InstKickOut's), that row is not conditional, and warns.
	// Generated with tools/questscan.ps1 -Hostile (its DISMISSES lines, SafeScan.StageDismisses and DismissedCompanion: a
	// HOSTILE stage whose chain reaches DismissCompanion through a companion's quest). Sorted by file, quest and stage.
	struct Dismissal
	{
		std::uint8_t  file;
		std::uint32_t quest;
		std::uint16_t stage;
		std::uint8_t  companionFile;   // File: the companion quest's
		std::uint32_t companionQuest;  // form ID in its file (COMDanse, COMDeacon, ComX688 ...)
		std::uint16_t alias;           // its unique alias: the companion
		bool          conditional;     // dismissed only on a condition of the game the stage runs in: no warning
	};
	inline constexpr Dismissal DISMISSES[] = {
		{ 0, 0x0229EE, 2, 0, 0x0BBD95, 0, true },  // MQ302: companion DismissCompanion (by Min301 0): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x0229EE, 1000, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by minutemencentralscript.CheckForMQAdvance): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 70, 0, 0x0BBD95, 0, true },  // Min02: companion DismissCompanion (by 500): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 200, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 400): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 250, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 200): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 400, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 500): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 500, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 520): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 520, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 570): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 570, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by min02script.CheckRadioTransmitterStatus): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x03A457, 600, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by minutemencentralscript.SetProgress): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x043275, 1, 0, 0x0BBD97, 0, false },  // RR303: companion DismissCompanion (by BoSKickOut 20): COMDanse's Danse
		{ 0, 0x05DEE4, 220, 0, 0x0BBD95, 0, true },  // Min01: companion DismissCompanion (by MinRecruit00 10): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x05DEE4, 1500, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 2010): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x05DEE4, 2000, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by minutemencentralscript.SetProgress): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x05DEE4, 2010, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by MinRecruit00 10): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x06D646, 1, 0, 0x0BBD95, 0, false },  // DN084: companion DismissCompanion (by BoS303 30): ComX688's X688
		{ 0, 0x06D646, 2, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by InstMassFusion 30): COMDanse's Danse
		{ 0, 0x06D646, 3, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by BoS303 30): ComX688's X688
		{ 0, 0x06D646, 4, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by BoSKickOut 30): COMDanse's Danse
		{ 0, 0x06D646, 5, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by BoS303 30): ComX688's X688
		{ 0, 0x06D646, 6, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by BoSKickOut 30): COMDanse's Danse
		{ 0, 0x06D646, 7, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by BoS303 30): ComX688's X688
		{ 0, 0x06D646, 8, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by BoSKickOut 30): COMDanse's Danse
		{ 0, 0x06D646, 9, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by 3): ComX688's X688
		{ 0, 0x0AA778, 980, 0, 0x0BBD95, 0, true },  // Min03: companion DismissCompanion (by minutemencentralscript.SetProgress): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x0AA778, 1000, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by 980): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x0B2D48, 790, 0, 0x0BBD95, 0, false },  // RR201: companion DismissCompanion (by InstKickOut 0): ComX688's X688
		{ 0, 0x0DDFDB, 1, 0, 0x0BBD95, 0, true },  // Min301: companion DismissCompanion (by 0): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x0FDC8C, 1, 0, 0x0BBD95, 0, false },  // BoS303: companion DismissCompanion (by 30): ComX688's X688
		{ 0, 0x0FDC8C, 2, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by DN084 3): ComX688's X688
		{ 0, 0x0FDC8C, 30, 0, 0x0BBD95, 0, false },  // companion DismissCompanion (by InstKickOut 0): ComX688's X688
		{ 0, 0x0FDC8C, 250, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by minutemencentralscript.CheckForMQAdvance): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x107A1C, 30, 0, 0x0BBD9B, 0, false },  // BoS302B: companion DismissCompanion (by RRKickOut 100): COMDeacon's Deacon
		{ 0, 0x10C64A, 1910, 0, 0x0BBD95, 0, true },  // MQ302Min: companion DismissCompanion (by MQ302 1000): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x10C64A, 2000, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by MQ302 1000): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x10C64B, 1000, 0, 0x0BBD95, 0, true },  // MQ302BoS: companion DismissCompanion (by MQ302 1000): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x10C64C, 1000, 0, 0x0BBD95, 0, true },  // MQ302RR: companion DismissCompanion (by MQ302 1000): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x136329, 200, 0, 0x0BBD95, 0, true },  // Min207: companion DismissCompanion (by minutemencentralscript.CheckForMQAdvance): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 0, 0x142FD3, 1, 0, 0x0BBD97, 0, false },  // RR304: companion DismissCompanion (by RR303 1): COMDanse's Danse
		{ 0, 0x142FD3, 20, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x142FD3, 25, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x142FD3, 30, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x15BD39, 30, 0, 0x0BBD97, 0, false },  // InstMassFusion: companion DismissCompanion (by BoSKickOut 20): COMDanse's Danse
		{ 0, 0x16C207, 100, 0, 0x0BBD9B, 0, false },  // RRKickOut: companion DismissCompanion (by COMDeacon 91): COMDeacon's Deacon
		{ 0, 0x16D036, 0, 0, 0x0BBD95, 0, false },  // InstKickOut: companion DismissCompanion (by instkickoutquestscript.KickOut): ComX688's X688
		{ 0, 0x16FF04, 20, 0, 0x0BBD97, 0, false },  // BoSKickOut: companion DismissCompanion (by boskickoutscript.KickOut): COMDanse's Danse
		{ 0, 0x16FF04, 30, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by boskickoutscript.KickOut): COMDanse's Danse
		{ 0, 0x173ED9, 255, 0, 0x0BBD95, 0, false },  // BoS304: companion DismissCompanion (by InstKickOut 0): ComX688's X688
		{ 0, 0x174029, 1, 0, 0x0BBD97, 0, false },  // Inst307Fight: companion DismissCompanion (by BoSKickOut 20): COMDanse's Danse
		{ 0, 0x174029, 2, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 3, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 4, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 5, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 6, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 7, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 0, 0x174029, 8, 0, 0x0BBD97, 0, false },  // companion DismissCompanion (by 1): COMDanse's Danse
		{ 1, 0x005BD7, 9000, 1, 0x006E55, 0, false },  // DLC03DialogueFarHarbor: companion DismissCompanion (by DLC03_COMOldLongfellow 91): DLC03_COMOldLongfellow's OldLongfellow
		{ 1, 0x040A87, 100, 0, 0x0BBD95, 0, true },  // DLC03WorkshopRadiantOwned05: companion DismissCompanion (by 400): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 1, 0x040A87, 400, 0, 0x0BBD95, 0, true },  // companion DismissCompanion (by DLC03:WorkshopRadiantOwnedScript.DLC03FinishQuest): ComX688's X688 [only if MinutemenCentralScript.CheckForMQAdvance goes on]
		{ 3, 0x000802, 1500, 3, 0x027715, 0, false },  // DLC04MQ02: companion DismissCompanion (by DLC04RaiderKickout 20): DLC04COMGage's Gage
	};

	// The companion a HOSTILE stage dismisses (DISMISSES), or null.
	inline const Dismissal* DismissalOf(File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		const auto key = std::tuple{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		const auto it = std::ranges::lower_bound(DISMISSES, key, {}, [](const Dismissal& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage }; });
		return it != std::end(DISMISSES) && std::tuple{ it->file, it->quest, it->stage } == key ? &*it : nullptr;
	}

	// The faction whose kick-out drops each of those companions: the one the warning names. By hand, from the kick-outs'
	// scripts (BoSKickOut, RRKickOut and InstKickOut turn their own faction on the player; Far Harbor's turn on the player
	// sends Old Longfellow away; Nuka-World's Open Season, DLC04RaiderKickout, turns the gangs on the player, the Disciples
	// the one HOSTILE names). Not the stage's HOSTILE row: a stage can turn another faction on the player on its way (Mass
	// Fusion's lab security, the airport's feral ghouls) and reach the kick-out through another quest, and the line would
	// name the wrong one. tests/HostileHoldTests.cpp checks every DISMISSES row against HOSTILE, those stages apart.
	struct CompanionFaction
	{
		std::uint8_t  companionFile;   // File: the companion quest's
		std::uint32_t companionQuest;  // form ID in its file
		std::uint8_t  factionFile;     // File: the faction's
		std::uint32_t faction;         // form ID in its file
	};
	inline constexpr CompanionFaction COMPANION_FACTIONS[] = {
		{ 0, 0x0BBD95, 0, 0x05E558 },  // ComX688: InstituteFaction
		{ 0, 0x0BBD97, 0, 0x05DE41 },  // COMDanse: BrotherhoodofSteelFaction
		{ 0, 0x0BBD9B, 0, 0x0994F6 },  // COMDeacon: RailroadFaction
		{ 1, 0x006E55, 1, 0x005C7E },  // DLC03_COMOldLongfellow: DLC03SettlementFarHarborFaction
		{ 3, 0x027715, 3, 0x00F438 },  // DLC04COMGage: DLC04GangDisciplesFaction
	};

	// The faction whose kick-out drops a row's companion (COMPANION_FACTIONS), or null: the warning then names none.
	inline const CompanionFaction* DismissalFaction(const Dismissal& a_row)
	{
		for (const auto& row : COMPANION_FACTIONS) {
			if (row.companionFile == a_row.companionFile && row.companionQuest == a_row.companionQuest) {
				return &row;
			}
		}
		return nullptr;
	}

	// The warnings of a session: one for each companion, and none for a conditional row, which doesn't spend the
	// companion's either (a false line would, and the real kick-out later in the session would land unannounced).
	class DismissalWarnings
	{
	public:
		// Whether this row's warning is due for the companion (a form ID) now; said, it is spent.
		bool Due(const Dismissal& a_row, std::uint32_t a_companion) { return !a_row.conditional && warned.insert(a_companion).second; }

		void Clear() { warned.clear(); }

	private:
		std::unordered_set<std::uint32_t> warned;
	};

	// A stage that starts its quest here (the quest isn't running: setting a stage starts it) runs the
	// quest's start-up stage first, its 0 in the game's quests (MQ105's 0 moves the player): the stage is
	// judged and held for what both do. Null if neither does anything.
	// (StartRule: a quest's stage 0 that is a developer's quickstart is no start-up stage, the game never runs it, and its
	// kDebug would have every stage that starts the quest here skipped as one: BoS303's 0, Inst307Fight's, MQ302's.)
	inline const Rule* StartRule(File a_file, std::uint32_t a_quest)
	{
		const auto rule = Find(a_file, a_quest, 0);
		return rule && !(rule->flags & kDebug) ? rule : nullptr;
	}

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
	// player here all the same (kHostileOnly: the chooser's choice, without the move). A developer's quickstart (kDebug) is
	// skipped before any of that, whatever else it does: it never gets this far (IsDebug), and if it did it is nobody's choice.
	inline Verdict Judge(const Rule* a_rule, const Situation& a_now)
	{
		const std::uint8_t flags = a_rule ? a_rule->flags : 0;
		if ((flags & (kPays | kDebug)) || ((flags & kStory) && !a_now.sharedStory)) {
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
