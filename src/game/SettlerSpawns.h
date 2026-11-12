#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>

// The settlers the host's game made at run time, seen by friends (the shared workshop, bSharedWorkshop). A settlement's
// first settlers are a plugin's references, the same in every game, and NpcSync mirrors them. One a recruitment beacon
// brought, or a quest moved in, is a reference the host's game made (Protocol::IsSpawnRef): no friend's game has it, and
// until now a friend saw it only while it fought (a fight's spawn). At a settlement the host's game runs it is shared calm,
// from the moment the host's game has it loaded, as a spawn of a kind of its own (Protocol::SpawnKind::kSettler) that names
// its workbench where a quest actor names its quest: a friend's game places a copy and the host's game's states move it, as
// for any spawn. The host's game names it among its settlers under its spawn id (common/SettlerRoster.h), so a friend's
// workshop mode offers Command on the copy, and a friend's job for it (SettlerRequests.h) names it by its spawn id.
//
// A settlement has twenty-odd settlers and a game shares at most Protocol::MAX_SPAWNS_PER_PLAYER spawns, a fight's worth.
// So a settler takes none of those: the kind has Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER places of its own, counted apart
// in the game's own book (SpawnBook::Ours) and by the server, and a fight at a full settlement still has its whole share.
// Only the host's own game runs a settlement, so its share is the session's (MAX_IN_SESSION). What it costs: one Spawn
// message (about 45 bytes) per settler when it is first shared and in each welcome, its states at the NPC stream's rate
// while the host's game has it loaded, and one more actor in each friend's game near it.
//
// These are the rules of the three ends (the host's game, the server, a friend's game), game-free, for the tests.
namespace SettlerSpawns
{
	// Settler spawns the session holds at once: the host's own game's share (no other game shares any).
	inline constexpr std::size_t MAX_IN_SESSION = Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER;

	[[nodiscard]] constexpr bool IsSettler(Protocol::SpawnKind a_kind)
	{
		return a_kind == Protocol::SpawnKind::kSettler;
	}

	// ---- The host's game: which of its runtime NPCs is a settler to share ------------------------------------

	struct Facts
	{
		bool          serves = false;            // the rule is in force and ours is the host's own game (SharedWorkshop::Serves)
		std::uint32_t bench = 0;                 // what the actor is linked to as a workshop's own (WorkshopItemKeyword), 0: nothing
		bool          benchIsWorkbench = false;  // ...and that is a settlement's workbench
		bool          benchOurs = false;         // ...which our player owns
		bool          alive = false;
		bool          companion = false;         // our player's companion now: the companion handling's, never a settler's
		bool          built = false;             // a turret or a robot: a piece workshop mode builds, shared as the pieces are
	};

	// The workbench to name the actor's spawn with, 0 when it is not shared as a settler. A workbench a plugin places only
	// (the same reference in every game): one made at run time names nothing a friend's game has.
	[[nodiscard]] constexpr std::uint32_t BenchToName(const Facts& a_facts)
	{
		const bool settler = a_facts.serves && a_facts.alive && !a_facts.companion && !a_facts.built && a_facts.benchIsWorkbench && a_facts.benchOurs &&
		                     Protocol::IsShareableRef(a_facts.bench);
		return settler ? a_facts.bench : 0;
	}

	// How a settler is offered, given whether a settler's place is free and whether it fights now (or is hostile to our
	// player: what makes any runtime NPC a fight's spawn).
	enum class As : std::uint8_t
	{
		kSettler,  // calm or fighting: one of the settlers' places
		kFight,    // no settler's place left, and it fights: as before the rule, one of a fight's places (a plain spawn)
		kWait,     // no settler's place left, and calm: not shared now (said once in the log, counted)
	};

	[[nodiscard]] constexpr As OfferAs(bool a_settlerRoom, bool a_fights)
	{
		return a_settlerRoom ? As::kSettler : a_fights ? As::kFight : As::kWait;
	}

	// ---- Places: the settlers' and the others' are counted apart ---------------------------------------------

	struct Held
	{
		std::size_t settlers = 0;
		std::size_t others = 0;
	};

	// Whether one more of that kind has a place beside what is held.
	[[nodiscard]] constexpr bool Room(bool a_settler, const Held& a_held, std::size_t a_maxSettlers, std::size_t a_maxOthers)
	{
		return a_settler ? a_held.settlers < a_maxSettlers : a_held.others < a_maxOthers;
	}

	// ---- The server and a friend's game: whose word a settler is ---------------------------------------------

	// A spawn as it is taken: a settler only when it may be one. The server allows the host's own game's under the rule
	// (no other game runs a settlement, and no other may take the settlers' places); a friend's game allows one from the
	// host's game that names a workbench in its own files. Anything else is a plain spawn, placed as any.
	[[nodiscard]] constexpr Protocol::Spawn Taken(Protocol::Spawn a_spawn, bool a_allowed)
	{
		if (IsSettler(a_spawn.kind) && !a_allowed) {
			a_spawn.kind = Protocol::SpawnKind::kPlain;
			a_spawn.quest = 0;
			a_spawn.alias = 0;
		}
		return a_spawn;
	}
}
