#pragma once

#include "game/RemoteEdits.h"
#include "game/SpawnBook.h"

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

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
// host's sQuests = safe a SAFE or SILENT one, or a place's fight in waves (QuestFilter::FIGHTS) whatever sQuests says; its stages go between the games, so its creatures' deaths must too), of
// those only one that is running here and runs once or starts with the game (never started twice: a second run's alias
// would be the first's on the wire), of no story or faction line (Quest type none, misc or side: never the main quest's
// or a faction's), no radiant (RadiantRules shares those by their run) nor a quest each game runs alone
// (QuestFilter::PER_GAME), and only a creature: an NPC our player could talk to (Barney, Gun Run's giver, made by its
// alias 25) stays each game's own, its scenes its own game's, and not a vehicle the player sits in (a vertibird: paired,
// it would be mirrored to the owner's while its own flight scene still moves it, and a seated player could be dropped).
// The pairing's wire, the server's refusal and the every-frame rules are those Gun Run, the first quest paired, brought
// (protocol 61). Kept free of the game so it can be tested (tests/QuestActorTests.cpp).
//
// An enemy is paired whatever the quest's line and whatever its race: an NPC the alias created that is hostile to our
// player, as its game has it (Actor::GetHostileToActor). And an alias that holds another quest's alias (an external
// alias) is judged by the alias its chain ends at: When Freedom Calls (Min00, a Minutemen quest, shared) holds the raiders
// that come at the players in Concord's street in its aliases WaveRaider01 to 03, RaiderLookout and RaiderBoss, each
// another quest's: DN092's (The Museum of Freedom, a misc quest that is not shared), whose aliases create them. Each
// game's script switches its own on as its player comes down from the museum's roof. Left each game's own, both games
// shared theirs as spawns, each game had two raiders for each alias, and a friend's shots hurt only the one their own game
// made: in the other game the raider never lost a point (a Nexus report on 0.21.0, "my friend's shots just don't
// register"). An enemy's scenes are its fight, which the game that runs it plays, as for any placed enemy of a story; a
// calm NPC of a story or a faction (a giver, a companion to be), or a human of any quest that is not hostile, stays each
// game's own as before. The quest's other guards hold for an enemy too (running, once, official, no radiant, not
// PER_GAME), and so does the vertibird's; the session need not share the quest (below). No new wire: the spawn names its alias as Gun Run's do, and a game of an
// older build takes it for what it took it for before (it pairs nothing, and places a copy or withholds one as for any spawn).
//
// Every enemy a quest makes in each game's run of it is paired the same way, whatever made it ("twins", below):
// - an alias that creates it, as above;
// - a reference collection alias (BGSRefCollectionAlias, "RefColl") that creates its members: every member names the same
//   alias, so a friend's spawn of it pairs with the nearest of our members of the same NPC that is not paired yet, one to
//   one. Before, a collection was keyed but never paired (0.21.1-rc: Special Delivery's raider reinforcements, Boston
//   After Dark's raiders, the Constitution's mercenaries and scavengers, Far Harbor's Walk in the Park mirelurk waves:
//   the friend's members were shared with nobody), then shared plain (0.21.1: doubled, each game counting only its own
//   kills);
// - an alias or a collection the quest's script put a runtime enemy into (PlaceActorAtMe, then ForceRefTo or AddRef: Far
//   Harbor's gate siege, the Mechanist's robots): the alias is filled the same in every game's run, so it keys the enemy,
//   matched as a script's placing is (the same NPC, shared where ours first stood: Match::kPlaced);
// - an enemy a script placed into no alias at all (a respawn, a teleporting synth): no key, but placed at the same marker
//   in each game, so a friend's spawn of the same NPC that their game shared where ours first stood (SPOT_TWIN) is its twin.
// An enemy's quest need not be one the session shares (sQuests = story, sStory = own): pairing shares only the fight,
// never a stage. Our game holds its own back from a friend's twin, and pairs it with theirs, whenever one is there; when
// both games shared theirs already (one shared plain before it turned hostile, or held by two quests' aliases that each
// game keyed apart, or a script's raider in both), the game whose spawn ranks lower (Yields: plain below keyed, then the
// higher player id) lets go of its own and pairs it with the other's. Both games read the same ranks off the wire, so
// exactly one lets go. The server is as before: it refuses a second game's report of the same alias, and a game never
// sends one that would be refused (ReportAs: a collection's member past the friend's count, or one whose alias the
// friend's game owns, goes plain).
namespace QuestActors
{
	// BGSRefAlias::FILL_TYPE::kCreated: the alias makes its reference as the quest starts.
	inline constexpr std::uint16_t FILL_CREATED = 4;
	// BGSRefAlias::FILL_TYPE::kFromExternal: the alias holds what another quest's alias holds (its fillData: that quest,
	// then that alias's ID; read from the game, Min00's WaveRaider01 holding DN092's alias 10).
	inline constexpr std::uint16_t FILL_EXTERNAL = 5;
	// No fill found (an external alias whose chain could not be followed).
	inline constexpr std::uint16_t FILL_NONE = 0xFFFF;
	// How many external aliases in a row are followed to the alias that fills the last one.
	inline constexpr int EXTERNAL_HOPS = 4;

