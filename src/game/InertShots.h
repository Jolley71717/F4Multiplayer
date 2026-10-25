#pragma once

#include <cstdint>
#include <string>

// A stand-in's or a copy's shot that would go off flies with no blast of its own: the engine's Projectile::Launch is
// hooked at its entry, and such a launch is made from an inert copy of the projectile record (InertShot.h has the
// decisions and the bytes; docs/DESIGN.md, "Who sets off an explosion"). The shooter's game reports the blast, and
// WeaponFire::ApplyExplosion sets it off here, once.
namespace InertShots
{
	// Puts the hook on Projectile::Launch, at plugin load. Only where the function begins as 1.11.240 has it and the
	// trampoline has room; otherwise nothing is hooked, and such shots stay held (AttackKind::ShotReplay). Whether it is in.
	bool Install();
	[[nodiscard]] bool Installed();

	// Makes the inert copy of a projectile record before a replayed shot needs it, on the main thread (the launch may
	// come on another: a copy is made there too, but this is where forms are best made). Null is fine.
	void Prepare(RE::BGSProjectile* a_base);

	// Whether an inert copy of this record exists (dev `proj`).
	[[nodiscard]] bool HasCopy(RE::BGSProjectile* a_base);

	// Counters for `net status`: launches seen, swapped, refused (no copy to be had), copies made, launches off the
	// main thread.
	[[nodiscard]] std::string Describe();
}
