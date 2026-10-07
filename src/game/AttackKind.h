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
}
