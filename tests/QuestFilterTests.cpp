#include "Test.h"

#include "game/QuestFilter.h"
#include "game/RadiantRules.h"
#include "game/StageRules.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace
{
	using QuestFilter::Quest;

	// Fallout4.esm's quests, as the game files them.
	constexpr std::uint32_t DC_GATE = 0x000190BC;  // DialogueDiamondCityEntrance: no type, no objectives
	constexpr std::uint32_t MQ104 = 0x0001F25E;    // Unlikely Valentine: main quest (type 1)
	constexpr std::uint32_t MS17 = 0x00022A08;     // Human Error: side quest (type 7)
	constexpr std::uint32_t INST305 = 0x000A274B;  // Powering Up: a faction's finale, filed under no type
	constexpr std::uint32_t BOS302 = 0x000B9F9D;   // Blind Betrayal
	constexpr std::uint32_t BOS304 = 0x00173ED9;   // Ad Victoriam
	constexpr std::uint32_t OTHER = 0x00012345;    // any other quest (only the finales are told by form ID)

	Quest Filed(std::uint32_t a_formId, std::int8_t a_type, bool a_hasObjectives, std::string_view a_editorId)
	{
		Quest quest;
		quest.formId = a_formId;
		quest.type = a_type;
		quest.hasObjectives = a_hasObjectives;
		quest.editorId = a_editorId;
		return quest;
	}
}

TEST("quest filter: the main story, the factions, side quests and the add-ons' quests are shared by type; internal and misc quests aren't")
{
	for (const int type : { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12 }) {
		CHECK(QuestFilter::IsSharedType(static_cast<std::int8_t>(type)));
	}
	CHECK(!QuestFilter::IsSharedType(0));
	CHECK(!QuestFilter::IsSharedType(6));
	CHECK(!QuestFilter::IsSharedType(-1));

	CHECK(QuestFilter::Watched(Filed(MQ104, 1, true, "MQ104")));
	CHECK(QuestFilter::Watched(Filed(MS17, 7, true, "MS17")));
	CHECK(!QuestFilter::Watched(Filed(OTHER, 6, true, "FFDiamondCity01")));
	CHECK(!QuestFilter::Watched(Filed(OTHER, 0, false, "Followers")));
}

TEST("quest filter: the quests whose stages empty the player's inventory are never shared: the opening, the cult")
{
	// Fallout4.esm, as the game files them (tools/questscan.ps1): MQ101 "War Never Changes", the opening, empties the
	// player's inventory (RemoveAllItems at its 0 and 900; MQ102 7 gives the Pip-Boy in Vault 111); DN019JoinCult
	// (the Children of Atom's initiation) takes everything at its 600. Both are misc quests (type 6): a friend's stage of
	// either is never set here, so a host's Pip-Boy and weapons never go for a joiner's opening.
	CHECK(!QuestFilter::Watched(Filed(0x0001ED86, 6, true, "MQ101")));
	CHECK(!QuestFilter::Watched(Filed(0x000C5093, 6, true, "DN019JoinCult")));
	// Out of Time, which gives the Pip-Boy, is the story's (set only on a player before its 7, and only beside the friend).
	CHECK(QuestFilter::Watched(Filed(0x0001CC2A, 1, true, "MQ102")));
}

TEST("quest filter: the factions' finales are shared though the game files them under no type")
{
	for (const auto id : { INST305, BOS302, BOS304 }) {
		CHECK(QuestFilter::IsFinale(Filed(id, 0, true, "")));
		CHECK(QuestFilter::Watched(Filed(id, 0, true, "")));
	}
	CHECK(!QuestFilter::IsFinale(Filed(OTHER, 0, false, "Followers")));
	CHECK(!QuestFilter::IsFinale(Filed(INST305, 7, true, "")));  // a typed quest is shared by its type
	CHECK(!QuestFilter::IsDialogue(Filed(INST305, 0, true, "Inst305")));
}