	// BGSBaseAlias::QType() of a reference alias (RE::BGSRefAlias::TYPE): it holds one reference. A reference collection
	// alias's is "RefColl" (RE::BGSRefCollectionAlias::TYPE); its class is made from BGSRefAlias, so its fill type reads as
	// a reference alias's does (a collection that creates its members is FILL_CREATED too).
	inline constexpr std::string_view REF_ALIAS = "Ref";
	inline constexpr std::string_view COLLECTION_ALIAS = "RefColl";

	enum class AliasKind : std::uint8_t
	{
		kOther,       // a location alias, or anything else: holds no NPC
		kRef,         // one reference
		kCollection,  // any number of references
	};

	constexpr AliasKind KindOf(std::string_view a_type)
	{
		return a_type == REF_ALIAS ? AliasKind::kRef : a_type == COLLECTION_ALIAS ? AliasKind::kCollection : AliasKind::kOther;
	}

	// The fill an alias of that kind (its QType) is keyed by: a reference alias's or a collection's own, FILL_NONE for any
	// other kind.
	constexpr std::uint16_t AliasFill(std::string_view a_type, std::uint16_t a_fill)
	{
		return KindOf(a_type) != AliasKind::kOther ? a_fill : FILL_NONE;
	}

	// How a friend's spawn of an alias that keys our NPC is matched with ours (KeyTwin). A single alias that created its
	// NPC holds that one alone, rolled from the same leveled NPC in every game; a collection holds several, and an alias
	// the quest's script fills could be filled with another NPC later.
	enum class Match : std::uint8_t
	{
		kAlias,   // a single alias that created its NPC: the alias alone
		kNear,    // a collection that created its members: the same NPC, standing near (StandsFor, PAIR_RANGE)
		kPlaced,  // an alias filled with an NPC made elsewhere (a script's, forced in, or found by conditions): the same NPC,
		          // near, and shared by their game where ours first stood (SPOT_TWIN), as a script places it in each game.
		          // Without the spot, a quest that gathers enemies near the player (one that starts with the game and
		          // collects hostiles by conditions) would pair two raiders of one leveled list that only stand near.
	};

	constexpr Match MatchOf(AliasKind a_kind, std::uint16_t a_keyFill)
	{
		if (a_keyFill != FILL_CREATED) {
			return Match::kPlaced;
		}
		return a_kind == AliasKind::kRef ? Match::kAlias : Match::kNear;
	}

