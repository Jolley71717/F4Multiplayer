#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Radiant quests: the ones the game hands out again and again, each time picking its own targets (the
// settlement that asks, the place to clear, the person kidnapped) by the aliases' conditions. Each game
// picks its own, so a stage of one player's run means nothing in another's: the "clear the raiders" stage
// of a friend's run at Corvega would finish yours at Tenpines Bluff. So a radiant is shared by its run:
// when one player's game starts one, the friends' games start the same quest with the same fills (the
// same settlement, place and people), and only then do its stages go between them, under that run's name.
// A fill one game can't match (an NPC made at runtime that isn't shared, a reference that isn't there)
// leaves that run each player's own, said in the log, so neither game's quest is broken.
//
// Which radiants are shared (RADIANTS, by file and form ID there: Fallout4.esm's, and Far Harbor's and Nuka-World's that can be): every one whose targets are the game's own
// references and locations (a settlement, a dungeon, its people, bosses and map marker, a container), with items
// the quest makes (a locket, a holotape, a tracker) made in each game for its own player. The Minutemen's
// settlement requests, Preston's requests from settlements the player has (but their attacks), the Brotherhood's,
// the Institute's and the Railroad's jobs, and Diamond City's wanted posters. The jobs whose targets or helpers are
// NPCs the quest makes at runtime (a courser, a patrol, a captive, a synth, an assault's defenders, the Brotherhood's
// squire and scribe, the Minutemen's synth boss) are shared by their run too, with those actors the run's owner's
// (Entry::spawned): one game's runtime NPCs are in its friends' games already, as copies (Spawns), placed while both
// players stand near them, and a friend's game puts its copies into the quest's aliases in place of the second set its
// own start made. Preston's "defend the settlement" jobs are attacks (Entry::attack): shared only from the game that
// hosts the session, whose attackers are the only ones; a friend's game holds its own attack back. Left alone, each
// player's own: the Castle's siege, the Minutemen's refugees, a vertibird flown by a scene. Those stay unshared even
// when the story is (before, a friend's stage started them here with this game's own targets). Kept free of the game
// so it can be tested; RadiantSync reads the quests.
namespace RadiantRules
{
	enum class Share : std::uint8_t
	{
		kNotRadiant,  // not one of the game's radiants: QuestFilter decides
		kShared,      // shared by its run, with the same fills
		kLeftAlone,   // a radiant each player keeps for themselves
	};

	struct Entry
	{
		std::uint8_t     file;    // StageRules::File: 0 Fallout4.esm, 1 DLCCoast.esm, 2 DLCRobot.esm, 3 DLCNukaWorld.esm
		std::uint32_t    quest;   // its form ID in that file (no load index: an add-on's is the player's load order's)
		std::string_view editorId;
		Share            share;
		// A run of it here that this player hasn't been given (no objective shown) may be stopped for a friend's with
		// other targets: the faction's own scripts start these whenever they can (the Institute's when the player joins,
		// the Railroad's at HQ, the Brotherhood's as their giver speaks), so a friend's game nearly always holds one of
		// its own. Only where stopping it before it is given changes nothing else: nothing counted, enabled or hidden
		// by its start, no stage run as it stops, and a quest that can run again (not "run once").
		bool replace = false;
		// Its actors are made at runtime (aliases its start creates: a courser, a patrol, a captive), so no game has the
		// other's. The run's owner's stand in every game: its fills name them by their spawn ids once they are shared, sent
		// again as that changes, and a friend's game forces its copies of them into those aliases as they are placed
		// (ReferenceAlias.ForceRefTo), removing its own of the quest as they come (RadiantSync, Spawns: as a followed
		// encounter's, EncounterRunBook.h). Its stages go by the run as any shared radiant's.
		bool spawned = false;
		// A settlement attack Preston hands out ("defend the settlement": WorkshopAttackScript): the attack is the host's.
		// Only the game that hosts the session shares its run (Begins); a friend's own run of it stays the friend's, as
		// before. In a friend's game that shares the host's run, its own attack never starts (HoldsAttack): its attackers
		// stay disabled where its start made them, and the host's, copies there (Spawns), are the attack.
		bool attack = false;
	};