TEST("quest filter: a dialogue quest with no objectives is shared: Diamond City's entrance scene opens the gate for friends")
{
	const auto gate = Filed(DC_GATE, 0, false, "DialogueDiamondCityEntrance");
	CHECK(QuestFilter::IsDialogue(gate));
	CHECK(QuestFilter::Watched(gate));
	CHECK(!QuestFilter::IsFinale(gate));
	CHECK(!QuestFilter::IsSharedType(gate.type));

	// Any place's: the rule is the game's naming, not a list of form IDs.
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DialogueGoodneighbor")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DialogueCovenant")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DLC03DialogueFarHarbor")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DLC04_DialogueNukaTown")));
	CHECK(QuestFilter::IsDialogueName("Dialogue"));
}

TEST("quest filter: a dialogue quest with objectives, or one of the game's other untyped quests, isn't shared by that rule")
{
	// With objectives it is started by having one shown, like any quest; the type decides whether it is shared.
	CHECK(!QuestFilter::IsDialogue(Filed(DC_GATE, 0, true, "DialogueDiamondCityEntrance")));
	CHECK(!QuestFilter::Watched(Filed(DC_GATE, 0, true, "DialogueDiamondCityEntrance")));
	// A typed quest is shared by its type, not as a dialogue quest (it is started by an objective).
	CHECK(!QuestFilter::IsDialogue(Filed(DC_GATE, 7, false, "DialogueDiamondCityEntrance")));
	CHECK(QuestFilter::Watched(Filed(DC_GATE, 7, false, "DialogueDiamondCityEntrance")));
	// The game's other quests under no type are each game's own, or run in the background.
	for (const auto name : { "REChokepointCT01", "RESceneKMK01", "COMPiper", "TutorialQuest", "WorkshopParent", "DN015", "Followers", "MQ101", "DLC03", "DLC03_", "" }) {
		CHECK(!QuestFilter::IsDialogueName(name));
		CHECK(!QuestFilter::Watched(Filed(OTHER, 0, false, name)));
	}
	// The name must begin with the word, after an add-on's prefix only.
	CHECK(!QuestFilter::IsDialogueName("MQ101PrewarDialogue"));
	CHECK(!QuestFilter::IsDialogueName("COMDialogueCait"));
	CHECK(!QuestFilter::IsDialogueName("DLCDialogue"));  // not an add-on's prefix (no number)
}