	// The fill an alias is judged by: its own, or for one that holds another quest's alias, the fill of the alias its
	// chain ends at (a_sourceFill: FILL_NONE when it could not be followed). Min00's wave raiders are created by DN092 (The
	// Museum of Freedom, a misc quest not shared) and held by Min00's external aliases: keyed with Min00's alias, which
	// both games run as the same story, as if Min00 had created them.
	constexpr std::uint16_t KeyFill(std::uint16_t a_fill, std::uint16_t a_sourceFill)
	{
		return a_fill == FILL_EXTERNAL ? a_sourceFill : a_fill;
	}

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
	// game starts it, and no event starts it again. So is one QuestFilter::RUNS_ONCE names (Vault 75's DN143). a_enemy:
	// for an NPC hostile to our player, a quest of any line (the main quest's, a faction's, an add-on's story) pairs it,
	// and so does one the session does not share (sQuests = story, sStory = own): each game runs its own stages, and the
	// pairing shares only the fight (the enemy dies in both games, and each game's script hears of its own enemy's death).
	constexpr bool QuestPairs(const Quest& a_quest, bool a_enemy = false)
	{
		const bool typeOk = a_enemy || a_quest.type == QUEST_TYPE_NONE || a_quest.type == QUEST_TYPE_MISC || a_quest.type == QUEST_TYPE_SIDE;
		const bool once = (a_quest.flags & (QUEST_RUN_ONCE | QUEST_STARTS_ENABLED)) != 0 || a_quest.runsOnce;
		return (a_enemy || a_quest.shared) && a_quest.official && (a_quest.flags & QUEST_RUNNING) != 0 && once && typeOk && !a_quest.radiant && !a_quest.perGame;
	}

