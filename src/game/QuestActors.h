#pragma once

#include "game/RemoteEdits.h"

#include "Protocol.h"

#include <cstdint>

// Creatures a quest's alias creates at runtime (the alias's "create reference" fill), the same in every game's run of
// that quest: Salem's three mirelurks in Gun Run (DN083_Barney's StartMirelurk01 to 03). Each game's run creates its
// own, under ids that are that game's own (0xFF......), so Spawns took a friend's for an NPC our game never made: no
// copy of it (its base is unique), and ours stood idle beside it, unhurt by what the friend shot, its quest never told
// of its death. The game that shares one first (Spawns: hostile or fighting) names the alias with it (Protocol::Spawn's
// quest and alias), and a friend's game pairs its own creature from that alias with the spawn: mirrored as a copy is
// (NpcSync), dying when it dies there (so that game's alias scripts count the death too), but never deleted, placed or
// kept out of a save, as it is that game's own quest's. The server refuses a second game's report of the same alias.
//
// Guarded so it cannot touch a story: only a quest this session shares (QuestSync::SharesHere: a side quest, or with the
// host's sQuests = safe a SAFE or SILENT one; its stages go between the games, so its creatures' deaths must too), of
// those only one that is running here and runs once or starts with the game (never started twice: a second run's alias
// would be the first's on the wire), of no story or faction line (Quest type none, misc or side: never the main quest's
// or a faction's), no radiant (RadiantRules shares those by their run) nor a quest each game runs alone
// (QuestFilter::PER_GAME), and only a creature: an NPC our player could talk to (Barney, Gun Run's giver, made by its
// alias 25) stays each game's own, its scenes its own game's, and not a vehicle the player sits in (a vertibird: paired,
// it would be mirrored to the owner's while its own flight scene still moves it, and a seated player could be dropped).
// The pairing's wire, the server's refusal and the every-frame rules are those Gun Run, the first quest paired, brought
// (protocol 61). Kept free of the game so it can be tested (tests/QuestActorTests.cpp).
namespace QuestActors
{
	// BGSRefAlias::FILL_TYPE::kCreated: the alias makes its reference as the quest starts.
	inline constexpr std::uint16_t FILL_CREATED = 4;

	// TESQuest::data.flags (CommonLibF4's RE::QuestFlag): running (kEnabled), starts with the game (kStartsEnabled), and
	// runs once (kRunOnce: it can't be started again once it stops).
	inline constexpr std::uint16_t QUEST_RUNNING = 0x0001;
	inline constexpr std::uint16_t QUEST_STARTS_ENABLED = 0x0010;
	inline constexpr std::uint16_t QUEST_RUN_ONCE = 0x0100;

	// TESQuest::data.questType: none, misc and side quests. 1 to 5 are the main quest's and the factions' (the
	// Brotherhood, the Institute, the Minutemen, the Railroad), 8 and up the add-ons' stories.
	inline constexpr std::int8_t QUEST_TYPE_NONE = 0;
	inline constexpr std::int8_t QUEST_TYPE_MISC = 6;
	inline constexpr std::int8_t QUEST_TYPE_SIDE = 7;

	// RACE_DATA::flags "allow PC dialogue": set on HumanRace, GhoulRace and DogmeatRace in Fallout4.esm, clear on
	// MirelurkRace (read from the game's files).
	inline constexpr std::uint32_t RACE_ALLOWS_DIALOGUE = 0x00200000;

	// VertibirdRace in Fallout4.esm (load index 0, so the full form ID): the player sits in a vertibird, yet it allows no
	// PC dialogue, so the creature rule alone would pair it.
	inline constexpr std::uint32_t VERTIBIRD_RACE = 0x000D77E3;

	// A friend's game's creature and ours stand this close at most to be paired (both are created at the same marker).
	inline constexpr float PAIR_RANGE = 4096.0f;

	// What a quest that holds one of our runtime NPCs in an alias is.
	struct Quest
	{
		bool          shared = false;    // the session shares its stages now (QuestSync::SharesHere)
		bool          official = false;  // from the game's own files (Official::IsOfficialForm)
		std::uint16_t flags = 0;         // TESQuest::data.flags
		std::int8_t   type = 0;          // TESQuest::data.questType
		bool          radiant = false;   // RadiantSync::IsRadiant (by file and form ID in it)
		bool          perGame = false;   // QuestFilter::IsPerGame
		bool          runsOnce = false;  // QuestFilter::IsRunsOnce: no run-once flag, but read in the game's files as running once
	};