TEST("quest filter: the side quests the story manager starts with targets each game picks itself are each player's own (tools/questscan.ps1 -PerGame)")
{
	for (auto it = std::next(std::begin(QuestFilter::PER_GAME)); it != std::end(QuestFilter::PER_GAME); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::pair{ before.file, before.quest } < std::pair{ it->file, it->quest }));
	}
	CHECK(std::size(QuestFilter::PER_GAME) == 21);
	// Far Harbor's settlement requests that make a settler at runtime, Automatron's robot assaults, Nuka-World's raids and
	// the gangs' troublemakers.
	CHECK(QuestFilter::IsPerGame(1, 0x00EB4C));  // DLC03WorkshopRadiantOwned01
	CHECK(QuestFilter::IsPerGame(1, 0x039953));  // DLC03WorkshopRadiantOwned03
	CHECK(QuestFilter::IsPerGame(2, 0x003E8A));  // DLC01MQPostQuestRadiantAssault01
	CHECK(QuestFilter::IsPerGame(2, 0x00D5FC));  // DLC01MQPostQuestRadiantScene02
	CHECK(QuestFilter::IsPerGame(3, 0x014352));  // DLC04RaidChaseOff
	CHECK(QuestFilter::IsPerGame(3, 0x0322DA));  // DLC04_RQ_KillTroubleMaker
	// The four whose aliases are the game's own are RadiantRules's now (shared by their run), not listed here.
	CHECK(!QuestFilter::IsPerGame(0, 0x026340));  // DN121
	CHECK(!QuestFilter::IsPerGame(0, 0x0482CD));  // DN151
	CHECK(!QuestFilter::IsPerGame(1, 0x00EB51));  // DLC03WorkshopRadiantOwned02
	CHECK(!QuestFilter::IsPerGame(1, 0x040A86));  // DLC03WorkshopRadiantOwned04
	// Kept each game's own for what they do in each game: the raid that takes the settlement, the cache jobs' spawns.
	CHECK(QuestFilter::IsPerGame(3, 0x019088));  // DLC04RaidCoerce: its 20 takes the raided settlement from the player
	CHECK(QuestFilter::IsPerGame(3, 0x01B170));  // DLC04_RQ_StealCache: attackers made and the enemy gang rolled in each game
	CHECK(QuestFilter::IsPerGame(3, 0x02191C));  // DLC04_RQ_DefendCache: the same
	CHECK(QuestFilter::IsPerGame(3, 0x017F46));  // DLC04_RQ_KillRivalBoss: waits for a raider-path check, tested on a raider save
	CHECK(QuestFilter::IsPerGame(3, 0x04BD3C));  // DLC04_RQ_CollarRivalBoss: the same
	// The add-ons' own stories and side quests are shared by their type as before.
	CHECK(!QuestFilter::IsPerGame(2, 0x000806));  // DLC01MQ01
	CHECK(!QuestFilter::IsPerGame(3, 0x000801));  // DLC04MQ01
	CHECK(!QuestFilter::IsPerGame(3, 0x000807));  // DLC04MS02
	CHECK(!QuestFilter::IsPerGame(1, 0x001B40));  // DLC03MQ02
	// A form ID is the quest's only in its own file.
	CHECK(!QuestFilter::IsPerGame(0, 0x014352));
	CHECK(!QuestFilter::IsPerGame(3, 0x026340));
	// No quest is both PER_GAME and one of RadiantRules's radiants (shared by their run, or left alone there).
	for (const auto& radiant : RadiantRules::RADIANTS) {
		CHECK(!QuestFilter::IsPerGame(radiant.file, radiant.quest));
	}
	for (const auto& local : QuestFilter::PER_GAME) {
		CHECK(local.file <= 3);
		CHECK(local.quest <= 0x00FFFFFF);
	}
}

