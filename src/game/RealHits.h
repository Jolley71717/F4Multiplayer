#pragma once

#include <cstdint>
#include <string>

#include "Protocol.h"

namespace RE
{
	class Actor;
	class PlayerCharacter;
}

// A hit an NPC a friend's game runs lands on our stand-in there, made a real hit on our own player here (since protocol
// 85). Up to 84 the friend's game sent only the number its stand-in took, after the stand-in's copy of our armor took
// its share, and we took that off our health: none of our own side of a hit ran (our perks, legendary armor effects,
// the limb, radiation, poison, stagger). Now it says what the hit was (PlayerHit::kind, weapon, explosion, rawDamage,
// attack, flags), and our game makes it as the engine makes any NPC's hit on the player, from our own copy of that NPC
// (read from Fallout4.exe 1.11.240):
//   - HitData's constructor (ID 2236839, +E36380), then, for a swing or a shot, HitData::InitializeHitData(aggressor,
//     target, BGSObjectInstanceT<TESObjectWEAP>&, BGSEquipIndex by value, bool melee) (ID 2236845, +E36AD0): the
//     weapon's damage from the copy (CombatFormulas::CalcWeaponDamage, its perks' entry points, a shot's fall-off with
//     the copy's distance), the attack's damage multiplier (an attack data set on the HitData first is kept: the friend's
//     attack, found by its event in the copy's race), the critical and the typed damage (radiation, energy, poison...),
//     the limb (HitData::SetTargetedLimb), and HitData::ReduceDamage (ID 2236862): our armor, our perks' entry points,
//     the limb's damage, blocking, reflected damage and VATS's damage multipliers, all of them our player's;
//   - for an explosion's blow, the explosion's own part of HitData's explosion set-up (ID 2236849, +E376A0: aggressor,
//     target, the weapon instance, the BGSExplosion, the damage and a multiplier), as the engine's explosion step calls
//     it (ID 2236848) with the explosion reference's base, damage and fall-off;
//   - then QueueDoHitMe(target, HitData*) (ID 2231111, +CD1B90: the task queue's QueueActorHitMe, ID 2229193), which
//     copies the HitData and runs Actor::DoHitMe (ID 2231148) on our player on the main thread: the health damage with
//     our difficulty (fDiffMultHPToPC*), the critical's and the weapon's hit effects cast from the copy, the limb's
//     condition and crippling, the stagger and the knock-down (the attack data's), the image space.
//   - and after it what the engine's melee and shot step does next (+CD7AA9 in ID 2231181): the copy's perks' "apply
//     combat hit spell" (or "apply bash spell") entry point, whose spell is cast on our player (ID 2201036, the task
//     queue's cast); for an explosion, the explosion's enchantment, as the engine's explosion step casts it (+E28043).
// The copy's own outgoing damage is zeroed (Puppets::SetAttackDamage: its swings and shots are replays), and the engine
// multiplies a hit's damage by the aggressor's AttackDamageMult (+E3A258, in ID 2236864): the zero is lifted for the
// call that makes the hit and put back after.
// Not made real, but taken as it came (the old way): friendly fire (the shooter's game made it, its rules are theirs), a
// hit whose NPC is not to be seen here (no copy to strike with: PlayerHits::Judge's kTakeUnseen), a hit that names no
// kind (a sender that couldn't read it), and any hit when one of the functions does not begin as 1.11.240 has it.
// What stays on the friend's side: the reflected damage of our Ricochet-like perks hurts our copy of the NPC here, not
// the NPC in the friend's game; the limb is the engine's own choice for a hit without a projectile (weighted by the
// race's body parts), not the one the stand-in was struck in.
namespace RealHits
{
	// The functions, checked once: true when every one begins as 1.11.240 has it.
	bool Ready();

	// The hit made real on our player from a_attacker (our copy of the NPC that struck our stand-in), when it can be:
	// false when it can't (then it is taken as it came).
	bool Strike(RE::PlayerCharacter* a_player, RE::Actor* a_attacker, const Protocol::PlayerHit& a_hit);

	// For `net status`: " realHits=<made> realHitsFailed=<not made though asked> realLast='<the last one>'".
	std::string Describe();
}
