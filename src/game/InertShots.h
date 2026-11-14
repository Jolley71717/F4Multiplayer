#pragma once

#include "game/InertShot.h"

#include <cstdint>
#include <string>

// A stand-in's or a copy's shot that would go off flies with no blast of its own: the engine's Projectile::Launch is
// hooked at its entry, and such a launch is made from an inert copy of the projectile record (InertShot.h has the
// decisions and the bytes; docs/DESIGN.md, "Who sets off an explosion"). The shooter's game reports the blast, and
// WeaponFire::ApplyExplosion sets it off here, once. The same hook sends every launch of a stand-in's where its friend
// aimed (InertShot::Aims): a flamer's stream or a harpoon of the engine's own went at whoever the stand-in looked at.
namespace InertShots
{
	// Puts the hook on Projectile::Launch, at plugin load. Only where the function begins as 1.11.240 has it and the
	// trampoline has room; otherwise nothing is hooked, and such shots stay held (AttackKind::ShotReplay). Whether it is in.
	bool Install();
	[[nodiscard]] bool Installed();

	// Makes the inert copy of a projectile record before a replayed shot needs it, on the main thread (the launch may
	// come on another: a copy is made there too, but this is where forms are best made). Null is fine. A record that goes off
	// is copied for anyone; a plain bullet or beam only for a stand-in (a_standIn: its shot flies, InertShot::Swaps).
	void Prepare(RE::BGSProjectile* a_base, bool a_standIn = false);

	// Dev `trails tracer always|engine`: a stand-in's plain bullet or beam is launched with the launch data's tracer flag set, which
	// makes every one a tracer (the engine otherwise makes every tracerFrequency-th one of an actor's a tracer, InertShot::PROJECTILE_TRACER).
	// Off (the engine's) by default.
	void               SetForceTracer(bool a_on);
	[[nodiscard]] bool ForceTracer();

	// A stand-in's plain bullet or beam launched by us, not by its firing animation's "WeaponFire" (WeaponFire::PlayShot asks,
	// by InertShot::LaunchDirect): from its gun's muzzle (chest height when its 3D has none), along its friend's aim, one
	// launch per pellet (a shotgun's spread round the aim), each through our own hook, so it is swapped for the inert copy,
	// its damage zeroed and it is kept track of like any other trail. Nothing is launched when the actor is no stand-in,
	// holds no gun firing a plain trail, has no cell or no known aim, or the hook is not in. How many were launched.
	// Main thread.
	std::uint32_t Launch(RE::Actor* a_standIn);

	// Dev `trails mode graph|refused|always`: when a replayed shot's trail is launched directly (InertShot::Mode). The graph's
	// alone by default, until the direct launch has been seen right on the rig.
	void                          SetDirectMode(InertShot::Mode a_mode);
	[[nodiscard]] InertShot::Mode DirectMode();

	// Dev `launchlog <n>`: the next n launches of anyone's (our player's, an NPC's, a stand-in's) are written to the log in full, every
	// field of the launch data as the engine filled it, what the hook did, and the projectile made, followed for its first frames.
	// Read-only. How many are still to be logged.
	void              LogLaunches(int a_count);
	[[nodiscard]] int LaunchesToLog();

	// Whether an inert copy of this record exists (dev `proj`).
	[[nodiscard]] bool HasCopy(RE::BGSProjectile* a_base);

	// Every inert shot is kept a handle to while it flies (its base exists at runtime only, and the game saves projectile
	// references: InertShot.h). Once a frame, on the main thread: those the engine has deleted are let go of, and one that
	// is stale, or settled in a cell no longer attached, is ended (disabled, deleted, its record dropped).
	void Frame();

	// Before every save, after DespawnAll: every live inert shot is ended now (disabled, deleted, its record dropped), so none
	// is written into the save. How many were ended.
	std::uint32_t BeforeSave();

	// Before every load, after DespawnAll: every live inert shot is handed to the load (SaveGuard::HandOver), given its
	// record back so the load frees it, or let go of now when it takes none. Ended instead, it would stay in memory through
	// the load, deleted and with a base of this run alone: the load frees only what has a record. How many were handed over.
	std::uint32_t BeforeLoad();

	// A relayed explosion was placed from a record that spawns projectiles (WeaponFire::ApplyExplosion): for a moment an
	// unowned launch of that spawn record is swapped for its inert copy too (InertShot::SwapsSpawned). Null is fine.
	void ExpectSpawned(RE::BGSProjectile* a_spawn);

	// Counters for `net status`: launches seen, swapped, refused (no copy to be had), copies made, launches off the
	// main thread, stand-ins' plain bullets and beams swapped (trails=), shots live now, ended (stale, unloaded, or at a save or load), spawned children swapped, stand-ins'
	// launches of their own record given their friend's aim, and the trails we launched directly (trailsDirect=, Launch).
	[[nodiscard]] std::string Describe();
}
