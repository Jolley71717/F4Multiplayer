#pragma once

#include "game/CopyHealth.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RE
{
	class Actor;
	class TESHitEvent;
}

// Our copies of NPCs another game runs: nothing in our world but our hits (our player's, and those of the actors our
// game runs: CopyHealth::Judge) is to take their health (CopyHealth), and what does hit them, or kill them, goes to the
// log. Main thread, except the Note* calls.
namespace CopyWatch
{
	// What the reference is as the cause of a hit or a death, for CopyHealth::Judge (0, or gone: nothing).
	[[nodiscard]] CopyHealth::Cause CauseOf(std::uint32_t a_refId);

	// What the hit's blow was (an explosion's, a throw's, a spell's alone), for CopyHealth::OwnersToDeal. Any thread.
	[[nodiscard]] CopyHealth::Blow BlowOf(const RE::TESHitEvent& a_event);

	// The form id of the reference the hit names as its source (an explosion's names it: WeaponFire::BlastAt), 0 for none.
	// Any thread.
	[[nodiscard]] std::uint32_t SourceOf(const RE::TESHitEvent& a_event);

	// Whether a_owner's game deals the blow on our copy a_target (our form id) itself (CopyHealth::OwnersToDeal): an
	// explosion's, by what became of the explosion that dealt it, the one the hit names (a_sourceRef: SourceOf) if we saw it
	// (WeaponFire::BlastAt, RemoteEdits::DealtThere). Main thread.
	[[nodiscard]] bool OwnersToDeal(CopyHealth::Blow a_blow, std::uint32_t a_target, std::uint32_t a_owner, std::uint32_t a_sourceRef);

	// A hit in our world (WorldSync's hit sink, on whatever thread the engine sends it).
	void NoteHit(const RE::TESHitEvent& a_event);

	// A death in our world, and what killed it (0: nothing).
	void NoteDeath(std::uint32_t a_refId, std::uint32_t a_killer);

	// Another player's report set the copy's health (WorldSync::TrySetHealth, a_refId our game's form id, a spawn copy's
	// too): where it stands from now on.
	void HealthSet(std::uint32_t a_refId, float a_before, float a_health);

	// Gives copies back what something other than our hits took, and logs the hits and deaths. Every frame.
	void Frame();

	// What our hits took off our copies since the last call (CopyHealth::Book::TakeOurs), each as a session id and a
	// fraction of the copy's maximum: WorldSync sends it to the game that runs the NPC (kReportDamage).
	[[nodiscard]] std::vector<std::pair<std::uint32_t, float>> TakeDamage();

	void Reset();

	// Whether the actor is a ghost, as the engine says and as its extra data holds it: "on (extra on)", "off (extra
	// none)" and so on ("?" for no actor).
	[[nodiscard]] std::string GhostState(RE::Actor* a_actor);

	[[nodiscard]] std::string Describe();
}
