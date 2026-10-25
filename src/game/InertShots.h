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

	// Every inert shot is kept a handle to while it flies (its base exists at runtime only, and the game saves projectile
	// references: InertShot.h). Once a frame, on the main thread: those the engine has deleted are let go of, and one that
	// is stale, or settled in a cell no longer attached, is ended (disabled, deleted, its record dropped).
	void Frame();

	// Before every save and every load, after DespawnAll: every live inert shot is ended now, so none is written into the
	// save nor left for the load (SaveGuard's own hand-over is for what Track knows of; these go the engine's own way, as
	// a projectile that hit something does). How many were ended.
	std::uint32_t BeforeSaveOrLoad();

	// A relayed explosion was placed from a record that spawns projectiles (WeaponFire::ApplyExplosion): for a moment an
	// unowned launch of that spawn record is swapped for its inert copy too (InertShot::SwapsSpawned). Null is fine.
	void ExpectSpawned(RE::BGSProjectile* a_spawn);

	// Counters for `net status`: launches seen, swapped, refused (no copy to be had), copies made, launches off the
	// main thread, shots live now, ended (stale, unloaded, or at a save or load), spawned children swapped.
	[[nodiscard]] std::string Describe();
}