	// The radiants by file and form ID (tools/questscan.cs reads them; their aliases were read the same way). Fallout4.esm's first.
	inline constexpr std::array RADIANTS{
		// The Minutemen's "a settlement needs our help": the settlement (its workshop), the place nearest it and
		// who lives there. Its 400 gives the reward (MinRecruitQuestScript.FinishQuest: MinRadiantReward to the
		// player, the settlement the player's), set once in each game.
		Entry{ 0, 0x11B36E, "MinRecruit00", Share::kShared },  // raiders (Oberland Station, Tenpines Bluff or another)
		Entry{ 0, 0x098136, "MinRecruit01", Share::kShared },  // raider troubles
		Entry{ 0, 0x099848, "MinRecruit02", Share::kShared },  // kidnapping (its 90, paying the ransom, is never set from a friend: StageRules)
		Entry{ 0, 0x157CA5, "MinRecruit04", Share::kShared },  // clear a place
		Entry{ 0, 0x186642, "MinRecruit07", Share::kShared },  // ghoul problem
		Entry{ 0, 0x106F05, "MinRecruit03", Share::kShared },  // Abernathy Farm's locket: the satellite station's container (the locket made in each game's)
		Entry{ 0, 0x15F03F, "MinRecruit05", Share::kShared },  // a hostile settlement to take, and its bosses
		Entry{ 0, 0x15F040, "MinRecruit06", Share::kShared },
		// A synth boss made at runtime at its dungeon's boss (its 200 the boss dead, its 400 the reward): the owner's
		// stands in every game, as the Brotherhood's squire does (Entry::spawned).
		Entry{ 0, 0x157577, "MinRecruit08", Share::kShared, false, true },
		// Refugees made at runtime, two to five of them enabled at random by its start (the rest deleted), who become the
		// settlement's settlers at its 400 (WorkshopParent's AddActorToWorkshopPUBLIC on each in its own collection, an
		// alias that can't be forced): a friend's game would add its own set, or none. Left alone.
		Entry{ 0, 0x164167, "MinRecruit09", Share::kLeftAlone },
		// Preston's requests from the player's own settlements.
		Entry{ 0, 0x03DF95, "MinRadiantOwned02", Share::kShared },               // raiders
		Entry{ 0, 0x03E0C1, "MinRadiantOwned03ChangeLocOnly", Share::kShared },  // kidnapping
		Entry{ 0, 0x161D1D, "MinRadiantOwned06ChangeLocOnly", Share::kShared },  // building up a settlement (built in every game)
		Entry{ 0, 0x162F44, "MinRadiantOwned07ChangeLocOnly", Share::kShared },  // a settlement's generator, a dungeon's part for it
		Entry{ 0, 0x109D73, "MinRadiantOwned10ChangeLocOnly", Share::kShared },  // a synth among the settlers (one recruited at runtime: each player's own)
		// Preston's "defend the settlement" (Entry::attack): the settlement from the event, its attackers made at runtime
		// at its attack marker (a collection, disabled until WorkshopAttackScript.StartAttack enables them at its 130). One
		// game's attackers are in its friends' games already, as the copies of any NPC made at runtime (Spawns); a friend's
		// game starting the same attack would send a second wave of its own. So only the host's run is shared, and a
		// friend's game that shares it never starts its own attack (its 130, and its 300, which rolls the attack's damage
		// when the settlement isn't loaded, never taken: UNTAKEN; the attack's clock held: ATTACK_*). The attack's end and
		// the player's arrival that starts its clock are the host's to report (OWNER_STAGES). These aren't the random
		// attacks the session's host books for everyone (WorkshopRaiderAttack and the others): those aren't radiants here.
		Entry{ 0, 0x0A1412, "MinRadiantOwned01", Share::kShared, false, false, true },  // raiders
		Entry{ 0, 0x05E51F, "MinRadiantOwned09ChangeLocOnly", Share::kShared, false, false, true },
		Entry{ 0, 0x160409, "MinRadiantOwned05", Share::kShared, false, false, true },  // super mutants
		Entry{ 0, 0x186A08, "MinRadiantOwned08", Share::kShared, false, false, true },  // gunners
		Entry{ 0, 0x0E477F, "MinRadiantOwned04_BOS", Share::kShared, false, false, true },
		Entry{ 0, 0x1533A9, "MinRadiantOwned11", Share::kShared, false, false, true },  // synths (WorkshopSynthAttackScript)
		// The Castle's siege: scripted waves from a holding cell (its _Attackers quest fills each wave), vertibirds and
		// Preston's lines, started by its starter at a story event. Left alone: a siege belongs with the attacks the
		// session's host books for everyone, not with the jobs shared by their run.
		Entry{ 0, 0x0BDB04, "MinDefendCastle", Share::kLeftAlone },
		Entry{ 0, 0x176328, "MinDefendCastleRadiantStarter", Share::kLeftAlone },
		Entry{ 0, 0x22FD6E, "MinDefendCastle_Attackers", Share::kLeftAlone },
		// The Brotherhood's. Their giver's line sets their 10 (it starts them), the player taking the job their 20, and
		// the reward is their 50: an item, and BoS200's stage for the job done (shared with the story too, set once). The
		// 0 is the developers' shortcut (it makes the player a member and moves them to the giver): never taken (UNTAKEN).
		// The squire's and the scribe's jobs (Entry::spawned) make the one who walks with the player at the airport as the
		// job is taken (their 20 and 30 enable them, their 25 and 40 make them follow: FollowerFollow, no teammate): the
		// owner's walks with the owner in every game (a copy is a puppet, its packages idle), and the alias of a friend's
		// game is forced onto its copy; the friend's own is removed as it loads. They go home (their 45 and 65 disable
		// them) as the owner reaches the Prydwen, not a friend: OWNER_STAGES. A dungeon's clearing (BoSR03's 30, from the
		// location's event in each game) and the turn-ins are the stages any shared job has. Learning Curve left hollow (the
		// scribe gone with its owner) is stopped and left to Quinlan, who offers it only while it isn't running
		// (NOT_RESTARTED); Leading by Example is started again as the Railroad's are, and Kells offers it at its 5.
		Entry{ 0, 0x064EC7, "BoSR01", Share::kShared, true },  // Cleansing the Commonwealth: a dungeon by the player's level, its bosses
		Entry{ 0, 0x0C30DC, "BosR02", Share::kShared, true },  // Quartermastery: a dungeon, the tech in its boss's container (made in each game)
		Entry{ 0, 0x09A687, "BoSR01PostHello", Share::kLeftAlone },  // dialogue only
		Entry{ 0, 0x0C8675, "BoSR03", Share::kShared, true, true },  // Leading by Example: a dungeon by the player's level; the squire made at the airport walks with the owner
		Entry{ 0, 0x0CF3E2, "BoSR04", Share::kShared, true, true },  // Learning Curve: a dungeon by the player's level; the scribe made at the airport walks with the owner, the data read by the owner's
		Entry{ 0, 0x0CF3E3, "BoSR04_AO_Scribe", Share::kLeftAlone },  // the story event of the scribe reading the data (its 10 and 20 only set Learning Curve's 50 and 60, which go by that run); it fires where the scribe's AI runs, the owner's game
		Entry{ 0, 0x0D1EB2, "BoSR05", Share::kShared, true },  // Feeding the Troops: a settlement (its happiness changed in every game)
		// The Institute's: all five start when the player joins (Inst301) and wait at their 5 or 10 until their giver hands
		// one out (its 100); each pays at its turn-in (10 or 20, and 1000). Appropriation and Reclamation (Entry::spawned)
		// make their actors at the start: the assault's defenders at its site's markers (enabled from its 10, so they stand
		// at the site, as its enemies, from the moment the player joins; its 200 starts the assault, acting on its link
		// aliases); the synth at its dungeon's marker, disabled and restrained until the job is taken (its 100). The owner's
		// stand in every game once the job is given (Spawns::OwnedRadiantOf shares them from its take, not its offer), the
		// item each makes (the tech, the recall code) is each game's own, as Hypothesis's data is. No objective of
		// Appropriation points at a defender (its 100 at the site and the item, its 200 at the giver), so it is never hollow:
		// a friend left without the owner finds the site empty, but the item is in its container in each game (the 150 puts
		// it there) and the job goes on by it.
		Entry{ 0, 0x0F7933, "InstR01", Share::kShared, true },  // Pest Control: a dungeon and its bosses
		Entry{ 0, 0x0EB268, "InstR02", Share::kShared, true },  // Hypothesis: a dungeon, its boss, the data on it (made in each game)
		Entry{ 0, 0x0F0D8C, "InstR05", Share::kShared, true },  // Political Leanings: the same NPC and marker in every game
		Entry{ 0, 0x0E8D19, "InstR01_OLD", Share::kLeftAlone },  // not used by the game
		Entry{ 0, 0x0EC1E9, "InstR03_OLD", Share::kLeftAlone },
		Entry{ 0, 0x0EDCEF, "InstR03NEW", Share::kShared, true, true },  // Appropriation: an assault at a site picked by conditions, its defenders made at the site's markers; the item made in each game
		Entry{ 0, 0x0EDE28, "InstR04", Share::kShared, true, true },     // Reclamation: a dungeon, the synth to bring back made at its marker (talked to by the owner); the recall code made in each game
		// The Railroad's. Their HQ and each other's stages start them whenever they can (Butcher's Bill's 100 offers Mercer
		// Safehouse, whose 300 offers Jackpot and Concierge), and most count the job as on offer (RRQuestsAvailable, counted
		// off as the job is taken): one stopped for a friend's run before it was taken is counted off as it stops (OFFERS).
		// Butcher's Bill and its second part run once, and Randolph Safehouse's offer hides the place's enemies until it is
		// taken: those are never stopped (a game holding its own run of one keeps it).
		Entry{ 0, 0x0B1F1D, "RRR01a", Share::kShared },        // Butcher's Bill: a dead drop by its conditions
		Entry{ 0, 0x0B1F1C, "RRR01b", Share::kShared },        // Butcher's Bill 2: a place; the dead captive and what is on them made in each game
		Entry{ 0, 0x0B3E82, "RRR02a", Share::kShared, true },  // Concierge: a place, its people (nothing counted at its start)
		Entry{ 0, 0x0B926B, "RRR03", Share::kShared, true },   // Jackpot: a place and its door; the tracker made in each game
		Entry{ 0, 0x0B26C2, "RRR04", Share::kShared, true },   // Mercer Safehouse: a settlement to build up (built in every game)
		Entry{ 0, 0x0B926A, "RRR05", Share::kShared, true },   // Weathervane: a high point; the MILA made in each game; offered again when done
		Entry{ 0, 0x1845F7, "RRR06", Share::kShared },         // Randolph Safehouse: a dead drop and a place, its bosses
		// PAM's, Desdemona's and Carrington's jobs whose actors are made at runtime (Entry::spawned): the place is picked by
		// conditions and forced as any; the actors made there are the owner's, their copies forced into a friend's aliases.
		// All four are offered at their 50 (counted) and taken at their 100 (To the Mattresses at its 75), so one stopped on
		// offer is counted off (OFFERS).
		Entry{ 0, 0x186C79, "RRR07", Share::kShared, true, true },  // Variable Removal: a courser made at the place
		Entry{ 0, 0x13A33D, "RRR08", Share::kShared, true, true },  // To the Mattresses: the gang's boss made at the place (which one, each game's RRR08DisplayNumber)
		Entry{ 0, 0x145F1A, "RRR09", Share::kShared, true, true },  // Lost Soul: the captive made at the place (its care package made in each game)
		Entry{ 0, 0x14A34A, "RRR10", Share::kShared, true, true },  // A Clean Equation: the patrol made at its start (its collection filled from them)
		Entry{ 0, 0x150577, "RRR11", Share::kLeftAlone },           // High Ground: a vertibird and its crew made at runtime, flown by a scene (QuestActors pairs no vertibird either)
		// Diamond City's wanted posters (misc quests): a dungeon and its bosses, the poster's note made in each game. All
		// three start with the game and run once, so a game whose own poster names another dungeon keeps its own.
		Entry{ 0, 0x14182C, "FFDiamondCityWanted01", Share::kShared },
		Entry{ 0, 0x18127D, "FFDiamondCityWanted04", Share::kShared },
		Entry{ 0, 0x181280, "FFDiamondCityWanted07", Share::kShared },
		Entry{ 0, 0x02C59B, "FFDiamondCity05", Share::kLeftAlone },  // a bloatfly's gland: the bloatfly is made at runtime
		// The add-ons' radiants and the story manager's side quests that were each player's own (QuestFilter::PER_GAME,
		// which said these four could be shared by their run): every alias is filled by its conditions, from the start's
		// event or is the game's own reference, and what the quest makes at runtime (the settlement's two created
		// aliases) is each game's own, left as kDerived. Each is handed out by a conversation or an event in one game, so a
		// friend's own run of it with other targets is not stopped (replace stays false). Left in PER_GAME, as not shared by
		// a run: the cache jobs (StealCache, DefendCache: their attackers and defenders are made at runtime and the enemy
		// gang is rolled in each game) and DLC04RaidCoerce (its 20 takes the settlement from the player; tools/questscan.ps1
		// -Safe reports a shared radiant's stage that does), and the gang jobs Kill and Collar Rival Boss (DLC04_RQ_Manager runs in
		// every save with Nuka-World, so nothing says the player took the raiders' path: they return with a check tested on a raider save).
		Entry{ 0, 0x026340, "DN121", Share::kShared },                        // Saugus Ironworks: the settlement from the event; 2 aliases made at runtime
		Entry{ 0, 0x0482CD, "DN151", Share::kShared },                        // Weston Water Treatment: the settlement from the event (its 500 places something at runtime)
		Entry{ 1, 0x00EB51, "DLC03WorkshopRadiantOwned02", Share::kShared },  // Far Harbor: a settlement's request
		Entry{ 1, 0x040A86, "DLC03WorkshopRadiantOwned04", Share::kShared },
	};

	// The radiant filed under a quest's file and its form ID there (no load index).
	inline const Entry* Find(std::uint8_t a_file, std::uint32_t a_quest)
	{
		const auto it = std::ranges::find_if(RADIANTS, [&](const Entry& a_entry) { return a_entry.file == a_file && a_entry.quest == a_quest; });
		return it != RADIANTS.end() ? &*it : nullptr;
	}

	inline Share ShareOf(std::uint8_t a_file, std::uint32_t a_quest)
	{
		const auto entry = Find(a_file, a_quest);
		return entry ? entry->share : Share::kNotRadiant;
	}

	inline bool IsRadiant(std::uint8_t a_file, std::uint32_t a_quest)
	{
		return ShareOf(a_file, a_quest) != Share::kNotRadiant;
	}


	// Whether some radiant has this form ID in some file: a cheap test before the quest's file is looked up.
	inline bool MayBeRadiant(std::uint32_t a_quest)
	{
		return std::ranges::any_of(RADIANTS, [&](const Entry& a_entry) { return a_entry.quest == a_quest; });
	}

	// Takes, OfferOf, LeavesCounted and Replaces below are keyed by a quest's runtime form ID, as RadiantSync holds it. All
	// their rows are Fallout4.esm's, whose load index is 00 in every load order (it is the master), so there the runtime ID
	// is the local one; an add-on's quest has a runtime ID above 0x00FFFFFF and matches none of them.

	// Stages of a shared radiant never taken from a friend: the developers' shortcuts, which a player never reaches
	// (BoSR01's, BosR02's, BoSR03's, BoSR04's and BoSR05's 0 make the player a member of the Brotherhood, set its
	// story's stages, BoSR03's and BoSR04's the main quest's too (MQ206's 1200 and 2000), and move them to their giver;
	// InstR03NEW's 0 puts the player in the Institute's faction and its marker on their map). Their startup stage is
	// another (BoSR03's 5, InstR03NEW's 10; BoSR04 has none, Quinlan's line sets its 10), so the 0 is set only from the
	// console or by a mod, and then only in its own game. StageRules lists the Brotherhood's as moving the player.
	// InstR04's 0 only sets its 5, as InstR01's, InstR02's and InstR05's do: taken.
	// Preston's attacks' 130 and 300 (Entry::attack): their 130 starts the attack (WorkshopAttackScript.StartAttack
	// enables this game's own attackers, or kills them all where its start had no story event to size the attack); their
	// 300 resolves it, rolling the settlement's damage (CheckResolveAttack) unless the attackers are dead here or the
	// settlement is loaded. A friend's game has the host's attack instead: the end of it comes as the host's 200 (the
	// attackers dead), 310 (failed) or 400 (the reward).
	struct Untaken
	{
		std::uint32_t quest;
		std::uint16_t stage;
	};
	inline constexpr std::array UNTAKEN{
		Untaken{ 0x00064EC7, 0 },  // BoSR01
		Untaken{ 0x000C30DC, 0 },  // BosR02
		Untaken{ 0x000C8675, 0 },  // BoSR03
		Untaken{ 0x000CF3E2, 0 },  // BoSR04
		Untaken{ 0x000D1EB2, 0 },  // BoSR05
		Untaken{ 0x000EDCEF, 0 },  // InstR03NEW
		Untaken{ 0x000A1412, 130 },  // MinRadiantOwned01
		Untaken{ 0x000A1412, 300 },
		Untaken{ 0x0005E51F, 130 },  // MinRadiantOwned09ChangeLocOnly
		Untaken{ 0x0005E51F, 300 },
		Untaken{ 0x00160409, 130 },  // MinRadiantOwned05
		Untaken{ 0x00160409, 300 },
		Untaken{ 0x00186A08, 130 },  // MinRadiantOwned08
		Untaken{ 0x00186A08, 300 },
		Untaken{ 0x000E477F, 130 },  // MinRadiantOwned04_BOS
		Untaken{ 0x000E477F, 300 },
		Untaken{ 0x001533A9, 130 },  // MinRadiantOwned11
		Untaken{ 0x001533A9, 300 },
	};

