#pragma once

#include "game/RemoteEdits.h"

#include "Protocol.h"

#include <array>
#include <cstdint>
#include <optional>

// Creatures a quest's alias creates at runtime (the alias's "create reference" fill), the same in every game's run of
// that quest: Salem's three mirelurks in Gun Run (DN083_Barney's StartMirelurk01 to 03). Each game's run creates its
// own, under ids that are that game's own (0xFF......), so Spawns took a friend's for an NPC our game never made: no
// copy of it (its base is unique), and ours stood idle beside it, unhurt by what the friend shot, its quest never told
// of its death. The game that shares one first (Spawns: hostile or fighting) names the alias with it (Protocol::Spawn's
// quest and alias), and a friend's game pairs its own creature from that alias with the spawn: mirrored as a copy is
// (NpcSync), dying when it dies there (so that game's alias scripts count the death too), but never deleted, placed or
// kept out of a save, as it is that game's own quest's. The server refuses a second game's report of the same alias.
//
// Guarded so it cannot touch a story: only a quest LISTED (Gun Run alone, until the rig shows more need it), and of those
// only one that runs once, of no story or faction line (Quest type none, misc or side: never the main quest's or a
// faction's), no radiant (RadiantRules shares those by their run) nor a quest each game runs alone (QuestFilter::PER_GAME),
// and only a creature: an NPC our player could talk to (Barney, the quest's giver, made by its alias 25) stays each game's
// own, its scenes its own game's. Kept free of the game so it can be tested (tests/QuestActorTests.cpp).
namespace QuestActors
{
	// The quests whose created creatures pair: a file (StageRules::File) and the quest's form ID in it.
	struct Listed
	{
		std::uint8_t  file;
		std::uint32_t quest;
	};
	inline constexpr std::array<Listed, 1> LISTED{ {
		{ 0, 0x1243D6 },  // Fallout4.esm DN083_Barney (Gun Run): Salem's mirelurks, aliases 26, 38 and 41
	} };

	constexpr bool IsListed(std::optional<std::uint8_t> a_file, std::uint32_t a_quest)
	{
		for (const auto& entry : LISTED) {
			if (a_file && entry.file == *a_file && entry.quest == (a_quest & 0x00FFFFFF)) {
				return true;
			}
		}
		return false;
	}

	// BGSRefAlias::FILL_TYPE::kCreated: the alias makes its reference as the quest starts.
	inline constexpr std::uint16_t FILL_CREATED = 4;

	// TESQuest::data.flags: running, and runs once (it can't be started again once it stops).
	inline constexpr std::uint16_t QUEST_RUNNING = 0x0001;
	inline constexpr std::uint16_t QUEST_RUN_ONCE = 0x0100;

	// TESQuest::data.questType: none, misc and side quests. 1 to 5 are the main quest's and the factions' (the
	// Brotherhood, the Institute, the Minutemen, the Railroad), 8 and up the add-ons' stories.
	inline constexpr std::int8_t QUEST_TYPE_NONE = 0;
	inline constexpr std::int8_t QUEST_TYPE_MISC = 6;
	inline constexpr std::int8_t QUEST_TYPE_SIDE = 7;

	// RACE_DATA::flags "allow PC dialogue": set on HumanRace, GhoulRace and DogmeatRace in Fallout4.esm, clear on
	// MirelurkRace (read from the game's files).
	inline constexpr std::uint32_t RACE_ALLOWS_DIALOGUE = 0x00200000;

	// A friend's game's creature and ours stand this close at most to be paired (both are created at the same marker).
	inline constexpr float PAIR_RANGE = 4096.0f;

	// What a quest that holds one of our runtime NPCs in an alias is.
	struct Quest
	{
		bool          listed = false;    // in LISTED (IsListed)
		bool          official = false;  // from the game's own files (Official::IsOfficialForm)
		std::uint16_t flags = 0;         // TESQuest::data.flags
		std::int8_t   type = 0;          // TESQuest::data.questType
		bool          radiant = false;   // RadiantSync::IsRadiant (by file and form ID in it)
		bool          perGame = false;   // QuestFilter::IsPerGame
	};

	// Whether a quest's created creatures are paired across games.
	constexpr bool QuestPairs(const Quest& a_quest)
	{
		const bool typeOk = a_quest.type == QUEST_TYPE_NONE || a_quest.type == QUEST_TYPE_MISC || a_quest.type == QUEST_TYPE_SIDE;
		return a_quest.listed && a_quest.official && (a_quest.flags & QUEST_RUNNING) != 0 && (a_quest.flags & QUEST_RUN_ONCE) != 0 && typeOk && !a_quest.radiant && !a_quest.perGame;
	}

	// Whether our NPC, held by an alias of that fill type in such a quest, of a race with those flags, is named with the
	// alias as we share it. An alias ID past what the wire holds is not.
	constexpr bool Keyed(const Quest& a_quest, std::uint16_t a_fillType, std::uint32_t a_raceFlags, std::uint32_t a_alias)
	{
		return a_fillType == FILL_CREATED && (a_raceFlags & RACE_ALLOWS_DIALOGUE) == 0 && a_alias <= 0xFFFF && QuestPairs(a_quest);
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
