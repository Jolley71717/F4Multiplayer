#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Destructible objects, shared: a car, an explosive barrel, a fuel tank or a gas pump a player shoots is as wrecked in
// every game, and goes off once in each. The rules are DestructionRules.h (game-free, tested); the server keeps the
// session's (Server.cpp's handleDestruction, saved with the session).
//
// - Our side: an object our player hit (TESHitEvent) is watched closely, ten times a second for half a minute after its
//   last change, and a scan once a second finds the objects near our player whose stage changed otherwise (an
//   explosion's, an NPC's shot). Each stage it passes that the session didn't know of goes out (kReportDestruction):
//   its health and whether it is destroyed, and whether the change set off an explosion here.
// - A friend's (kDestructions): our copy takes the lower health, and what the stages it passes do in the engine is done
//   to it: its damaged model, its replacement model (a car's hulk), destroyed, gone (a barrel). If the friend's game set
//   off an explosion (and the server let it through), the explosion of each stage our copy passes now goes off here,
//   through WeaponFire's ApplyExplosion: never one our copy had passed already, which went off here then. A catch-up
//   (after joining or loading: playerId 0) is the wreck alone. An object not in our world yet waits until it is.
//
// Only references a plugin placed (Protocol::IsShareableRef) and never actors (their own sync: a turret's or a robot's
// death). Main thread only, but for OnHit.
namespace Destructibles
{
	// Every frame (welcomed: in a session).
	void Frame(bool a_welcomed);

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();  // reliable

	// From the server.
	void Apply(const Protocol::Destructions& a_message);

	// Our player hit a reference that is not an actor (TESHitEvent; any thread).
	void OnHit(std::uint32_t a_ref);

	// Whether an explosion in our world is a destructible object's own (a stage it passed set it off): the object's
	// report carries it to the others, so WeaponFire's watch must not report it as one of ours too.
	[[nodiscard]] bool ClaimsExplosion(RE::TESObjectREFR* a_explosion);

	// What waits for objects not in our world yet (WorldSync's retry, when references load). ApplyWaiting is true when it
	// applied what waited for a_ref (or found it needs nothing).
	[[nodiscard]] bool AnyWaiting();
	[[nodiscard]] std::vector<std::uint32_t> WaitingRefs();
	bool ApplyWaiting(std::uint32_t a_ref);

	// Before a save or a load: a model being loaded again is back on now, so no save keeps it off.
	void BeforeSaveOrLoad();

	// The session ended, or a save loaded: everything forgotten.
	void Reset();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// The dev channel's `destruct` (docs/DEV-CHANNEL.md): status; `near` (the destructible objects around our player);
	// `probe <ref>`.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