	inline bool Takes(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		return std::ranges::none_of(UNTAKEN, [&](const Untaken& a_untaken) { return a_untaken.quest == a_quest && a_untaken.stage == a_stage; });
	}

	// Stages of a shared radiant with actors made at runtime (Entry::spawned) that only the run's owner reports: a
	// friend's game sets its own as its player does what sets it, and takes the owner's, but never passes its own on.
	// The Brotherhood's escorts go home (BoSR03's 45 and BoSR04's 65 disable the squire and the scribe) when the player
	// who walks with them, the owner, is back on the Prydwen: a friend home first would send the owner's escort away
	// mid-job. This protects only the owner's game: the friend's own 45 or 65 (set as the friend enters the Prydwen after
	// the dungeon, by BoSR03PlayerScript and BoSR04PlayerScript) disables what the friend's alias holds, its copy of
	// the owner's escort, so the friend stops seeing the escort from then on until the owner's own 45 or 65 comes. And
	// the turn-ins (BoSR03's 50, BoSR04's 70) go by the run as any stage: a friend who turns the job in first completes
	// and stops the owner's too, and the owner's escort, a temporary reference the quest made, goes with it mid-walk.
	// Either player's turn-in ends the job.
	// Preston's attacks (Entry::attack, their run always the host's): the player's arrival (70, MinRadiantOwned01Script's
	// CheckLocation, which starts the attack's clock), the attackers dead (200), the settlers dead (290) and the attack
	// failed (310: the settlers dead, or the settlement's damage too high) are the host's, where the attack is. A friend's
	// game sets its own as they happen there (its settlers, its settlement's damage), but its arrival never starts the
	// host's attack while the host is away, and its own count never ends the host's. The reward (400) is either player's
	// turn-in, as any job's.
	inline constexpr std::array OWNER_STAGES{
		Untaken{ 0x000C8675, 45 },  // BoSR03
		Untaken{ 0x000CF3E2, 65 },  // BoSR04
		Untaken{ 0x000A1412, 70 },  // MinRadiantOwned01
		Untaken{ 0x000A1412, 200 },
		Untaken{ 0x000A1412, 290 },
		Untaken{ 0x000A1412, 310 },
		Untaken{ 0x0005E51F, 70 },  // MinRadiantOwned09ChangeLocOnly
		Untaken{ 0x0005E51F, 200 },
		Untaken{ 0x0005E51F, 290 },
		Untaken{ 0x0005E51F, 310 },
		Untaken{ 0x00160409, 70 },  // MinRadiantOwned05
		Untaken{ 0x00160409, 200 },
		Untaken{ 0x00160409, 290 },
		Untaken{ 0x00160409, 310 },
		Untaken{ 0x00186A08, 70 },  // MinRadiantOwned08
		Untaken{ 0x00186A08, 200 },
		Untaken{ 0x00186A08, 290 },
		Untaken{ 0x00186A08, 310 },
		Untaken{ 0x000E477F, 70 },  // MinRadiantOwned04_BOS
		Untaken{ 0x000E477F, 200 },
		Untaken{ 0x000E477F, 290 },
		Untaken{ 0x000E477F, 310 },
		Untaken{ 0x001533A9, 70 },  // MinRadiantOwned11
		Untaken{ 0x001533A9, 200 },
		Untaken{ 0x001533A9, 290 },
		Untaken{ 0x001533A9, 310 },
	};

	// Whether a stage of a run shared here is reported to the friends: every stage of our own run, and of a friend's run
	// every stage but OWNER_STAGES' (a_ours: the run is this game's).
	inline bool Reports(std::uint32_t a_quest, std::uint16_t a_stage, bool a_ours)
	{
		return a_ours || std::ranges::none_of(OWNER_STAGES, [&](const Untaken& a_stage_) { return a_stage_.quest == a_quest && a_stage_.stage == a_stage; });
	}

	// Shared radiants this game never starts again itself (RadiantSync::RestartOwn: `startquest`, after its own run was
	// stopped for a friend's that then didn't start here, or as hollow): their giver offers the job only while the quest
	// isn't running, so one started by hand would never be offered or taken again, blocked for good. Learning Curve
	// (BoSR04) has no start-up stage, and Quinlan's line that sets its 10 is conditioned on the quest not running (the
	// lines that take it need its 10): stopped, it is offered afresh; started again, it would run with no stage, mute.
	// Leading by Example (BoSR03) is started again: its start-up stage is 5, and Kells' greeting that offers it is
	// conditioned on its stage being 5 (the same state his own line leaves it in). The Institute's start when the player
	// joins and wait at their 5 or 10 for the giver, and the Railroad's are offered at their 50 by their own start.
	// The Minutemen's synth boss job (MinRecruit08) is started by Preston's story event (MinutemenCentralQuest's, which
	// turns his intro on), which can't start a quest that runs: one started by hand has no event and would sit mute until
	// its shutdown days run out, Preston offering none of its kind meanwhile; its dialogue is conditioned on its 20 not
	// being done. Preston's attacks are started by the same event, but are never run again here: never stopped for a
	// friend's run (Entry::replace) and never hollow (no actor of theirs is followed: Entry::spawned).
	inline constexpr std::array NOT_RESTARTED{
		0x000CF3E2u,  // BoSR04
		0x00157577u,  // MinRecruit08
	};

	// Whether this game's own run of the quest, stopped, is started again by this game (false: left to its giver).
	inline bool Restarts(std::uint32_t a_quest)
	{
		return std::ranges::find(NOT_RESTARTED, a_quest) == NOT_RESTARTED.end();
	}

	// ---- Preston's attacks (Entry::attack) --------------------------------------------------------

	// Whether this game shares its own run of a radiant once it is handed out here: every shared one, but an attack only
	// from the game that hosts the session (a_hosting), whose attack is the only one. A friend's own attack stays the
	// friend's, as every attack was before; on a dedicated server no game hosts, and none is shared.
	inline bool Begins(const Entry& a_entry, bool a_hosting)
	{
		return a_entry.share == Share::kShared && (!a_entry.attack || a_hosting);
	}

	// Whether a friend's run of a radiant, sent here, may be shared here: not an attack while this game hosts (only the
	// host's attack is shared; a game that sends one while this one hosts is out of step).
	inline bool Joins(const Entry& a_entry, bool a_hosting)
	{
		return !a_entry.attack || !a_hosting;
	}

	// Whether this game holds its own attack of a quest back: an attack whose run shared here is a friend's (the host's:
	// a_ours false), while the quest runs here. Out of the session (the run no longer shared) it is let go.
	inline bool HoldsAttack(const Entry* a_entry, bool a_shared, bool a_ours, bool a_running)
	{
		return a_entry && a_entry->attack && a_shared && !a_ours && a_running;
	}

	// How (WorkshopAttackScript, which every one of them has, MinRadiantOwned11's as WorkshopSynthAttackScript): its
	// TryToStartAttack, called by its own timer (ID 0, started for an hour as the player arrives, ATTACK_ARRIVED) and by
	// the quest's 120 (the settlement's defences built, a stage taken from the host), sets the attack's start (130) only
	// while its PlayerIsWaiting is false (ShouldStartAttack), and otherwise starts the timer again. So in a friend's game
	// PlayerIsWaiting is held true, and the timer is pushed out further than any wait reaches every ATTACK_TIMER_EVERY:
	// a timer due during a wait fires as it ends, with the wait's end (OnPlayerWaitStop, setting it false) in a race.
	// Nothing else starts it; the attack's damage roll (CheckResolveAttack, at 300) and StopQuestWhenAliasesUnloaded
	// (set at 130) come after it.
	inline constexpr std::string_view ATTACK_SCRIPT = "WorkshopAttackScript";
	inline constexpr std::string_view ATTACK_WAITING = "PlayerIsWaiting";
	inline constexpr std::int32_t     ATTACK_TIMER = 0;
	inline constexpr float            ATTACK_TIMER_HELD = 48.0f;  // game hours: a wait is at most 24
	inline constexpr auto             ATTACK_TIMER_EVERY = std::chrono::seconds{ 10 };
	inline constexpr float            ATTACK_TIMER_RESUMED = 1.0f;  // game hours, as CheckLocation starts it
	inline constexpr std::uint16_t    ATTACK_ARRIVED = 70;           // MinRadiantOwned01Script.CheckLocation
	inline constexpr std::uint16_t    ATTACK_DEFENDED = 120;         // the settlement's defences built
	inline constexpr std::uint16_t    ATTACK_STARTED = 130;          // WorkshopAttackScript's attackStartStage, in all six
	inline constexpr std::uint16_t    ATTACK_WON = 200;              // the attackers dead
	inline constexpr std::uint16_t    ATTACK_SETTLERS_DEAD = 290;
	inline constexpr std::uint16_t    ATTACK_RESOLVED = 300;         // the settlement's damage rolled (CheckResolveAttack)
	inline constexpr std::uint16_t    ATTACK_FAILED = 310;
	inline constexpr std::uint32_t    ATTACK_WORKSHOP_ALIAS = 9;     // Workshop: the settlement's workbench, in all six

	// Whether a quest's attack is begun or over by its stages done here: this game's own start (130), or an end of it,
	// this game's own or the host's taken here (the attackers dead, the settlers dead, the damage rolled, the attack
	// failed).
	inline bool AttackBegun(std::span<const std::uint16_t> a_done)
	{
		return std::ranges::any_of(a_done, [](std::uint16_t a_stage) {
			return a_stage == ATTACK_STARTED || a_stage == ATTACK_WON || a_stage == ATTACK_SETTLERS_DEAD || a_stage == ATTACK_RESOLVED || a_stage == ATTACK_FAILED;
		});
	}

