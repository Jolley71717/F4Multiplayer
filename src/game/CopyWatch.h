#pragma once

#include "game/CopyHealth.h"

#include "Protocol.h"

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

	// Another player's report would set the actor's health from a_before to a_health (WorldSync::TrySetHealth, a_refId our
	// game's form id, a spawn copy's too): the health to set. On a copy, a raise that may be from before our latest hits is
	// held (CopyHealth::Book::Heard); otherwise a_health.
	[[nodiscard]] float HealthToSet(std::uint32_t a_refId, float a_before, float a_health);

	// ... and it reads a_health once set (what the engine let it come to): where it stands from now on.
	void HealthSet(std::uint32_t a_refId, float a_health);

	// Gives copies back what something other than our hits took, takes back what they gained with no report from their
	// game (a legendary's mutation of a copy legendary only here), and logs the hits and deaths (so many a
	// RollingLog::WINDOW) and the damage counters every WINDOW. Every frame.
	void Frame();

	// Books the hits the engine has told us of (queued by NoteHit) in the book and the hit log. Every frame, before any
	// packet is dispatched: a runner's health report heard first would heal back a shot whose event was not booked yet.
	void TakeHits();

	// A damage of ours to send: the copy's session id, what our hits took as a fraction of its maximum, and its number
	// (CopyHealth::Book::TakeOurs), which the game that runs the NPC answers.
	struct Damage
	{
		std::uint32_t id = 0;
		float         fraction = 0.0f;
		std::uint32_t seq = 0;
	};

	// What our hits took off our copies since the last call: WorldSync sends each to the game that runs the NPC
	// (kReportDamage), and it stays in flight on the copy until Answered.
	[[nodiscard]] std::vector<Damage> TakeDamage();

	// The answer to a damage of ours on the copy a_wire (its session id), numbered a_seq, from a_from (0: the server):
	// applied, ours to run now, or refused, in which case the copy gets the health back at once (CopyHealth::Answer).
	void Answered(std::uint32_t a_wire, std::uint32_t a_seq, std::uint32_t a_from, Protocol::DamageAnswer::Why a_why);

	void Reset();

	// Whether the actor is a ghost, as the engine says and as its extra data holds it: "on (extra on)", "off (extra
	// none)" and so on ("?" for no actor).
	[[nodiscard]] std::string GhostState(RE::Actor* a_actor);

	[[nodiscard]] std::string Describe();
}
