#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// A hit on our stand-in in a friend's game comes as kPlayerDamaged (Session::ApplyDamageToPlayer): an NPC their game
// runs struck it there, so our player takes the damage here. Our copy of that NPC never hurts us itself: a copy another
// game runs plays its moves and its attacks are disabled (Puppets), so every hit by an enemy a friend's game runs that
// doesn't get through this is a hit our player never takes, and that enemy can't hurt us at all.
//
// Up to 0.9.2 the hit was taken only if our copy of the NPC was there to see: loaded, alive, in our cell or worldspace
// and within RANGE. It was ignored when our copy was a corpse (the session had it dead and the host's game brought it
// back with a cell reset, ours not yet), stood outside while the friend and the NPC went through a door, had not loaded
// here, or was an NPC of a plugin our game lacks (the hit named nothing: FormTranslate makes such an attacker 0). The
// friend saw the enemy fight us; we took nothing.
//
// So a hit whose attacker isn't to be seen here is taken all the same when the friend who reported it is near us: the NPC
// stands within weapon range of our stand-in in their game, and their game runs only NPCs loaded around their player, so
// their player is within REPORTER_RANGE of us. A hit from a friend nowhere near is still ignored: their game has an NPC
// of theirs fighting a stand-in that stands nowhere near our player. Pure, so it can be tested without the game.
namespace PlayerHits
{
	constexpr float RANGE = 10000.0f;           // about 140 m: a sniper rifle's reach
	constexpr float REPORTER_RANGE = 20000.0f;  // the NPC within RANGE of our stand-in there, their player within RANGE of the NPC

	// Our copy of the NPC that hit our stand-in.
	enum class Attacker : std::uint8_t
	{
		kUnnamed,    // the hit names no NPC: one of a plugin our game (or the session's table) lacks
		kNotHere,    // no such actor here, or not loaded
		kDead,       // dead here (the session had it dead; the friend's game has it alive again)
		kElsewhere,  // in another cell or worldspace
		kFar,        // farther than RANGE
		kHere,       // loaded, alive, in our cell or worldspace, within RANGE
	};

	struct Facts
	{
		bool                 byPlayer = false;  // the friend hit us themselves (friendly fire): always taken
		Attacker             attacker = Attacker::kNotHere;
		std::optional<float> reporterApart;  // how far the reporting friend's stand-in is from our player, if in our cell or worldspace
	};

	enum class Verdict : std::uint8_t
	{
		kTake,
		kTakeUnseen,  // taken, though the attacker isn't to be seen here: the reporter is near
		kIgnore,
	};

	constexpr Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.byPlayer || a_facts.attacker == Attacker::kHere) {
			return Verdict::kTake;
		}
		if (a_facts.reporterApart && *a_facts.reporterApart < REPORTER_RANGE) {
			return Verdict::kTakeUnseen;
		}
		return Verdict::kIgnore;
	}

	constexpr const char* Name(Attacker a_attacker)
	{
		switch (a_attacker) {
		case Attacker::kUnnamed:
			return "an NPC our game can't name";
		case Attacker::kNotHere:
			return "not here";
		case Attacker::kDead:
			return "dead here";
		case Attacker::kElsewhere:
			return "in another cell";
		case Attacker::kFar:
			return "too far";
		case Attacker::kHere:
			return "here";
		}
		return "?";
	}

	// ---- a real hit (since protocol 85, src/game/RealHits.h) -------------------------------------------------------------
	// Up to protocol 84 the hit came as the number the stand-in took (its copy of our armor already took its share) and was
	// taken off our health, scaled by our difficulty and VATS: none of our own side of a hit ran. Our perks (Toughness,
	// Refractor, Adamantium Skeleton, ...), our legendary armor effects, the limb it struck, radiation, poison and other
	// weapon effects, the stagger. Now the sender says what the hit was, and our game makes it a real one on our player from
	// our own copy of the NPC (HitData::InitializeHitData, then Actor::DoHitMe through the task queue, as the engine's own
	// hits go): the engine runs all of it, difficulty and VATS included.

	// HitData's flags the sender reads and carries (RE::HitData::Flag).
	namespace HitFlag
	{
		inline constexpr std::uint32_t kBash = 1u << 15;
		inline constexpr std::uint32_t kTimedBash = 1u << 16;
		inline constexpr std::uint32_t kPowerAttack = 1u << 17;
		inline constexpr std::uint32_t kMeleeAttack = 1u << 18;
		inline constexpr std::uint32_t kExplosion = 1u << 20;
		inline constexpr std::uint32_t kCarried = kBash | kTimedBash | kPowerAttack;  // what the victim's hit is made with
	}

	// What a hit on a stand-in was, from its HitData's flags: an explosion's blow, a melee one, else a shot.
	constexpr std::uint8_t KindOf(std::uint32_t a_flags)
	{
		if (a_flags & HitFlag::kExplosion) {
			return Protocol::HitKind::kExplosion;
		}
		return (a_flags & HitFlag::kMeleeAttack) ? Protocol::HitKind::kMelee : Protocol::HitKind::kRanged;
	}

	// The hit is made real here when it was taken with its attacker to be seen (our copy of it strikes us), an NPC's (a
	// friend's own hit is taken as their game made it: friendly fire's rules are theirs) and the sender said what it was.
	// Else it is taken as it came: a hit taken unseen has no copy here to strike with.
	constexpr bool MakeReal(Verdict a_verdict, bool a_byPlayer, std::uint8_t a_kind)
	{
		return a_verdict == Verdict::kTake && !a_byPlayer && a_kind != Protocol::HitKind::kNone && a_kind <= Protocol::HitKind::kMax;
	}

	// A shot of a gun that fires several projectiles (a shotgun's pellets) hits once for each, and the engine makes each
	// pellet's hit from the whole shot's damage, resisted as one, then shares it among them (HitData::InitializeHitData
	// with a projectile): the share of one.
	constexpr float PelletShare(unsigned a_projectiles)
	{
		return a_projectiles > 1 ? 1.0f / static_cast<float>(a_projectiles) : 1.0f;
	}

	// An explosion's blow is made from its base form's damage times a multiplier (the explosion's own multiplier and how
	// far from its middle the blow struck): the sender's raw damage over the base's. A base with no damage of its own takes
	// the raw damage at a multiplier of 1. MAX_BLAST_MULT bounds a multiplier no explosion makes.
	inline constexpr float MAX_BLAST_MULT = 10.0f;
	struct Blast
	{
		float damage = 0.0f;
		float mult = 0.0f;
	};
	inline Blast BlastFrom(float a_raw, float a_baseDamage)
	{
		if (!std::isfinite(a_raw) || a_raw <= 0.0f) {
			return {};
		}
		if (!std::isfinite(a_baseDamage) || a_baseDamage <= 0.0f) {
			return { a_raw, 1.0f };
		}
		return { a_baseDamage, (std::min)(a_raw / a_baseDamage, MAX_BLAST_MULT) };
	}

	// What the sender reads of a hit on a stand-in for the victim. a_physical and a_resisted: HitData's physicalDamage
	// and resistedPhysicalDamage (the stand-in's armor's share), whose sum is the blow before any armor.
	inline float RawPhysical(float a_physical, float a_resisted)
	{
		const float raw = (std::isfinite(a_physical) ? a_physical : 0.0f) + (std::isfinite(a_resisted) ? a_resisted : 0.0f);
		return raw > 0.0f ? raw : 0.0f;
	}

	// The most a real hit takes in one go: as the old path's bound.
	inline constexpr float MAX_REAL_HIT = 1000.0f;
}