	// Whether a held attack's clock starts again as the hold ends (the session over, the quest still running here, now
	// the player's own): when it would be running in a game of one, the player arrived or the settlement's defences built,
	// and the attack neither begun nor over. Otherwise the game starts it as it would (the player arriving), and one the
	// host already defended or lost (its 200, 290 or 310 taken here, the session over before the turn-in) is never fought
	// again here.
	inline bool ResumesAttackClock(std::span<const std::uint16_t> a_done)
	{
		const auto done = [&](std::uint16_t a_stage) { return std::ranges::find(a_done, a_stage) != a_done.end(); };
		return (done(ATTACK_ARRIVED) || done(ATTACK_DEFENDED)) && !AttackBegun(a_done);
	}

	// What becomes of the attack's timer as an attack is let go (RadiantSync's LetAttackGo).
	enum class AttackClock : std::uint8_t
	{
		kLeft,       // not a held attack: nothing is touched
		kResumed,    // started again for ATTACK_TIMER_RESUMED
		kCancelled,  // cancelled (the script's CancelTimerGameTime)
	};

	// An attack no longer held (its run no longer shared here, its quest stopped, or a loaded world's): a_held, this game
	// held it until now; a_waiting, its script's PlayerIsWaiting is set; a_running, the quest runs; a_done, its stages.
	// One this game held is let go whatever PlayerIsWaiting says (a wait that just ended cleared it, the hold's timer
	// still pending); a loaded world's only where PlayerIsWaiting is set, which only a hold leaves in a save (none is
	// made during a wait). The hold's timer (ATTACK_TIMER_HELD, pushed out while it was held, in a game whose player never
	// arrived too) is never left running: started again for an hour where the clock resumes, cancelled everywhere else.
	// Left pending it would fire up to 48 game hours later with PlayerIsWaiting clear and set the attack's 130: on a job
	// turned in since (a SetStage on a stopped quest starts it again), on one the host already ended, or before the
	// player ever arrived; and a save would carry it.
	inline AttackClock LetGo(bool a_held, bool a_waiting, bool a_running, std::span<const std::uint16_t> a_done)
	{
		if (!a_held && !a_waiting) {
			return AttackClock::kLeft;
		}
		return a_running && ResumesAttackClock(a_done) ? AttackClock::kResumed : AttackClock::kCancelled;
	}

	// The job's settlers (Preston's attacks; REScript's DeadCountGroups, the group whose StageToSet is ATTACK_SETTLERS_DEAD,
	// index 1 in all six): every death of the settlement's settlers in this game counts (IncrementDeadCount), and once the
	// count reaches the group's size (this game's own SettlementNPCs at the start) the script sets 290, then 300 and 310: the
	// job failed and stopped. In a friend's game whose attack is held that count is this game's own, not the host's: its
	// settlers the host's game lacks (made in this game alone) can die there while the host's job goes on, and the two games
	// would end differently. So while the attack is held the group's size is raised by ATTACK_SETTLERS_HELD (the script's own
	// AdjustDeadCounts, which can't reach it), and as it is let go lowered back to what it was: the host's 290 is the one
	// taken. A size at or over ATTACK_SETTLERS_HELD is held (no settlement has that many), so a save made while held, loaded
	// alone, gives it back as its hold is let go.
	inline constexpr std::string_view RE_SCRIPT = "REScript";
	inline constexpr std::string_view RE_DEAD_GROUPS = "DeadCountGroups";
	inline constexpr std::int32_t     ATTACK_SETTLERS_HELD = 10000;

	[[nodiscard]] constexpr bool IsSettlersHeld(std::int32_t a_size)
	{
		return a_size >= ATTACK_SETTLERS_HELD;
	}

	// The group's size held (a_hold) or given back: raised once (a held size is left as it is), or lowered to what it was
	// (every raise taken off, however many a hold made: two raises a call apart give back the same). Never negative.
	[[nodiscard]] constexpr std::int32_t SettlersHeldSize(std::int32_t a_size, bool a_hold)
	{
		const auto size = a_size < 0 ? 0 : a_size;
		if (a_hold) {
			return IsSettlersHeld(size) ? size : size + ATTACK_SETTLERS_HELD;
		}
		return size % ATTACK_SETTLERS_HELD;
	}

	// How a held size is given back as the hold is let go: through the script's own AdjustDeadCounts (which sets 290 if this
	// game's own settlers are all dead by then: right for the game of one the job is now) only while the job runs and its
	// attack neither began nor ended here (a_done); on a stopped quest a SetStage would start it again, and on an ended
	// attack it would fail a job already won or lost, so there the size alone is written back.
	[[nodiscard]] inline bool SettlersRecheck(bool a_running, std::span<const std::uint16_t> a_done)
	{
		return a_running && !AttackBegun(a_done);
	}

	// What a hold let go does with the job's settlers. The raise is a script call the VM runs a frame or so later, so a hold
	// let go before it ran would read a size not yet raised, give nothing back, and the raise would then land for good:
	// the settlers are given back only once no raise of this game's is still queued (a_raiseQueued), and then only a held
	// size (a_sizeHeld; one the VM cancelled the raise of has nothing to give back).
	enum class SettlersGiveBack : std::uint8_t
	{
		kNothing,     // no raise queued and the size isn't held
		kNow,         // the size is held: given back now
		kAfterRaise,  // this game's raise is still queued: given back once it has run (or the VM cancelled it)
	};

	[[nodiscard]] constexpr SettlersGiveBack GiveBackSettlers(bool a_raiseQueued, bool a_sizeHeld)
	{
		if (a_raiseQueued) {
			return SettlersGiveBack::kAfterRaise;
		}
		return a_sizeHeld ? SettlersGiveBack::kNow : SettlersGiveBack::kNothing;
	}

	// Whether the host's run of a quest that runs here already with the same targets is shared here (a_done: this game's
	// stages of it): not an attack this game's own has begun. Its 130 is never taken from a friend (UNTAKEN), so one done
	// here is this game's own start: its attackers are out, the hold would hold nothing, and the host's copies would be a
	// second wave at the same settlement. Each player's own then, as every attack was before.
	inline bool JoinsRunning(const Entry& a_entry, std::span<const std::uint16_t> a_done)
	{
		return !a_entry.attack || std::ranges::find(a_done, ATTACK_STARTED) == a_done.end();
	}

	// ---- The Railroad's jobs on offer ---------------------------------------------------------------

	// RRQuestsAvailable (Fallout4.esm): the Railroad's jobs on offer at HQ, which the Drummer Boy and the others there
	// speak of. A job's offer counts one in (Mod +1) and the job taken counts it off (Mod -1), so a run stopped between
	// the two would leave it one too high for good.
	inline constexpr std::uint32_t RR_QUESTS_AVAILABLE = 0x0016C2A1;

	// When a job's offer counts it: always, or by another global's value then (one that can't change while it is on offer).
	enum class Counted : std::uint8_t
	{
		kAlways,
		kIfPositive,  // the global is above 0
		kIfZero,      // the global is 0
	};

	struct Offer
	{
		std::uint32_t quest;
		std::uint16_t offered;  // the stage that counts it in
		std::uint16_t taken;    // the stage that counts it off
		Counted       counted = Counted::kAlways;
		std::uint32_t global = 0;  // the global kIfPositive and kIfZero read
	};

	// Read off their fragments (the offer's Mod +1, the take's Mod -1, under the same condition).
	inline constexpr std::array OFFERS{
		Offer{ 0x000B926B, 50, 100, Counted::kIfPositive, 0x000E1685 },  // Jackpot: once done before (RRR03Completed)
		Offer{ 0x000B26C2, 50, 75 },                                      // Mercer Safehouse (its 100 and its 300 set 75)
		Offer{ 0x000B926A, 50, 100, Counted::kIfZero, 0x000A59AC },      // Weathervane: while no MILA is placed (RRR05MILAsPlaced)
		Offer{ 0x00186C79, 50, 100 },                                     // Variable Removal
		Offer{ 0x0013A33D, 50, 75 },                                      // To the Mattresses (its 100 sets 75)
		Offer{ 0x00145F1A, 50, 100 },                                     // Lost Soul
		Offer{ 0x0014A34A, 50, 100 },                                     // A Clean Equation
	};

	inline const Offer* OfferOf(std::uint32_t a_quest)
	{
		const auto it = std::ranges::find(OFFERS, a_quest, &Offer::quest);
		return it != OFFERS.end() ? &*it : nullptr;
	}

	// Whether this game's run of a quest, stopped now with a_done its stages done, leaves its offer counted in
	// RRQuestsAvailable (a_value: the offer's condition global now): counted in, and not off yet.
	inline bool LeavesCounted(std::uint32_t a_quest, std::span<const std::uint16_t> a_done, float a_value)
	{
		const auto offer = OfferOf(a_quest);
		if (!offer || std::ranges::find(a_done, offer->offered) == a_done.end() || std::ranges::find(a_done, offer->taken) != a_done.end()) {
			return false;
		}
		switch (offer->counted) {
		case Counted::kIfPositive:
			return a_value > 0.0f;
		case Counted::kIfZero:
			return a_value == 0.0f;
		default:
			return true;
		}
	}

	// A friend's run of a quest that runs here with other targets: whether this game's run is stopped for theirs. Only one
	// the book doesn't know (no run shared here, ours or a friend's), that this player hasn't been given (a_given: an
	// objective shown), of a quest that may be (Entry::replace) and can run again (a_runOnce: the quest's own flag).
	inline bool Replaces(std::uint32_t a_quest, bool a_known, bool a_given, bool a_runOnce)
	{
		const auto entry = Find(0, a_quest);  // Fallout4.esm's only (see above)
		return entry && entry->share == Share::kShared && entry->replace && !a_known && !a_given && !a_runOnce;
	}

	// ---- Fills ------------------------------------------------------------------------------------

	enum class Kind : std::uint8_t
	{
		kRef = 0,  // a reference alias (a collection's members aren't matched: they come from the others)
		kLoc = 1,  // a location alias
	};

	// How an alias is filled when its quest starts.
	enum class Fill : std::uint8_t
	{
		kSource,    // by its conditions or the start's event: each game picks its own, so it is forced to the friend's
		kByOthers,  // pushed in by other aliases as they fill ("forced by aliases": the quest's Workshop); forced too where it can be
		kFixed,     // the same in every game: a forced reference or location, a unique actor, another quest's alias
		kDerived,   // from another alias (its location's boss, map marker), near one, or made at runtime: follows the rest; forced
		            // too where it can be (MakePlan), never a reason to fall back
	};

	// An alias as the game files it.
	struct Alias
	{
		std::uint32_t    id = 0;
		Kind             kind = Kind::kRef;
		Fill             fill = Fill::kDerived;
		bool             optional = false;
		std::string_view name;
	};