	// Whether a quest's created creatures are paired across games. One that starts with the game but has no run-once flag
	// (Fallout4.esm's MS13 and DN054, read from the file's flags) is taken as running once per game all the same: the new
	// game starts it, and no event starts it again. So is one QuestFilter::RUNS_ONCE names (Vault 75's DN143).
	constexpr bool QuestPairs(const Quest& a_quest)
	{
		const bool typeOk = a_quest.type == QUEST_TYPE_NONE || a_quest.type == QUEST_TYPE_MISC || a_quest.type == QUEST_TYPE_SIDE;
		const bool once = (a_quest.flags & (QUEST_RUN_ONCE | QUEST_STARTS_ENABLED)) != 0 || a_quest.runsOnce;
		return a_quest.shared && a_quest.official && (a_quest.flags & QUEST_RUNNING) != 0 && once && typeOk && !a_quest.radiant && !a_quest.perGame;
	}

	// Whether our NPC, held by an alias of that fill type in such a quest, of a race with those flags and that form ID, is
	// named with the alias as we share it. An alias ID past what the wire holds is not, nor is a vertibird.
	constexpr bool Keyed(const Quest& a_quest, std::uint16_t a_fillType, std::uint32_t a_raceFlags, std::uint32_t a_raceForm, std::uint32_t a_alias)
	{
		return a_fillType == FILL_CREATED && (a_raceFlags & RACE_ALLOWS_DIALOGUE) == 0 && a_raceForm != VERTIBIRD_RACE && a_alias <= 0xFFFF && QuestPairs(a_quest);
	}

	// A friend's spawn named with an alias, and what our game has in that alias of our run of the quest.
	struct PairFacts
	{
		bool             keyed = false;      // the spawn names a quest alias (Protocol::Spawn::quest)
		bool             over = false;       // it died or its owner's game let go of it
		bool             questPairs = false; // our run of the quest is one whose creatures pair (QuestPairs)
		bool             ours = false;       // the alias holds one of our runtime NPCs: alive, loaded, no stand-in or puppet
		bool             creature = false;   // ... of a race our player can't talk to
		bool             shared = false;     // ... which our game shares already as ours (or reported it), or holds as a copy
		RemoteEdits::Spot oursAt;            // where ours stands
		RemoteEdits::Spot theirsAt;          // where their game has theirs
		RemoteEdits::Spot player;            // our player
	};

	// Whether ours is paired with their spawn now: a_placeRange is how near our player must be to where their game has
	// it, as for placing a copy (SpawnBook::PLACE_RANGE).
	inline bool Pairs(const PairFacts& a_facts, float a_placeRange)
	{
		return a_facts.keyed && !a_facts.over && a_facts.questPairs && a_facts.ours && a_facts.creature && !a_facts.shared &&
		       RemoteEdits::Within(RemoteEdits::Distance(a_facts.oursAt, a_facts.theirsAt), PAIR_RANGE) &&
		       RemoteEdits::Within(RemoteEdits::Distance(a_facts.theirsAt, a_facts.player), a_placeRange);
	}

	// What becomes of a pairing each frame.
	enum class Keep : std::uint8_t
	{
		kKeep,    // still paired
		kUnpair,  // ours goes back to our game alone (mirroring stops, its AI is its own again): never deleted
	};

	struct KeepFacts
	{
		bool              there = false;   // ours is still the NPC paired (its handle finds it, under the same id)
		bool              loaded = false;  // ... with its 3D
		bool              gone = false;    // their game let go of theirs (unloaded, or they left)
		bool              dead = false;    // it died
		RemoteEdits::Spot oursAt;
		RemoteEdits::Spot player;
	};

	// A pairing ends when ours is gone or unloaded, when our player left it behind (KEEP_RANGE), or when their game let
	// go of a living one: ours is then our game's own to run, and to share (its own report names the alias again). A dead
	// one stays paired while it is near: its death is the session's.
	inline Keep Decide(const KeepFacts& a_facts, float a_keepRange)
	{
		if (!a_facts.there || !a_facts.loaded || (a_facts.gone && !a_facts.dead)) {
			return Keep::kUnpair;
		}
		return RemoteEdits::Within(RemoteEdits::Distance(a_facts.oursAt, a_facts.player), a_keepRange) ? Keep::kKeep : Keep::kUnpair;
	}

	// Whether a friend's live spawn names the alias (a_spawn's quest and alias): our game then does not report its own
	// creature from that alias (the server would refuse it), and pairs it instead.
	constexpr bool Twin(const Protocol::Spawn& a_spawn, bool a_over, std::uint32_t a_quest, std::uint16_t a_alias)
	{
		return !a_over && a_quest != 0 && a_spawn.quest == a_quest && a_spawn.alias == a_alias;
	}
}