	// Whether our runtime NPC, held by an alias of that fill type (KeyFill: an external alias's is its chain's) in such a
	// quest, of a race with those flags and that form ID, is named with the alias as we share it: one the alias created, a
	// creature or an enemy of any race (a_enemy: hostile to our player); or an enemy the alias holds however it was filled
	// (the quest's script placed it and put it there, the same in every game's run). An alias ID past what the wire holds
	// is not, nor is a vertibird.
	constexpr bool Keyed(const Quest& a_quest, std::uint16_t a_fillType, std::uint32_t a_raceFlags, std::uint32_t a_raceForm, std::uint32_t a_alias, bool a_enemy = false)
	{
		const bool made = a_fillType == FILL_CREATED && (a_raceFlags & RACE_ALLOWS_DIALOGUE) == 0;
		return (a_enemy || made) && a_raceForm != VERTIBIRD_RACE && a_alias <= 0xFFFF && QuestPairs(a_quest, a_enemy);
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

	// Whether a friend's live spawn names the alias (a_spawn's quest and alias).
	constexpr bool Twin(const Protocol::Spawn& a_spawn, bool a_over, std::uint32_t a_quest, std::uint16_t a_alias)
	{
		return !a_over && a_quest != 0 && a_spawn.quest == a_quest && a_spawn.alias == a_alias;
	}

	// ---- Twins: a friend's spawn and our own NPC that are the same quest enemy -------------------------------------

	// A script that places an enemy in no alias places it at the same marker in each game: a friend's spawn their game
	// shared this near where our game first saw ours, of the same NPC, is its twin. Wide enough for the first scan to see
	// it a step or two from its marker, narrow beside the 4096 a quest's own alias pairs within.
	inline constexpr float SPOT_TWIN = 512.0f;

	// One of the aliases (a quest's alias that holds our NPC and keys it, Keyed) our NPC is named with, by quest form ID
	// and alias ID.
	struct Key
	{
		std::uint32_t quest = 0;  // 0: none
		std::uint16_t alias = 0;

		constexpr bool operator==(const Key&) const = default;
		constexpr bool operator<(const Key& a_other) const { return quest != a_other.quest ? quest < a_other.quest : alias < a_other.alias; }
	};

	// The key our NPC is reported under, of all the aliases that key it: the lowest, so two games whose NPC is held by the
	// same aliases name it the same however each game's alias array is ordered (DN092's Concord raiders are Min00's and
	// MQ102's). Quest 0 for none.
	template <class Keys>
	constexpr Key Canonical(const Keys& a_keys)
	{
		Key out;
		for (const Key& key : a_keys) {
			if (key.quest != 0 && (out.quest == 0 || key < out)) {
				out = key;
			}
		}
		return out;
	}

	// What our own runtime NPC is, as a friend's spawn is matched against it.
	struct Own
	{
		bool              enemy = false;         // hostile to our player
		bool              otherSystem = false;   // in an alias of a quest another system runs (a radiant, an encounter, PER_GAME, a settlement attack)
		RemoteEdits::Spot firstAt;               // where our game first saw it standing
		RemoteEdits::Spot at;                    // where it stands
		std::uint32_t     base = 0;              // what a copy of it would be placed from (Spawns' BaseToPlace)
		std::uint32_t     original = 0;          // the leveled NPC it was placed as (0: none)
		std::uint32_t     race = 0;              // its race (0: none)
	};

	// What a friend's spawn is, as matched.
	struct Theirs
	{
		bool              over = false;      // it died, or its owner's game let go of it
		bool              keyResolves = false;  // it names an alias that keys an NPC in our game (a quest we run, the alias's guards pass)
		RemoteEdits::Spot sharedAt;          // where their game had it when it was shared (Protocol::Spawn)
		RemoteEdits::Spot at;                // where their game has it now
		std::uint32_t     base = 0;
		std::uint32_t     original = 0;      // their roll's leveled NPC (0: not known yet)
		std::uint32_t     race = 0;          // the race of their base as our game has it (0: not known)
	};

	// Ours and theirs are of the same NPC (placed from the same base, or rolled from the same leveled NPC).
	inline bool SameNpc(const Own& a_own, const Theirs& a_theirs)
	{
		return SpawnBook::StandsFor(a_own.base, a_own.original, a_theirs.base, a_theirs.original);
	}

	// Ours and theirs stand near each other now.
	inline bool Near(const Own& a_own, const Theirs& a_theirs)
	{
		return RemoteEdits::Within(RemoteEdits::Distance(a_own.at, a_theirs.at), PAIR_RANGE);
	}

	// Ours and theirs are of the same NPC and stand near each other now.
	inline bool SameNear(const Own& a_own, const Theirs& a_theirs)
	{
		return SameNpc(a_own, a_theirs) && Near(a_own, a_theirs);
	}

	// Ours and theirs are of one race (both known).
	constexpr bool SameRace(const Own& a_own, const Theirs& a_theirs)
	{
		return a_own.race != 0 && a_own.race == a_theirs.race;
	}

	// Their game shared theirs where ours first stood.
	inline bool SameSpot(const Own& a_own, const Theirs& a_theirs)
	{
		return RemoteEdits::Within(RemoteEdits::Distance(a_own.firstAt, a_theirs.sharedAt), SPOT_TWIN);
	}

	// A key twin: their spawn names an alias of ours that keys our NPC (a_holds), matched as a_match says. Past a created
	// single alias, ours is no NPC another system runs (a random encounter's, a radiant's, a settlement attack's, held by
	// a quest's alias as well): that system shares or holds it.
	inline bool KeyTwin(const Own& a_own, const Theirs& a_theirs, bool a_holds, Match a_match)
	{
		if (a_theirs.over || !a_holds) {
			return false;
		}
		switch (a_match) {
		case Match::kAlias:
			return true;
		case Match::kNear:
			// Each game rolls each member of a collection on its own, so the two games' members need not be of the same NPCs
			// (Special Delivery's three raiders, two of three paired by NPC on the rig, 2026-11-23): one of the same race stands
			// for theirs when none of the same NPC is left (Pick takes the same NPC first), as a created single alias pairs
			// whatever each game rolled.
			return !a_own.otherSystem && (SameNpc(a_own, a_theirs) || SameRace(a_own, a_theirs)) && Near(a_own, a_theirs);
		case Match::kPlaced:
			return !a_own.otherSystem && SameNear(a_own, a_theirs) && SameSpot(a_own, a_theirs);
		}
		return false;
	}

	// A spot twin: their spawn names no alias our game keys anything with, ours is an enemy no other system runs, of the
	// same NPC, their game shared theirs within SPOT_TWIN of where ours first stood, and the two stand near each other now
	// (PAIR_RANGE): one that wandered off before it was shared stays a spawn of its own, shared, rather than one held
	// back for a twin it could never pair with (Pick pairs only within PAIR_RANGE).
	inline bool SpotTwin(const Own& a_own, const Theirs& a_theirs)
	{
		return !a_theirs.over && !a_theirs.keyResolves && a_own.enemy && !a_own.otherSystem && SameNear(a_own, a_theirs) && SameSpot(a_own, a_theirs);
	}

	// Both games shared their twin already: whether ours lets go of its spawn and pairs with theirs. A keyed spawn
	// outranks a plain one (the plain one was shared before its key was there, or by a game that keys nothing: an older
	// build); between two of a kind the lower player id keeps its own. Both games read the same facts, so one lets go.
	constexpr bool Yields(bool a_oursKeyed, std::uint32_t a_ourPlayer, bool a_theirsKeyed, std::uint32_t a_theirPlayer)
	{
		if (a_oursKeyed != a_theirsKeyed) {
			return !a_oursKeyed;
		}
		return a_ourPlayer > a_theirPlayer;
	}

	// How our NPC, not shared yet, is shared now.
	enum class ShareAs : std::uint8_t
	{
		kWait,   // a friend's live spawn that no NPC of ours is paired with is its twin: not shared, and it pairs with that one
		kKeyed,  // named with its key (kQuestActor)
		kPlain,  // shared plain: no key, or the key's alias is a friend's game's already (a collection's member past their
		         // count, an alias whose spawn is dead there): the server would refuse it keyed
	};

	// a_waitingTwin: a friend's live spawn not paired here is its twin (KeyTwin or SpotTwin). a_keyTaken: a friend's
	// spawn, live or not, names our key (the server holds it until its owner lets go of it).
	constexpr ShareAs ReportAs(const Key& a_key, bool a_waitingTwin, bool a_keyTaken)
	{
		if (a_waitingTwin) {
			return ShareAs::kWait;
		}
		return a_key.quest != 0 && !a_keyTaken ? ShareAs::kKeyed : ShareAs::kPlain;
	}

	// One of our NPCs that could pair with a friend's spawn, as Pick weighs it.
	struct Candidate
	{
		bool              twin = false;     // KeyTwin or SpotTwin
		bool              ready = false;    // alive, switched on, with its 3D, our game's own (no copy, stand-in, puppet or companion)
		bool              paired = false;   // paired with another friend's spawn already
		bool              shared = false;   // our game shares it (or reported it and awaits the answer)
		bool              yields = false;   // ... and lets it go for theirs (Yields)
		bool              exact = true;     // of the same NPC as theirs (SameNpc), not only of its race: taken first
		RemoteEdits::Spot at;
	};

	// The one of a_candidates that pairs with a friend's spawn standing at a_theirsAt: of the twins that are ready and not
	// paired, shared by nobody or letting go for theirs, within PAIR_RANGE: the nearest of the same NPC, else the nearest. None:
	// a_candidates.size().
	template <class Candidates>
	std::size_t Pick(const Candidates& a_candidates, const RemoteEdits::Spot& a_theirsAt)
	{
		std::size_t best = std::size(a_candidates);
		bool        bestExact = false;
		float       nearest = 0.0f;
		std::size_t i = 0;
		for (const Candidate& c : a_candidates) {
			const float d = RemoteEdits::Distance(c.at, a_theirsAt);
			if (c.twin && c.ready && !c.paired && (!c.shared || c.yields) && RemoteEdits::Within(d, PAIR_RANGE)) {
				if (best == std::size(a_candidates) || (c.exact && !bestExact) || (c.exact == bestExact && d < nearest)) {
					best = i;
					bestExact = c.exact;
					nearest = d;
				}
			}
			++i;
		}
		return best;
	}
}