	// Aliases' flags (BGSBaseAlias::FLAGS): optional, filled by other aliases, may take a reference another quest reserves.
	inline constexpr std::uint32_t ALIAS_OPTIONAL = 1u << 1;
	inline constexpr std::uint32_t ALIAS_BY_OTHERS = 1u << 11;
	inline constexpr std::uint32_t ALIAS_ALLOW_RESERVED = 1u << 9;

	// A forced alias's flags for a friend's start: no longer filled by the others, and, only where its conditions are lifted
	// (an optional pick that follows this game's state: a settlement owned here and not there), allowed a reference another
	// quest of this game reserves. A needed alias keeps the reservations: a target a story or companion quest holds here
	// fails the start, and the run stays each player's own.
	constexpr std::uint32_t ForcedFlags(std::uint32_t a_flags, bool a_lifted)
	{
		return (a_flags & ~ALIAS_BY_OTHERS) | (a_lifted ? ALIAS_ALLOW_RESERVED : 0u);
	}

	// How a reference alias is filled, by BGSRefAlias::FILL_TYPE: 0 conditions, 1 forced, 2 from another alias, 3
	// from the start's event, 4 made at runtime, 5 another quest's alias, 6 a unique actor, 7 near another alias.
	inline Fill RefFill(std::uint16_t a_fillType, std::uint32_t a_flags)
	{
		if (a_flags & ALIAS_BY_OTHERS) {
			return Fill::kByOthers;
		}
		switch (a_fillType) {
		case 0:
		case 3:
			return Fill::kSource;
		case 1:
		case 5:
		case 6:
			return Fill::kFixed;
		default:
			return Fill::kDerived;
		}
	}

	// How a location alias is filled, by its fill type (read off the game's own: 0 conditions, 1 a forced location, 2
	// from another alias, 3 from the start's event, 4 another quest's alias).
	inline Fill LocFill(std::uint16_t a_fillType, std::uint32_t a_flags)
	{
		if (a_flags & ALIAS_BY_OTHERS) {
			return Fill::kByOthers;
		}
		switch (a_fillType) {
		case 0:
		case 3:
			return Fill::kSource;
		case 1:
		case 4:
			return Fill::kFixed;
		default:
			return Fill::kDerived;
		}
	}

	// An alias forced to a fill of this game's for the start (form 0: left empty).
	struct Force
	{
		std::uint32_t alias = 0;
		Kind          kind = Kind::kRef;
		std::uint32_t form = 0;

		bool operator==(const Force&) const = default;
	};

	// How this game starts a friend's run: the aliases to force, or why it can't (fallback set: the run stays each
	// player's own).
	struct Plan
	{
		std::vector<Force>         forces;
		std::string                fallback;
		// Reference aliases that follow the others whose friend's fill this game didn't have loaded at the start (a settler of a
		// settlement far away): left to the aliases they follow, and only noted if they differ once it runs (Compare).
		std::vector<std::uint32_t> unforced;

		[[nodiscard]] bool Ok() const { return fallback.empty(); }
	};

	inline const Protocol::RadiantFill* FillFor(std::span<const Protocol::RadiantFill> a_fills, const Alias& a_alias)
	{
		const auto it = std::ranges::find_if(a_fills, [&](const Protocol::RadiantFill& a_fill) { return a_fill.alias == a_alias.id && a_fill.kind == static_cast<std::uint8_t>(a_alias.kind); });
		return it != a_fills.end() ? &*it : nullptr;
	}

	inline std::string Why(const Alias& a_alias, std::uint32_t a_form)
	{
		if (a_alias.kind == Kind::kLoc) {
			return std::format("'{}' is the location {:08X}, which this game doesn't have", a_alias.name, a_form);
		}
		if (a_form == Protocol::RADIANT_UNSHARED_REF) {
			return std::format("'{}' is an NPC the friend's game made at runtime, which the session doesn't share", a_alias.name);
		}
		if (Protocol::IsSpawnRef(a_form)) {
			return std::format("'{}' is an NPC the friend's game made at runtime ({:08X}) that this game has no copy of", a_alias.name, a_form);
		}
		return std::format("'{}' is {:08X}, which this game doesn't have (or not loaded)", a_alias.name, a_form);
	}

	// The fills the friend's game picked for the aliases that each game picks for itself, as this game's: a_resolve(kind,
	// form) gives this game's form for theirs (the same plugin reference or location if it is here, our NPC under a
	// spawn id) or nothing. The ones pushed in by other aliases, or that follow from others, are forced too where they can
	// be (a settlement has two settlers with its boss's location reference type, and each game may take either; the boss is
	// then pushed into the spokesman), and otherwise left to the aliases they follow, as in the friend's game. Every fill
	// they sent must be one of this quest's aliases (their game's files and ours are the same).
	template <class Resolve>
	Plan MakePlan(std::span<const Alias> a_aliases, std::span<const Protocol::RadiantFill> a_fills, Resolve&& a_resolve)
	{
		Plan plan;
		for (const auto& fill : a_fills) {
			const auto known = std::ranges::any_of(a_aliases, [&](const Alias& a_alias) { return a_alias.id == fill.alias && static_cast<std::uint8_t>(a_alias.kind) == fill.kind; });
			if (!known) {
				plan.fallback = std::format("the friend's quest has an alias {} this game's hasn't", fill.alias);
				return plan;
			}
		}
		for (const auto& alias : a_aliases) {
			if (alias.fill == Fill::kFixed) {
				continue;
			}
			const auto          fill = FillFor(a_fills, alias);
			const std::uint32_t form = fill ? fill->form : 0;
			if (form == 0) {
				if (alias.fill != Fill::kSource) {
					continue;
				}
				if (!alias.optional) {
					plan.fallback = std::format("'{}' is empty in the friend's game", alias.name);
					return plan;
				}
				plan.forces.push_back({ alias.id, alias.kind, 0 });
				continue;
			}
			const std::optional<std::uint32_t> local = a_resolve(alias.kind, form);
			if (!local || *local == 0) {
				if (alias.fill != Fill::kSource) {
					if (alias.kind == Kind::kRef) {
						plan.unforced.push_back(alias.id);
					}
					continue;
				}
				plan.fallback = Why(alias, form);
				return plan;
			}
			plan.forces.push_back({ alias.id, alias.kind, *local });
		}
		return plan;
	}

	// A fill this game has that the friend's doesn't.
	struct Mismatch
	{
		std::uint32_t    alias = 0;
		std::string_view name;
		bool             decisive = false;  // one each game picks for itself: the targets differ
		std::uint32_t    theirs = 0;        // as this game's form (0: empty)
		std::uint32_t    ours = 0;
	};

	// After the start, or for a quest that already runs here: the fills of this game (a_here(kind, alias), 0 when empty)
	// that differ from the friend's. Only those this game can name (a fill made at runtime in each game is each game's).
	// a_unforced: after a start, the plan's (Plan::unforced): one of them that differs while a place of the run is the
	// friend's is another settler of the same place, only noted.
	template <class Resolve, class Here>
	std::vector<Mismatch> Compare(std::span<const Alias> a_aliases, std::span<const Protocol::RadiantFill> a_fills, Resolve&& a_resolve, Here&& a_here,
		std::span<const std::uint32_t> a_unforced = {})
	{
		const bool samePlace = std::ranges::any_of(a_aliases, [&](const Alias& a_alias) {
			const auto fill = FillFor(a_fills, a_alias);
			if (a_alias.kind != Kind::kLoc || !fill || fill->form == 0) {
				return false;
			}
			const std::optional<std::uint32_t> local = a_resolve(a_alias.kind, fill->form);
			return local && *local != 0 && *local == a_here(a_alias.kind, a_alias.id);
		});
		std::vector<Mismatch> out;
		for (const auto& alias : a_aliases) {
			const auto fill = FillFor(a_fills, alias);
			if (!fill) {
				continue;
			}
			// A place is the run's target whichever way it is filled (a fill type read wrong leaves it checked).
			const bool unforced = alias.kind == Kind::kRef && alias.fill != Fill::kSource && samePlace && std::ranges::find(a_unforced, alias.id) != a_unforced.end();
			const bool picked = (alias.fill == Fill::kSource || alias.fill == Fill::kByOthers || alias.kind == Kind::kLoc) && !unforced;
			std::uint32_t theirs = 0;
			if (fill->form != 0) {
				const std::optional<std::uint32_t> local = a_resolve(alias.kind, fill->form);
				if (!local || *local == 0) {
					if (alias.fill == Fill::kSource || alias.kind == Kind::kLoc) {
						out.push_back({ alias.id, alias.name, true, fill->form, a_here(alias.kind, alias.id) });
					}
					continue;
				}
				theirs = *local;
			}
			const std::uint32_t ours = a_here(alias.kind, alias.id);
			if (ours != theirs) {
				out.push_back({ alias.id, alias.name, picked, theirs, ours });
			}
		}
		return out;
	}

	inline bool SameTargets(std::span<const Mismatch> a_mismatches)
	{
		return std::ranges::none_of(a_mismatches, &Mismatch::decisive);
	}

	// ---- Runs -------------------------------------------------------------------------------------

	// One run of a radiant: the game that started it (its player id in the session) and its number there.
	struct Run
	{
		std::uint32_t origin = 0;
		std::uint32_t serial = 0;

		auto operator<=>(const Run&) const = default;
	};

	// What becomes of a friend's start here.
	enum class Answer : std::uint8_t
	{
		kIgnore,   // a run known here already, one finished or turned down here, or a start under way
		kStart,    // start the quest here with their fills
		kCompare,  // the quest runs here: share it if its fills are theirs
		kDone,     // a run with its targets finished here (shared again under another number): never started again
		kTake,     // the run is shared here already (taken back from a save, or after a reconnect): its start's stages are taken, each once
	};

	// The fills that name a run's targets, to know the run again under another number (a game that loaded a save from
	// inside it, or came back, shares it again as a new run): its locations, or where it has none its references, but
	// those no other game names (empty, made at runtime). In order, so two runs' compare equal.
	inline std::vector<Protocol::RadiantFill> TargetsOf(std::span<const Protocol::RadiantFill> a_fills)
	{
		const auto named = [](const Protocol::RadiantFill& a_fill) { return a_fill.form != 0 && (a_fill.form >> 24) != 0xFF; };
		const auto place = [](const Protocol::RadiantFill& a_fill) { return a_fill.kind == static_cast<std::uint8_t>(Kind::kLoc); };
		const bool places = std::ranges::any_of(a_fills, [&](const Protocol::RadiantFill& a_fill) { return named(a_fill) && place(a_fill); });
		std::vector<Protocol::RadiantFill> out;
		for (const auto& fill : a_fills) {
			if (named(fill) && place(fill) == places) {
				out.push_back(fill);
			}
		}
		std::ranges::sort(out, {}, [](const Protocol::RadiantFill& a_fill) { return std::tuple{ a_fill.kind, a_fill.alias, a_fill.form }; });
		return out;
	}

