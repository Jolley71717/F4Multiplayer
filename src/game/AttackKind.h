#pragma once

#include <chrono>
#include <cstdint>

// How a reported attack (a shot, a swing or a punch) is replayed on a stand-in, by what it holds.
namespace AttackKind
{
	constexpr std::int32_t GUN_TYPE = 9;  // RE::WEAPON_TYPE::kGun

	// Guns replay through the animation graph ("attackStart": the firing animation; its bullet is kept from the engine,
	// HoldsOwnFire).
	// Melee weapons, and fists when nothing is held, refuse that event: they perform the engine's
	// own attack action instead.
	constexpr bool ReplaysAsGun(std::int32_t a_weaponType, bool a_holdsWeapon)
	{
		return a_holdsWeapon && a_weaponType == GUN_TYPE;
	}

	// Whether an actor with these weapons equipped (their types, in the order the engine lists them) replays as a gun:
	// any one of them a gun. A grenade or a mine is equipped beside the gun, and at times listed before it: taken as
	// what it holds, a friend's shots were replayed as swings with their gun out, and went unheard.
	template <class Types>
	constexpr bool HoldsGun(const Types& a_weaponTypes)
	{
		for (const std::int32_t type : a_weaponTypes) {
			if (ReplaysAsGun(type, true)) {
				return true;
			}
		}
		return false;
	}

	// What the attacker's own animation graph announced: "WeaponFire" (a shot leaving the gun) or "weaponSwing" (the hit
	// frame of a melee swing, a punch, or a gun's bash).
	enum class Event
	{
		kFire,
		kSwing,
	};

	// Whether an attack the attacker's graph announced goes to the others as a shot (ShotAction::kShot). A gun's bash
	// ("weaponSwing" with a gun in hand) does not: the receiver replays whatever a friend holding a gun reports as a shot
	// (ReplayShot: the firing animation, the muzzle flash and the gunshot), so every bash was heard as a gunshot. It has
	// no replay of its own on the wire; what it hits takes its damage in the attacker's game, and health sync carries it.
	constexpr bool ReportsAsShot(Event a_event, bool a_holdsGun)
	{
		return a_event == Event::kFire || !a_holdsGun;
	}

	// Whether a stand-in's own "WeaponFire" is kept from the engine: a gun's (a throw's, payload "2", is held by
	// ThrowReplays) within HOLD_FIRE_FOR of a shot replayed on it. The replay's "attackStart" plays the firing animation,
	// and the animation's "WeaponFire" had the engine fire a real bullet from the stand-in (seen on the rig: a 10mm round
	// of the stand-in's hit our player on every replayed shot, whichever way it faced, so a friend's every shot showed
	// the other player a hit's flash, blood and damage marker). The friend's shot is fired in their game, not here. A
	// stand-in never attacks on its own (Puppets), so a window after the last replay is enough: an automatic gun's next
	// replay renews it.
	inline constexpr std::chrono::milliseconds HOLD_FIRE_FOR{ 1500 };

	constexpr bool HoldsOwnFire(Event a_event, bool a_throw, std::chrono::steady_clock::duration a_sinceReplay)
	{
		return a_event == Event::kFire && !a_throw && a_sinceReplay >= std::chrono::steady_clock::duration::zero() && a_sinceReplay <= HOLD_FIRE_FOR;
	}

	enum class Replay
	{
		kNone,       // nothing: a swing or a punch with the weapon put away
		kSwing,      // the engine's own attack action (melee, fists)
		kFire,       // the firing animation and the gun's sound
		kSoundOnly,  // the gun's sound alone: the stand-in's gun is put away (it is still drawing it again)
	};

	// What a reported attack plays on a stand-in. A gun's shot is always heard: the stand-in's gun can be put away
	// while the shooter's is out (it is drawn again after it is dressed, which takes a moment), and their shots went
	// silent for that moment. The sound is played from the stand-in and needs nothing of the gun's 3D; the firing
	// animation needs the gun out. A swing with nothing drawn would have the engine draw the weapon first, so it waits.
	constexpr Replay ReplayShot(bool a_holdsGun, bool a_drawn)
	{
		if (a_holdsGun) {
			return a_drawn ? Replay::kFire : Replay::kSoundOnly;
		}
		return a_drawn ? Replay::kSwing : Replay::kNone;
	}
}