TEST("quest filter: the quests sQuests = safe shares are the rule's, sorted, of the game's masters, none shared another way (tools/questscan.ps1 -Safe)")
{
	const auto key = [](const QuestFilter::Local& a_local) { return std::pair{ a_local.file, a_local.quest }; };
	for (auto it = std::next(std::begin(QuestFilter::SAFE)); it != std::end(QuestFilter::SAFE); ++it) {
		CHECK(key(*std::prev(it)) < key(*it));
	}
	for (auto it = std::next(std::begin(QuestFilter::DENIED)); it != std::end(QuestFilter::DENIED); ++it) {
		CHECK(key(*std::prev(it)) < key(*it));
	}
	// 98: Fallout4.esm's 46, Far Harbor's 38, Automatron's 1, Nuka-World's 13 (ten of them shared for their outcomes, below).
	CHECK(std::size(QuestFilter::SAFE) == 98);
	std::size_t perFile[4]{};
	for (const auto& safe : QuestFilter::SAFE) {
		REQUIRE(safe.file <= 3);
		CHECK(safe.quest <= 0x00FFFFFF);
		++perFile[safe.file];
	}
	CHECK(perFile[0] == 46);
	CHECK(perFile[1] == 38);
	CHECK(perFile[2] == 1);
	CHECK(perFile[3] == 13);

	// Nuka-World's main line under no type, Far Harbor's own quests, Diamond City's and Goodneighbor's errands.
	CHECK(QuestFilter::IsSafe(3, 0x000803));  // DLC04MQ03
	CHECK(QuestFilter::IsSafe(3, 0x000806));  // DLC04MS01
	CHECK(QuestFilter::IsSafe(1, 0x023ACA));  // DLC03FarHarborFF02
	CHECK(QuestFilter::IsSafe(1, 0x04E779));  // DLC03FarHarborM03
	CHECK(QuestFilter::IsSafe(0, 0x04F623));  // FFDiamondCity01Misc
	CHECK(QuestFilter::IsSafe(0, 0x10CB94));  // FFGoodneighbor03
	CHECK(QuestFilter::IsSafe(2, 0x00AEDD));  // DLC01DialogueQuest
	// Mama Murphy's: her death at 700 is already every game's (a kill is shared).
	CHECK(QuestFilter::IsSafe(0, 0x0EBE71));  // MQ00MamaMurphy
	// A stage that starts or sets a quest each game runs its own way (a repeatable one, PER_GAME) leaves its quest out:
	// a friend's game would start that one there, unshared.
	CHECK(!QuestFilter::IsSafe(0, 0x048516));  // DN054: starts the repeatable DN054Radio
	CHECK(!QuestFilter::IsSafe(0, 0x0684D0));  // DN123: sets the repeatable Skylanes assault
	CHECK(!QuestFilter::IsSafe(0, 0x145722));  // BoSM04Postquest: sets the repeatable DN053
	CHECK(!QuestFilter::IsSafe(3, 0x010D91));  // DLC04DialogueRaiderBossOperators: sets DLC04RaidCoerce (PER_GAME)
	// A stage that, itself or through the scripts it calls and the unshared quests' stages it sets, turns a faction on
	// another or on the player, makes or lets someone be a companion, or changes a settlement's owner: each game's.
	CHECK(!QuestFilter::IsSafe(0, 0x02074B));  // CZ: Cait's and Tommy's ally faction, Cait recruitable
	CHECK(!QuestFilter::IsSafe(0, 0x1323AB));  // FFGoodneighbor07: SetEnemy, Hancock recruitable
	CHECK(!QuestFilter::IsSafe(0, 0x18817F));  // MS07cPostQuest: a stage of Nick's companion quest
	CHECK(!QuestFilter::IsSafe(3, 0x000805));  // DLC04MQ05: DLC04GangSetup turns the gangs on the player
	CHECK(!QuestFilter::IsSafe(3, 0x027704));  // DLC04RaiderKickout: DLC04SendKickOut, through its quest's script
	// A form ID is the quest's only in its own file.
	CHECK(!QuestFilter::IsSafe(0, 0x000803));
	CHECK(!QuestFilter::IsSafe(2, 0x04F623));
	// A place's own quest with no objectives (DN035) is not in this table but SILENT: one counts as started only by its
	// stages.
	CHECK(!QuestFilter::IsSafe(0, 0x0EF40C));
	CHECK(QuestFilter::IsSilent(0, 0x0EF40C));
	// The opening, survival mode's, the dog, the Combat Zone's fight, Nuka-World's park bookkeeping, its raiders' kickout
	// and its outpost count: denied.
	for (const auto& denied : QuestFilter::DENIED) {
		CHECK(!QuestFilter::IsSafe(denied.file, denied.quest));
	}
	CHECK(std::size(QuestFilter::DENIED) == 9);
	CHECK(!QuestFilter::IsSafe(3, 0x032A4A));  // DLC04MQ04_RaiderOutpostTracker: each game's own outposts
	CHECK(!QuestFilter::IsSafe(0, 0x01ED86));  // MQ101
	CHECK(!QuestFilter::IsSafe(0, 0x06DFAF));  // DogmeatQuest
	CHECK(!QuestFilter::IsSafe(0, 0x00080E));  // HC_Manager
	CHECK(!QuestFilter::IsSafe(3, 0x03C5A7));  // DLC04PowerTracker
	// DENIED is a backstop: the rule leaves MQ101 (empties the inventory), DogmeatQuest (AllowCompanion) and
	// DLC04RaiderKickout (SetEnemy) out by itself, and -Safe prints "denied:" for the other six only.
	std::size_t backstop = 0;
	for (const auto& denied : QuestFilter::DENIED) {
		backstop += (denied.file == 0 && (denied.quest == 0x01ED86 || denied.quest == 0x06DFAF)) || (denied.file == 3 && denied.quest == 0x027704);
	}
	CHECK(backstop == 3);
	// A call through a variable typed with a quest's script is followed (DN036_Post 15 and 25: DN036QuestScript's endings
	// set DN036 1000, which keeps nothing for each game): DN036_Post stays in (DN036 itself is in for its outcomes).
	CHECK(QuestFilter::IsSafe(0, 0x10E695));   // DN036_Post
	// None that the game's targets make each player's own, no radiant, no finale (those are shared their own way).
	for (const auto& local : QuestFilter::PER_GAME) {
		CHECK(!QuestFilter::IsSafe(local.file, local.quest));
	}
	for (const auto& radiant : RadiantRules::RADIANTS) {
		CHECK(!QuestFilter::IsSafe(radiant.file, radiant.quest));
	}
	for (const auto finale : QuestFilter::STORY_FINALES) {
		CHECK(!QuestFilter::IsSafe(0, finale));
	}
	// The story's own quests are shared by their type, not by this table.
	CHECK(!QuestFilter::IsSafe(0, MQ104));
	CHECK(!QuestFilter::IsSafe(0, MS17));
	CHECK(!QuestFilter::IsSafe(3, 0x000801));  // DLC04MQ01
}