	// Where a game's run numbers start: a number of its own start (the seconds since 1970 then), so a game that restarts,
	// whose player the server gives back their id, never numbers a run as one of its last (its friends may still hold
	// that one, running or finished). A game makes far fewer runs than one a second.
	constexpr std::uint32_t SerialSeed(std::uint64_t a_unixSeconds)
	{
		return static_cast<std::uint32_t>(a_unixSeconds);
	}

	// What a save keeps of the book (Book::Save; the co-save's record): the session it was saved in, the runs its world
	// shares, each with its session (so a game that loads it shares its run again under the same run, which a friend who finished it
	// knows, and not as a new one), and the runs finished here in each of its last sessions (so one is never started
	// again after the game restarts, or comes back from another server). Run ids are a session's: the runs shared are
	// shared again only in that session, and a run finished is known only in its own.
	struct Saved
	{
		struct Quest
		{
			std::uint32_t                      quest = 0;
			std::vector<Run>                   runs;
			std::vector<Protocol::RadiantFill> targets;
			std::uint64_t                      session = 0;  // the session the runs are of

			bool operator==(const Quest&) const = default;
		};

		struct Finished
		{
			std::uint64_t    session = 0;
			std::vector<Run> runs;  // the most recent last

			bool operator==(const Finished&) const = default;
		};

		std::uint64_t         session = 0;  // the one it was saved in
		std::vector<Quest>    shared;
		std::vector<Finished> finished;  // by session, the one played most recently last

		bool operator==(const Saved&) const = default;
	};

	// Whether a save's record held for its session (RadiantSync's held record: a load's, not taken in yet, or the runs
	// kept from another session) says a friend's run of the quest runs here: a run of the quest, of the session the
	// player is in now (a_session: the one welcomed into; 0 out of one), that another player started. A run of another
	// session says nothing: the player had another id there, so its origin can't be told from theirs, and a player's
	// own run is never counted as a friend's (it would be run again as this game's own when hollow, HollowClock).
	inline bool HeldFriendsRun(const Saved& a_held, std::uint32_t a_quest, std::uint32_t a_self, std::uint64_t a_session)
	{
		if (a_self == 0 || a_session == 0) {
			return false;
		}
		return std::ranges::any_of(a_held.shared, [&](const Saved::Quest& a_saved) {
			return a_saved.quest == a_quest && a_saved.session == a_session && std::ranges::any_of(a_saved.runs, [&](const Run& a_run) { return a_run.origin != a_self; });
		});
	}

	inline constexpr std::uint16_t SAVED_VERSION = 1;
	inline constexpr std::size_t   MAX_SAVED_QUESTS = 128;    // RADIANTS has fewer (63; room for the settlement attacks to come)
	inline constexpr std::size_t   MAX_SAVED_RUNS = 8;        // of one quest's (the latest kept)
	inline constexpr std::size_t   MAX_SAVED_FINISHED = 256;  // of one session's (the oldest forgotten)
	inline constexpr std::size_t   MAX_SAVED_SESSIONS = 4;    // the one played longest ago forgotten

	// The co-save's bytes: the version, the session, each shared quest's runs and targets, then each session's finished
	// runs.
	inline std::vector<std::uint8_t> Encode(const Saved& a_saved)
	{
		std::vector<std::uint8_t> out;
		const auto                put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		const auto putRun = [&](const Run& a_run) {
			put(a_run.origin);
			put(a_run.serial);
		};
		const auto quests = (std::min)(a_saved.shared.size(), MAX_SAVED_QUESTS);
		put(SAVED_VERSION);
		put(a_saved.session);
		put(static_cast<std::uint8_t>(quests));
		for (std::size_t i = 0; i < quests; ++i) {
			const auto& quest = a_saved.shared[i];
			const auto  runs = (std::min)(quest.runs.size(), MAX_SAVED_RUNS);
			const auto  targets = (std::min)(quest.targets.size(), Protocol::MAX_RADIANT_FILLS);
			put(quest.quest);
			put(quest.session);
			put(static_cast<std::uint8_t>(runs));
			for (auto it = quest.runs.end() - static_cast<std::ptrdiff_t>(runs); it != quest.runs.end(); ++it) {
				putRun(*it);
			}
			put(static_cast<std::uint8_t>(targets));
			for (std::size_t j = 0; j < targets; ++j) {
				put(quest.targets[j].alias);
				put(quest.targets[j].kind);
				put(quest.targets[j].form);
			}
		}
		const auto sessions = (std::min)(a_saved.finished.size(), MAX_SAVED_SESSIONS);
		put(static_cast<std::uint8_t>(sessions));
		for (auto session = a_saved.finished.end() - static_cast<std::ptrdiff_t>(sessions); session != a_saved.finished.end(); ++session) {
			const auto finished = (std::min)(session->runs.size(), MAX_SAVED_FINISHED);
			put(session->session);
			put(static_cast<std::uint16_t>(finished));
			for (auto it = session->runs.end() - static_cast<std::ptrdiff_t>(finished); it != session->runs.end(); ++it) {
				putRun(*it);
			}
		}
		return out;
	}

	// A save's record (nothing if it can't be read: a damaged record shares no run).
	inline std::optional<Saved> Decode(std::span<const std::uint8_t> a_bytes)
	{
		std::size_t offset = 0;
		const auto  take = [&](auto& a_value) {
			if (offset + sizeof(a_value) > a_bytes.size()) {
				return false;
			}
			std::memcpy(&a_value, a_bytes.data() + offset, sizeof(a_value));
			offset += sizeof(a_value);
			return true;
		};
		const auto takeRun = [&](Run& a_run) { return take(a_run.origin) && take(a_run.serial); };
		std::uint16_t version = 0;
		Saved         saved;
		std::uint8_t  quests = 0;
		if (!take(version) || version != SAVED_VERSION || !take(saved.session) || !take(quests) || quests > MAX_SAVED_QUESTS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < quests; ++i) {
			Saved::Quest quest;
			std::uint8_t runs = 0;
			if (!take(quest.quest) || !take(quest.session) || !take(runs) || runs > MAX_SAVED_RUNS) {
				return std::nullopt;
			}
			quest.runs.resize(runs);
			for (auto& run : quest.runs) {
				if (!takeRun(run)) {
					return std::nullopt;
				}
			}
			std::uint8_t targets = 0;
			if (!take(targets) || targets > Protocol::MAX_RADIANT_FILLS) {
				return std::nullopt;
			}
			quest.targets.resize(targets);
			for (auto& fill : quest.targets) {
				if (!take(fill.alias) || !take(fill.kind) || !take(fill.form)) {
					return std::nullopt;
				}
			}
			saved.shared.push_back(std::move(quest));
		}
		std::uint8_t sessions = 0;
		if (!take(sessions) || sessions > MAX_SAVED_SESSIONS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < sessions; ++i) {
			Saved::Finished session;
			std::uint16_t   finished = 0;
			if (!take(session.session) || !take(finished) || finished > MAX_SAVED_FINISHED) {
				return std::nullopt;
			}
			session.runs.resize(finished);
			for (auto& run : session.runs) {
				if (!takeRun(run)) {
					return std::nullopt;
				}
			}
			saved.finished.push_back(std::move(session));
		}
		if (offset != a_bytes.size()) {
			return std::nullopt;
		}
		return saved;
	}

	// A quest running in this world, for Book::Restore: its stages done, and its fills (FillsOf), which name its targets.
	struct QuestHere
	{
		std::vector<std::uint16_t>         done;
		std::vector<Protocol::RadiantFill> fills;
	};

	// The radiants' runs in this game: which run of each quest is shared here, and with it the rule that pays a run's
	// reward once in each game. The reward is the quest's own stage (MinRecruit's 400), so a game must set each stage
	// of a run at most once and never start a run it has finished: a run's start that comes again (a friend's game
	// resends its runs when someone joins; a repeated packet) is ignored once that run is known, finished or turned
	// down here, and each stage of it is taken or reported once. A stage taken from a friend is never reported back.
	// A new run of the same quest (it is handed out again) is another run, and pays again. A run is known by its id: a
	// game that loads a save from inside a run, or comes back to the session, shares it again under the same run (the
	// save's record, Save and Restore, or the one kept at the session's end), so a friend who finished it ignores it, and
	// the runs finished here are kept with the save. A run shared again under a new number all the same (a save from
	// before its share, or one made before this was kept) is known by its targets, but only for a while after a run with
	// them finished here (SAME_RUN_WINDOW): later, the same targets are a new run handed out again (kDone otherwise).
	// That window is the only guard for such a save: one older than the run's share, loaded more than 15 minutes after
	// a friend finished the run (or after their game restarted, as the window isn't saved), starts it there again.
	// The runs finished here are kept through loads, sessions and servers, each known in its own session (Bind).
	class Book
	{
	public:
		using Time = std::chrono::steady_clock::time_point;

		// How long after a run finished here a run of the quest with its targets is taken for it shared again.
		static constexpr auto SAME_RUN_WINDOW = std::chrono::minutes{ 15 };

		Book() = default;
		// a_seed: where this game's run numbers start (SerialSeed).
		explicit Book(std::uint32_t a_seed) :
			serial(a_seed)
		{}

		enum class State : std::uint8_t
		{
			kStarting,  // started here with a friend's fills; not yet seen running
			kShared,
		};

		// This game started the quest in a session (the player has it): a run of ours, with the stages done so far.
		// a_fills: its fills here (FillsOf), which name its targets.
		Run Begin(std::uint32_t a_quest, std::uint32_t a_self, std::span<const std::uint16_t> a_done, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			const Run run{ a_self, ++serial };
			auto&     instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kShared;
			instance.runs.push_back(run);
			instance.reported.insert(a_done.begin(), a_done.end());
			instance.targets = TargetsOf(a_fills);
			return run;
		}

		// a_fills: the start's.
		[[nodiscard]] Answer OnStart(std::uint32_t a_quest, const Run& a_run, bool a_running, std::span<const Protocol::RadiantFill> a_fills = {}, Time a_now = {}) const
		{
			if (FinishedRun(a_run) || refused.contains(a_run)) {
				return Answer::kIgnore;
			}
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				const bool known = std::ranges::find(it->second.runs, a_run) != it->second.runs.end();
				if (known && it->second.state == State::kShared) {
					return Answer::kTake;
				}
				if (known || it->second.state == State::kStarting) {
					return Answer::kIgnore;
				}
				return Answer::kCompare;
			}
			if (a_running) {
				return Answer::kCompare;
			}
			return FinishedHere(a_quest, a_fills, a_now) ? Answer::kDone : Answer::kStart;
		}

