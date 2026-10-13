#pragma once

#include "game/FoundDead.h"

#include "Protocol.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// What a game believes of another player's changes to its world. The server only passes on what each
// player says (it can't see anyone's world), so each game judges what arrives by what it can see: its
// own copy of the thing, and where the sender last said they were. A friend's game that misbehaves (or
// is modified) can then change only what an honest game could have: near where its player stands, and
// not another player's story. Kept free of the game so the rules can be tested.
namespace RemoteEdits
{
	// Where someone or something is: an interior cell (worldspace 0), or a worldspace (cell 0), and where in it.
	struct Spot
	{
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0.0f, y = 0.0f, z = 0.0f;
	};

	inline Spot SpotOf(const Protocol::PlayerState& a_state)
	{
		return { a_state.cell, a_state.worldspace, a_state.x, a_state.y, a_state.z };
	}

	inline Spot SpotOf(const Protocol::ActorState& a_state)
	{
		return { a_state.cell, a_state.worldspace, a_state.x, a_state.y, a_state.z };
	}

	constexpr bool SameSpace(const Spot& a_a, const Spot& a_b)
	{
		return a_a.cell == a_b.cell && a_a.worldspace == a_b.worldspace;
	}

	// How far apart, in game units; NaN when they are in different spaces (coordinates in one cell are
	// nowhere in another), so every range test below fails for them.
	inline float Distance(const Spot& a_a, const Spot& a_b)
	{
		if (!SameSpace(a_a, a_b)) {
			return std::numeric_limits<float>::quiet_NaN();
		}
		const float dx = a_a.x - a_b.x, dy = a_a.y - a_b.y, dz = a_a.z - a_b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// Written so that NaN and infinity fail (a comparison with NaN is always false).
	constexpr bool Within(float a_distance, float a_range)
	{
		return a_distance >= 0.0f && a_distance <= a_range;
	}

	// ---- Story characters (WorldSync, NpcSync) ---------------------------------------------------
	//
	// With everyone on their own story, a character of one player's story is not another player's to
	// change: a friend's game switching one on, or killing one away from us, says nothing about ours.
	// The same rule FoundDead::Shared applies to the host's save.

	// A friend's game runs an NPC that is switched off in ours: switched on here too, unless it is a
	// character of our own story (a companion or quest character our story has not brought in yet).
	constexpr bool MaySwitchOn(bool a_sharedStory, const FoundDead::Npc& a_npc)
	{
		return a_sharedStory || !FoundDead::IsStoryCharacter(a_npc);
	}

	// A friend's companion is moved to where their game says it is (from a cell we haven't loaded, or after
	// its owner's copy ran off) only while that is beside the friend's own stand-in: a companion follows its
	// player. Its uniqueness is no reason to leave it behind (every vanilla companion is unique).
	constexpr float OWNER_NEAR = 1500.0f;

	constexpr bool OwnerNear(float a_distance)
	{
		return Within(a_distance, OWNER_NEAR);
	}

	// a_owner: where its owner last said they were (nothing if never heard from).
	inline bool OwnerNear(const std::optional<Spot>& a_owner, const Spot& a_spot)
	{
		return a_owner && OwnerNear(Distance(*a_owner, a_spot));
	}

	// A death the reporter could have seen: they stood in the actor's cell or worldspace, this close to
	// our copy. (Measured from our copy, not from where the reporter says the corpse lies: that comes
	// from them too.)
	constexpr float DEATH_WITNESS_RANGE = 8192.0f;

	constexpr bool Witnessed(bool a_sameSpace, float a_distance)
	{
		return a_sameSpace && Within(a_distance, DEATH_WITNESS_RANGE);
	}

	// a_reporter: where the reporter was (nothing if unknown, as for a death replayed from the session).
	inline bool Witnessed(const std::optional<Spot>& a_reporter, const Spot& a_actor)
	{
		return a_reporter && Witnessed(SameSpace(*a_reporter, a_actor), Distance(*a_reporter, a_actor));
	}

	// Whether a death another player reported kills our copy. Anyone else dies; a unique or essential
	// character of our own story only when the reporter was there. Being in a quest's alias is no reason
	// to keep one alive: radiant quests put generic raiders in aliases.
	constexpr bool MayKill(bool a_sharedStory, const FoundDead::Npc& a_npc, bool a_witnessed)
	{
		return a_sharedStory || !(a_npc.unique || a_npc.essential) || a_witnessed;
	}

	// The corpse is put where it fell in the reporter's game only from this close; farther, it dies where
	// ours stands (a report can't pull an NPC across the map).
	constexpr float CORPSE_MOVE_RANGE = 4096.0f;

	constexpr bool MayMoveCorpse(float a_distance)
	{
		return Within(a_distance, CORPSE_MOVE_RANGE);
	}

	// ---- Explosions (WeaponFire) ----------------------------------------------------------------
	//
	// A grenade is about 500 units across, a Fat Man's 850. Nothing bigger is thrown (a nuke or an
	// artillery strike reaches thousands), and a radius of 0 or one that isn't a number is no explosion
	// of a throw.
	constexpr float MAX_EXPLOSION_RADIUS = 1024.0f;
	constexpr float EXPLOSION_SLACK = 128.0f;  // a body is not a point

	// a_distance: from the explosion to the nearest of us and our companions (NaN counts as near).
	// a_friendlyFire: the host lets players hurt each other.
	constexpr bool MaySetOff(float a_radius, float a_distance, bool a_friendlyFire)
	{
		if (!(a_radius > 0.0f && a_radius <= MAX_EXPLOSION_RADIUS)) {
			return false;
		}
		const bool near = !(a_distance > a_radius + EXPLOSION_SLACK);
		return a_friendlyFire || !near;
	}

	// ---- Workshop (WorkshopSync) ----------------------------------------------------------------

	// A friend's build is shown at most this much smaller or bigger than the game's own size: what the
	// workshop makes is never far off 1, and a wall a hundred times its size covers a town.
	constexpr float MIN_SCALE = 0.1f;
	constexpr float MAX_SCALE = 10.0f;

	constexpr float ClampScale(float a_scale)
	{
		if (!(a_scale == a_scale)) {
			return 1.0f;
		}
		return a_scale < MIN_SCALE ? MIN_SCALE : (a_scale > MAX_SCALE ? MAX_SCALE : a_scale);
	}

	// A workbench's build area, as the game knows it here: volumes linked to the bench, loaded or not, or
	// none at all.
	enum class BuildArea
	{
		kNoVolumes,
		kNotLoaded,
		kLoaded,
	};

	enum class Verdict
	{
		kApply,
		kWait,    // tried again when a cell loads
		kRefuse,  // for good
	};

	// How far a build area reaches from a workbench that has no volumes (the game's own fallback).
	constexpr float BUILD_AREA_RADIUS = 5000.0f;

	// Whether a friend's scrap of a piece of our world falls in its workbench's build area. a_inArea: the
	// game's own test with the volumes (only read when they are loaded). While its volumes exist but aren't
	// loaded the scrap waits rather than being refused or let through by a guess.
	constexpr Verdict ScrapReach(BuildArea a_area, bool a_inArea, float a_distanceToBench)
	{
		switch (a_area) {
		case BuildArea::kLoaded:
			return a_inArea ? Verdict::kApply : Verdict::kRefuse;
		case BuildArea::kNotLoaded:
			return Verdict::kWait;
		default:
			return Within(a_distanceToBench, BUILD_AREA_RADIUS) ? Verdict::kApply : Verdict::kRefuse;
		}
	}

	// ---- A friend's look (RemotePlayers) --------------------------------------------------------

	// The default stand-in bases: settlers with a fixed look and no factions, one per sex.
	constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
	constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;

	// What a friend's stand-in may be placed from: the two settlers, or what our own ini picked for
	// stand-ins. Any other NPC brings its own scripts and factions into our world on a friend's word
	// (a unique one its quests, but plain ones too: a settler that runs workshop scripts, one that
	// teleports), so a friend asking for one is shown as our default.
	constexpr bool AllowedAppearance(std::uint32_t a_base, std::uint32_t a_localDefault)
	{
		return a_base == FEMALE_SETTLER || a_base == MALE_SETTLER || a_base == a_localDefault;
	}
}