TEST("quest filter: the quests shared for their faction or settlement outcomes are SAFE's (tools/questscan.cs ALLOW_OUTCOMES; tools/questscan.ps1 -Safe)")
{
	// The scan's "# outcomes shared with the story" lines: every ALLOW_OUTCOMES quest, each with objectives, SAFE.
	const QuestFilter::Local outcomes[] = {
		{ 0, 0x04542B },  // DN049: the robot-run shops
		{ 0, 0x08FCED },  // DN036: Egret Tours Marina
		{ 1, 0x004C8E },  // DLC03CoA_DialogueNucleus
		{ 1, 0x00A991 },  // DLC03FarHarborS01
		{ 1, 0x00AEC3 },  // DLC03FarHarborS02: Echo Lake Lumber
		{ 1, 0x013F3A },  // DLC03Acadia_Institute
		{ 1, 0x023ACB },  // DLC03FarHarborFF01: Dalton Farm
		{ 1, 0x02C687 },  // DLC03_ADV017
		{ 3, 0x01AABF },  // DLC04DialogueSettlement: Nuka-Town's traders
		{ 3, 0x03684F },  // DLC04_KiddieKingdomMain
	};
	for (const auto& quest : outcomes) {
		CHECK(QuestFilter::IsSafe(quest.file, quest.quest));
		CHECK(!QuestFilter::IsSilent(quest.file, quest.quest));
	}
	// Read and left out: the Hubologists' (their sessions are each player's: rads, the chair, donations, rank bonuses),
	// and those the rule still leaves out for a stage that starts a quest each game runs its own way.
	CHECK(!QuestFilter::IsSafe(3, 0x00CBD0));  // DLC04DialogueHubologists
	CHECK(!QuestFilter::IsSafe(3, 0x000805));  // DLC04MQ05: the park's power, DLC04GZNukaGalaxy
	CHECK(!QuestFilter::IsSafe(3, 0x0243A5));  // DLC04DialogueHarvey: DLC04REObjectCT03
	CHECK(!QuestFilter::IsSafe(3, 0x0260E2));  // DLC04SafariAdventureQuest: DLC04SafariAdventurePOST
	// Not in ALLOW_OUTCOMES, so still left out for their faction stage: the Combat Zone, Goodneighbor's 07, the Institute's
	// fight, Nuka-World's raider kick-out and Sierra's attacks.
	CHECK(!QuestFilter::IsSafe(0, 0x02074B));  // CZ
	CHECK(!QuestFilter::IsSafe(0, 0x1323AB));  // FFGoodneighbor07
	CHECK(!QuestFilter::IsSafe(0, 0x174029));  // Inst307Fight
	CHECK(!QuestFilter::IsSafe(3, 0x027704));  // DLC04RaiderKickout
	CHECK(!QuestFilter::IsSafe(3, 0x035790));  // DLC04MS01SierraAttacks
	// The objective-less ones (the kick-outs, DN084, DN117, Far Harbor's endings) are not shared yet: neither table's.
	CHECK(!QuestFilter::IsSilent(0, 0x16FF04));  // BoSKickOut
	CHECK(!QuestFilter::IsSilent(0, 0x082615));  // DN117
	CHECK(!QuestFilter::IsSafe(0, 0x082615));
}