		// Whether a run of the quest with these fills' targets finished here less than SAME_RUN_WINDOW before a_now.
		[[nodiscard]] bool FinishedHere(std::uint32_t a_quest, std::span<const Protocol::RadiantFill> a_fills, Time a_now = {}) const
		{
			const auto it = done.find(a_quest);
			if (it == done.end()) {
				return false;
			}
			const auto targets = TargetsOf(a_fills);
			const auto entry = std::ranges::find(it->second, targets, &Done::targets);
			return !targets.empty() && entry != it->second.end() && a_now - entry->at < SAME_RUN_WINDOW;
		}

		// Started here from a friend's run, the stages they had done to come (a_fills: the start's).
		void Starting(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			auto& instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kStarting;
			instance.runs.push_back(a_run);
			instance.targets = TargetsOf(a_fills);
		}

		// The quest runs here with the run's fills (started from it, or found running with them): the run is shared.
		// Whether it was shared here already, under another run (one shared again under a new number).
		bool Join(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			auto&      instance = quests[a_quest];
			const bool again = instance.state == State::kShared && !instance.runs.empty() && std::ranges::find(instance.runs, a_run) == instance.runs.end();
			instance.state = State::kShared;
			if (std::ranges::find(instance.runs, a_run) == instance.runs.end()) {
				instance.runs.push_back(a_run);
			}
			if (instance.targets.empty()) {
				instance.targets = TargetsOf(a_fills);
			}
			return again;
		}

		// The run can't be shared here (its fills can't be matched, the start failed): each player's own. Its start is
		// ignored from now on.
		void Refuse(std::uint32_t a_quest, const Run& a_run)
		{
			refused.insert(a_run);
			if (const auto it = quests.find(a_quest); it != quests.end() && it->second.state == State::kStarting && std::ranges::find(it->second.runs, a_run) != it->second.runs.end()) {
				quests.erase(it);
			}
		}

		// Whether the quest's run here is shared now: its stages go between the games.
		[[nodiscard]] bool Shares(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && it->second.state == State::kShared;
		}

		// Whether a run of the quest is known here (shared, or starting).
		[[nodiscard]] bool Knows(std::uint32_t a_quest) const
		{
			return quests.contains(a_quest);
		}

		[[nodiscard]] bool IsStarting(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && it->second.state == State::kStarting;
		}

