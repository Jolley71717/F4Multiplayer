#pragma once

#include <cstdint>

// How a reported attack (a shot, a swing or a punch) is replayed on a stand-in, by what it holds.
namespace AttackKind
{
	constexpr std::int32_t GUN_TYPE = 9;  // RE::WEAPON_TYPE::kGun

	// Guns replay through the animation graph ("attackStart": the firing animation, no bullet).
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