TEST("quest filter: the objective-less quests sQuests = safe shares are the rule's less those denied, sorted, none shared another way (tools/questscan.ps1 -Safe)")
{
	const auto key = [](const QuestFilter::Local& a_local) { return std::pair{ a_local.file, a_local.quest }; };
	for (auto it = std::next(std::begin(QuestFilter::SILENT)); it != std::end(QuestFilter::SILENT); ++it) {
		CHECK(key(*std::prev(it)) < key(*it));
	}
	// 112: Fallout4.esm's 76, Far Harbor's 19, Automatron's 5, Nuka-World's 12.
	CHECK(std::size(QuestFilter::SILENT) == 112);
	std::size_t perFile[4]{};
	for (const auto& silent : QuestFilter::SILENT) {
		REQUIRE(silent.file <= 3);
		CHECK(silent.quest <= 0x00FFFFFF);
		++perFile[silent.file];
		// None SAFE's (those have objectives), DENIED, PER_GAME, a radiant or a finale.
		CHECK(!QuestFilter::IsSafe(silent.file, silent.quest));
		CHECK(std::ranges::find_if(QuestFilter::DENIED, [&](const QuestFilter::Local& a_local) { return key(a_local) == key(silent); }) == std::end(QuestFilter::DENIED));
		CHECK(!QuestFilter::IsPerGame(silent.file, silent.quest));
		if (silent.file == 0) {
			CHECK(std::ranges::find(QuestFilter::STORY_FINALES, silent.quest) == QuestFilter::STORY_FINALES.end());
		}
		CHECK(std::ranges::find_if(RadiantRules::RADIANTS, [&](const auto& a_radiant) { return a_radiant.file == silent.file && a_radiant.quest == silent.quest; }) == std::end(RadiantRules::RADIANTS));
	}
	CHECK(perFile[0] == 76);
	CHECK(perFile[1] == 19);
	CHECK(perFile[2] == 5);
	CHECK(perFile[3] == 12);

	// A place's own quests, the Castle's waves, the Prydwen's arrival (BoSEnable; its 15 is ScriptDoors' all the same),
	// Vault 114's gear door (DN142), Far Harbor's fight at the Nucleus's gate.
	CHECK(QuestFilter::IsSilent(0, 0x0EF40C));     // DN035
	CHECK(QuestFilter::IsSilent(0, 0x03479A));     // DN102
	CHECK(QuestFilter::IsSilent(0, 0x0ABF55));     // DN029FirstWave
	CHECK(QuestFilter::IsSilent(0, 0x1851A0));     // BoSEnable
	CHECK(QuestFilter::IsSilent(0, 0x13B747));     // DN142
	CHECK(QuestFilter::IsSilent(1, 0x043822));     // DLC03MQ02Fight
	// A form ID is the quest's only in its own file.
	CHECK(!QuestFilter::IsSilent(1, 0x0EF40C));
	// Denied by name: the game's systems, ambient scenes.
	CHECK(!QuestFilter::IsSilent(0, 0x21666E));  // Achievements
	CHECK(!QuestFilter::IsSilent(0, 0x249E2E));  // Patch_1_3
	CHECK(!QuestFilter::IsSilent(0, 0x160147));  // Tutorial
	CHECK(!QuestFilter::IsSilent(0, 0x1C4E44));  // InstituteSS1
	CHECK(!QuestFilter::IsSilent(0, 0x152706));  // BoSPrydwenScene_Airport
	CHECK(!QuestFilter::IsSilent(0, 0x22DCC5));  // RECheckpoint_InstInitQuest
	CHECK(!QuestFilter::IsSilent(0, 0x056649));  // ConvDiamondCityGenericMoe01 (ambient chatter)
	CHECK(!QuestFilter::IsSilent(0, 0x0BBD98));  // COMNick (a companion's)
	CHECK(!QuestFilter::IsSilent(0, 0x07921A));  // COMCait
	// A change-location hook whose stage starts a quest each game runs its own way: a story event for a Minutemen
	// request (the story manager starts DN121 or DN151 with that game's own picks, PER_GAME), or the repeatable DN017
	// or DN053 started outright.
	CHECK(!QuestFilter::IsSilent(0, 0x1310AB));  // DN121_ChangeLocation
	CHECK(!QuestFilter::IsSilent(0, 0x0E6AB0));  // DN151_ChangeLocation
	CHECK(!QuestFilter::IsSilent(0, 0x0DEDDD));  // DN017_ChangeLocation
	CHECK(!QuestFilter::IsSilent(0, 0x0AEC4E));  // DN053_ChangeLocation
	CHECK(!QuestFilter::IsSilent(0, 0x0F8D39));  // DN084_BoS303Battle: its 200 sets DN084, whose 10 starts a repeatable one
	// A stage that stops the quest below one that does more: a friend catching up on the host's stages, lowest first,
	// would stop it before MS07Intro's 400 starts Nick's case files.
	CHECK(!QuestFilter::IsSilent(0, 0x0282A7));  // MS07Intro
	// Denied by hand: the player's own gear, the Gauntlet, DN151's tracker.
	CHECK(!QuestFilter::IsSilent(0, 0x0DEC92));  // MS04PostQuest
	CHECK(!QuestFilter::IsSilent(3, 0x00A549));  // DLC04MQ01Gauntlet
	CHECK(!QuestFilter::IsSilent(0, 0x048327));  // DN151_Tracking: the tracker of the PER_GAME DN151
	// Shared since 0.17 with StageRules rows for their teleports (kMoves: the follow offer), and FFGoodneighbor08 on the
	// host's timer (StageRules::HOST_TIMERS); denied by hand up to 0.16.1.
	CHECK(QuestFilter::IsSilent(0, 0x185E58));  // RRAct3Pickup: its quickstarts 1 and 2 into the Institute are kMoves
	CHECK(QuestFilter::IsSilent(0, 0x1ED4FA));  // InstituteTeleportEffects: its 10 moves X6-88 to the relay, kMoves
	CHECK(QuestFilter::IsSilent(0, 0x121D0C));  // DN136_Attack: its 10 teleports the companion in on the Brotherhood's route, kMoves; its 2000's BoSMarshal is ALLOW_STARTS
	CHECK(QuestFilter::IsSilent(0, 0x176E24));  // DNPrime_BoS304: moves Prime and the synths, never the player; its 15's starts are ALLOW_STARTS
	CHECK(QuestFilter::IsSilent(0, 0x215CC7));  // MQ302Post: moves the faction leaders home, never the player
	CHECK(QuestFilter::IsSilent(0, 0x15DE65));  // FFGoodneighbor08: its timer's 25, 200 and 300 are the host's (HOST_TIMERS)
	for (const auto& timer : StageRules::HOST_TIMERS) {
		CHECK(QuestFilter::IsSilent(timer.file, timer.quest));  // a timer's quest is a shared one
	}
	// Left out by the rule: the opening, survival mode's, Nick's companion quest's stage.
	CHECK(!QuestFilter::IsSilent(0, 0x01ED86));  // MQ101
	CHECK(!QuestFilter::IsSilent(0, 0x00080E));  // HC_Manager
	CHECK(!QuestFilter::IsSilent(0, 0x18817F));  // MS07cPostQuest
	// The dialogue quests are shared by their name, not by this table.
	CHECK(!QuestFilter::IsSilent(0, DC_GATE));
}