		// A friend's stage of a run: to be set here only if that run is the one shared here and the stage is new to it.
		// A stage that comes while the run is still starting is kept for it (the caller queues it once started).
		bool Accept(std::uint32_t a_quest, const Run& a_run, std::uint16_t a_stage)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end() || std::ranges::find(it->second.runs, a_run) == it->second.runs.end()) {
				return false;
			}
			auto& instance = it->second;
			if (instance.reported.contains(a_stage) || !instance.taken.insert(a_stage).second) {
				return false;
			}
			instance.reported.insert(a_stage);  // done here because of them: never reported back
			return true;
		}

		// A stage done here by this player (not a friend's set here): the runs to report it under, once.
		std::vector<Run> Report(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end() || it->second.state != State::kShared || !it->second.reported.insert(a_stage).second) {
				return {};
			}
			return it->second.runs;
		}

		// The quest stopped here (completed, failed or timed out) at a_now: its run is over here, and never starts again.
		void Ended(std::uint32_t a_quest, Time a_now = {})
		{
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				for (const auto& run : it->second.runs) {
					Finish(run, current);
				}
				NoteDone(a_quest, std::move(it->second.targets), a_now);
				quests.erase(it);
			}
		}

		// What a save keeps of the book (Saved): the runs shared now, as runs of a_session, and those finished here in the
		// last sessions.
		[[nodiscard]] Saved Save(std::uint64_t a_session) const
		{
			Saved out{ a_session, {}, {} };
			for (const auto& [quest, instance] : quests) {
				if (instance.state == State::kShared && out.shared.size() < MAX_SAVED_QUESTS) {
					out.shared.push_back({ quest, instance.runs, instance.targets, a_session });
				}
			}
			for (const auto& past : pasts) {
				if (!past.recent.empty()) {
					out.finished.push_back({ past.session, past.recent });
				}
			}
			if (out.finished.size() > MAX_SAVED_SESSIONS) {
				out.finished.erase(out.finished.begin(), out.finished.end() - MAX_SAVED_SESSIONS);
			}
			return out;
		}

		// A save's record (or the one kept when the session ended), its world here now: each run it shared is shared
		// again, if a_share and the run is of the session bound now, where its quest runs with the run's targets; one
		// whose quest runs so but that isn't shared here (another session's, or a_share false) goes to a_kept, to be kept
		// until it is over (and then finished in its session) or the player is back in its session.
		// a_here(quest): the quest in this world (QuestHere), or nothing if it doesn't run. Its stages done count as
		// reported (they went under the run when they were done, or with its start). A run whose quest doesn't run here
		// now, or runs with other targets (handed out again), is over in this world: finished here at a_now, in its
		// session, whichever it is (one finished while the player was out of the session, or on another server, must
		// never start here again when they come back). Returns the runs shared again.
		template <class Here>
		std::size_t Restore(const Saved& a_saved, Here&& a_here, bool a_share = true, Time a_now = {}, std::vector<Saved::Quest>* a_kept = nullptr)
		{
			std::size_t shared = 0;
			for (const auto& saved : a_saved.shared) {
				const std::optional<QuestHere> here = a_here(saved.quest);
				if (!here || TargetsOf(here->fills) != saved.targets) {
					for (const auto& run : saved.runs) {
						Finish(run, saved.session);
					}
					NoteDone(saved.quest, saved.targets, a_now);
					continue;
				}
				if (!a_share || saved.session != current) {
					if (a_kept) {
						a_kept->push_back(saved);
					}
					continue;
				}
				if (saved.runs.empty() || quests.contains(saved.quest)) {
					continue;
				}
				auto& instance = quests[saved.quest];
				instance.state = State::kShared;
				instance.runs = saved.runs;
				instance.reported.insert(here->done.begin(), here->done.end());
				instance.targets = saved.targets;
				++shared;
			}
			return shared;
		}

		// The runs shared here now, each with its quest (to send the stages done here under them again).
		[[nodiscard]] std::vector<std::pair<std::uint32_t, Run>> Shared() const
		{
			std::vector<std::pair<std::uint32_t, Run>> out;
			for (const auto& [quest, instance] : quests) {
				if (instance.state == State::kShared) {
					for (const auto& run : instance.runs) {
						out.emplace_back(quest, run);
					}
				}
			}
			return out;
		}

		// This game's own runs going now (to send again to a player who joins).
		[[nodiscard]] std::vector<std::pair<std::uint32_t, Run>> Ours(std::uint32_t a_self) const
		{
			std::vector<std::pair<std::uint32_t, Run>> out;
			for (const auto& [quest, instance] : quests) {
				for (const auto& run : instance.runs) {
					if (run.origin == a_self && instance.state == State::kShared) {
						out.emplace_back(quest, run);
					}
				}
			}
			return out;
		}

		[[nodiscard]] std::vector<std::uint32_t> Quests() const
		{
			std::vector<std::uint32_t> out;
			for (const auto& [quest, instance] : quests) {
				out.push_back(quest);
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return quests.size(); }
		[[nodiscard]] std::size_t Finished() const
		{
			const auto* past = PastOf(current);
			return past ? past->runs.size() : 0;
		}

		// The session ended: no run is shared any more (the quests go on, each player's own). The runs finished or
		// turned down here stay known (a player keeps their id in a session when they come back), so a start sent
		// again never starts one here twice.
		void EndSession() { quests.clear(); }

		// A save loaded: the runs going are the loaded world's (its record's, Restore; each game shares its own again). Those
		// turned down here may be taken now; those finished here never start here again, nor for a while do their targets
		// (a save from before the reward would pay it twice).
		void Reloaded()
		{
			quests.clear();
			refused.clear();
		}

		// Another server: nothing of the last is going here. The runs finished in each session stay known in it (a player who
		// comes back to the first server finds them there), as do the targets finished here, and run numbers go on.
		void Clear()
		{
			quests.clear();
			refused.clear();
		}

		// The session the runs are of now (its welcome): a run finished is known in its own session only.
		void Bind(std::uint64_t a_session)
		{
			current = a_session;
			if (const auto it = std::ranges::find(pasts, a_session, &Past::session); it != pasts.end()) {
				std::rotate(it, it + 1, pasts.end());  // the one played most recently last
			} else {
				pasts.push_back(Past{ a_session, {}, {} });
			}
		}

		// A loaded save's runs finished (its record), in their sessions: known here from now on, with those known already.
		void Remember(const Saved& a_saved)
		{
			for (auto session = a_saved.finished.rbegin(); session != a_saved.finished.rend(); ++session) {
				for (const auto& run : session->runs) {
					Finish(run, session->session);
				}
			}
		}

	private:
		struct Done
		{
			std::vector<Protocol::RadiantFill> targets;
			Time                               at;  // when its run finished here
		};

		// The runs finished here in one session.
		struct Past
		{
			std::uint64_t    session = 0;
			std::set<Run>    runs;
			std::vector<Run> recent;  // the latest of them, the most recent last (saved)
		};

		[[nodiscard]] const Past* PastOf(std::uint64_t a_session) const
		{
			const auto it = std::ranges::find(pasts, a_session, &Past::session);
			return it != pasts.end() ? &*it : nullptr;
		}

		[[nodiscard]] bool FinishedRun(const Run& a_run) const
		{
			const auto* past = PastOf(current);
			return past && past->runs.contains(a_run);
		}

		// A run of a_session over here. A session not known here yet is taken for one played before the others (a save's).
		void Finish(const Run& a_run, std::uint64_t a_session)
		{
			auto it = std::ranges::find(pasts, a_session, &Past::session);
			if (it == pasts.end()) {
				it = pasts.insert(pasts.begin(), Past{ a_session, {}, {} });
			}
			if (it->runs.insert(a_run).second) {
				if (it->recent.size() >= MAX_SAVED_FINISHED) {
					it->recent.erase(it->recent.begin());
				}
				it->recent.push_back(a_run);
			}
		}

		// A run of the quest with these targets finished here at a_now (FinishedHere).
		void NoteDone(std::uint32_t a_quest, std::vector<Protocol::RadiantFill> a_targets, Time a_now)
		{
			if (a_targets.empty()) {
				return;
			}
			auto& targets = done[a_quest];
			std::erase_if(targets, [&](const Done& a_done) { return a_done.targets == a_targets; });
			if (targets.size() >= MAX_DONE) {
				targets.erase(targets.begin());
			}
			targets.push_back({ std::move(a_targets), a_now });
		}

		struct Instance
		{
			State                   state = State::kShared;
			std::vector<Run>        runs;      // the run (ours or a friend's) and any other found to have the same fills
			std::set<std::uint16_t> taken;     // friends' stages taken for it
			std::set<std::uint16_t> reported;  // stages that need no report from here (reported, sent with the start, or taken)
			std::vector<Protocol::RadiantFill> targets;  // TargetsOf its fills
		};

		static constexpr std::size_t MAX_DONE = 32;  // targets kept for each quest, the oldest forgotten

		std::map<std::uint32_t, Instance>                                        quests;
		std::vector<Past>                                                        pasts;  // the runs finished here, by session, the one played most recently last
		std::uint64_t                                                            current = 0;  // the session bound (Bind)
		std::set<Run>                                                            refused;
		std::map<std::uint32_t, std::vector<Done>>                               done;  // the targets of runs finished here, by quest
		std::uint32_t                                                            serial = 0;
	};

	// ---- Runs whose actors are the owner's (Entry::spawned) ------------------------------------------

	// The friends' runs of quests whose actors are made at runtime, shared here and followed: the owner's actors stand
	// here through our copies of them, forced into the run's aliases as they are placed, and our own actors of the quest
	// are removed as they come (RadiantSync, Spawns). A run is followed from the moment it is shared here until its
	// quest stops here, its owner leaves or loads a save (their copies go with them: nothing of theirs stands here any
	// more; the run stays shared, so their send of the same run when they are back is followed again), or the session
	// ends. A run's fills are the owner's as last sent (sent again as an actor of theirs is shared). The quests whose
	// runs were followed here once are remembered (Once) until each stops here: only such a quest is ever run again as
	// this game's own when it is left hollow (HollowClock); a player's own run never is.
	class Followed
	{
	public:
		using Time = std::chrono::steady_clock::time_point;

		struct Entry
		{
			Run                                run;
			std::vector<Protocol::RadiantFill> fills;
			Time                               at{};  // when the run was last sent (followed, or followed again)
		};

		// The run is shared here with these fills (its start, or a newer send of it): followed from now on. True when it
		// is new here (another run of the quest, or none, was followed before): what was forced for the old one is forgotten.
		bool Follow(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills, Time a_now = {})
		{
			auto&      entry = quests[a_quest];
			const bool fresh = entry.run != a_run;
			entry.run = a_run;
			entry.fills.assign(a_fills.begin(), a_fills.end());
			entry.at = a_now;
			once.insert(a_quest);
			return fresh;
		}

		[[nodiscard]] bool Follows(std::uint32_t a_quest) const { return quests.contains(a_quest); }

		// Whether a friend's run of the quest was followed here once, and the quest hasn't stopped here since (whether or
		// not it is followed now: the owner may be gone, or the session over).
		[[nodiscard]] bool Once(std::uint32_t a_quest) const { return once.contains(a_quest); }

		[[nodiscard]] const Entry* Find(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() ? &it->second : nullptr;
		}

		[[nodiscard]] const std::map<std::uint32_t, Entry>& Entries() const { return quests; }

		// The player a_origin left the session or loaded a save: their runs are followed no more, but for one they sent
		// again within a_grace of now (their send came after the load the word is of: the server holds the word of a
		// second load within RELOAD_TOLD_EVERY of the first, and the owner's send from that load can come first; the run
		// is one they still have). The quests of those followed no more.
		std::vector<std::uint32_t> OwnerGone(std::uint32_t a_origin, Time a_now = {}, std::chrono::steady_clock::duration a_grace = {})
		{
			std::vector<std::uint32_t> out;
			for (auto it = quests.begin(); it != quests.end();) {
				if (it->second.run.origin == a_origin && !(a_grace > std::chrono::steady_clock::duration{} && a_now - it->second.at < a_grace)) {
					out.push_back(it->first);
					it = quests.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// The quest stopped here, or was run again as this game's own: its run, and that one was followed, are forgotten.
		void Ended(std::uint32_t a_quest)
		{
			quests.erase(a_quest);
			once.erase(a_quest);
		}

		// Nothing is followed any more (the session over, or another server): what was followed once stays known, as the
		// quests go on here.
		void Clear() { quests.clear(); }

		// Another world was loaded: nothing of this one's is known (its record says what was shared there).
		void Reset()
		{
			quests.clear();
			once.clear();
		}

		[[nodiscard]] std::size_t Size() const { return quests.size(); }

	private:
		std::map<std::uint32_t, Entry> quests;
		std::set<std::uint32_t>        once;
	};

	// The server tells the others of a player's load at most this often (Server::Options::reloadToldEvery), holding the
	// word of a sooner one until it is up: the owner's send of their runs from that load can come before the word of it.
	inline constexpr auto RELOAD_TOLD_EVERY = std::chrono::seconds(2);
	// A run its owner sent again within this of the word of their load is one they still have (Followed::OwnerGone).
	inline constexpr auto RELOAD_GRACE = RELOAD_TOLD_EVERY + std::chrono::seconds(1);

	// A run's start sent again (its fills changed: an actor of the owner's shared since names itself by its spawn id)
	// while an earlier send of it waits its turn here, or is being started: the newer fills replace the older, and the
	// stages it brings join those that came meanwhile (each is set once anyway, Book::Accept). True when it is the same
	// run (otherwise nothing is changed).
	inline bool Supersedes(Protocol::RadiantStart& a_pending, const Protocol::RadiantStart& a_newer)
	{
		if (a_pending.quest != a_newer.quest || a_pending.playerId != a_newer.playerId || a_pending.serial != a_newer.serial) {
			return false;
		}
		a_pending.fills = a_newer.fills;
		for (const auto stage : a_newer.stages) {
			if (std::ranges::find(a_pending.stages, stage) == a_pending.stages.end()) {
				a_pending.stages.push_back(stage);
			}
		}
		return true;
	}

	// A job whose target is gone for good (hollow, below) is run again as this game's own once it has been so this long:
	// a friend's run of it heard meanwhile (the owner back in the session, or their save loaded from inside the run)
	// is followed again instead, and nothing is lost.
	inline constexpr auto HOLLOW_JOB_WAIT = std::chrono::minutes(5);

	// What a target of an objective says of the job (ObjectiveHere): nothing when its alias isn't a reference alias of
	// the quest (a keyword, or none) or holds something that isn't an actor (a place's marker, filled by the start and
	// standing whatever became of the actors: it is the actors the job is about); else the alias is a target, standing
	// when its actor is here (enabled, not deleted; alive or dead), gone when it holds nothing or one that isn't.
	enum class Target : std::uint8_t
	{
		kNone,
		kGone,
		kStanding,
	};

	inline Target TargetOf(bool a_refAlias, bool a_held, bool a_actor, bool a_stands)
	{
		if (!a_refAlias || (a_held && !a_actor)) {
			return Target::kNone;
		}
		return a_held && a_stands ? Target::kStanding : Target::kGone;
	}

	// An objective of the quest as it is here (HollowJob): whether it is shown to the player and not done, and of the
	// aliases it points at, how many are targets (TargetOf) and how many of those stand.
	struct ObjectiveHere
	{
		bool        shown = false;
		std::size_t targets = 0;
		std::size_t standing = 0;

		void Add(Target a_target)
		{
			targets += a_target != Target::kNone ? 1 : 0;
			standing += a_target == Target::kStanding ? 1 : 0;
		}
	};

	// BGSQuestObjective::state: displayed (1), with completed (2) or failed (4) set once done. Open: shown and not done.
	inline bool ObjectiveOpen(std::int32_t a_state)
	{
		return (a_state & 1) != 0 && (a_state & 6) == 0;
	}

	// Whether a job with actors made at runtime is hollow here, its player left with an objective and nothing to do it
	// to: an open objective points at nothing that stands, and none of the actors the quest's own start made stands
	// either (a_createdStanding: an enabled one, alive or dead, in an alias the start created). Only a run that was
	// followed looks so: its own actors were removed as they came and its target alias was forced onto the owner's copy,
	// which no save keeps and which goes with the owner or the session. A job on offer shows no objective; one whose
	// target was killed has that objective done and the corpse in the alias while the next points at the giver; a run
	// of this game's own has its actor in the alias its start created, whether or not the target alias is forced yet
	// (To the Mattresses makes seven bosses and keeps the six it didn't pick disabled: those count for nothing).
	inline bool HollowJob(std::span<const ObjectiveHere> a_objectives, bool a_createdStanding)
	{
		return !a_createdStanding && std::ranges::any_of(a_objectives, [](const ObjectiveHere& a_objective) { return a_objective.shown && a_objective.targets > 0 && a_objective.standing == 0; });
	}

	// The jobs with actors made at runtime that are hollow here (HollowJob), and since when nothing has stood in for
	// each. A job's clock runs only while it is hollow with no friend's run of it followed (their actors are what
	// stands, and a followed job is often hollow: our own actors go as they come, and our copy of the owner's stands only
	// while their game has it loaded) and no friend's start of it waiting its turn or under way (it fills the aliases
	// itself), and starts over whenever one does. So a brief absence of the owner (a drop, a load, the session ending
	// and coming back) never counts the time before it: the wait is a wait for the owner's return. Our run is to be run
	// again as this game's own (the quest stopped and started again: its start picks a place and makes its actors afresh,
	// on offer at the giver as a fresh job) once a_friends (a friend's run of it was followed here: a player's own run
	// never is, whatever it looks like) and the clock reaches HOLLOW_JOB_WAIT.
	class HollowClock
	{
	public:
		using Time = std::chrono::steady_clock::time_point;

		// The quest polled: hollow or not, with a friend's run standing in or not (followed, or starting), a friend's or
		// not. True when it is to be run again now.
		bool Poll(std::uint32_t a_quest, bool a_hollow, bool a_standsIn, bool a_friends, Time a_now)
		{
			if (!a_hollow || a_standsIn || !a_friends) {
				since.erase(a_quest);
				return false;
			}
			const auto start = since.try_emplace(a_quest, a_now).first->second;
			return a_now - start >= HOLLOW_JOB_WAIT;
		}

		// How long the quest has been hollow with nothing standing in (none: it isn't, or it is still followed).
		[[nodiscard]] std::optional<std::chrono::steady_clock::duration> HollowFor(std::uint32_t a_quest, Time a_now) const
		{
			const auto it = since.find(a_quest);
			return it != since.end() ? std::optional{ a_now - it->second } : std::nullopt;
		}

		// The quest was run again (or stopped): counted from the start if it is ever hollow again.
		void Forget(std::uint32_t a_quest) { since.erase(a_quest); }

		// Another world was loaded: counted from the load.
		void Clear() { since.clear(); }

	private:
		std::map<std::uint32_t, Time> since;
	};
}
