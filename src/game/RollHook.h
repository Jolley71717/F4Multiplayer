#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <string>

namespace RE
{
	class Actor;
	class TESObjectREFR;
}

// One roll per leveled NPC for the session, made as the game builds the NPC (docs/SHARED-ROLLS.md; the pure part is
// SharedRoll.h).
//
// Two hooks in the engine's resolver of a leveled actor (TESActorBaseData's, 1.11.240 +309B10, ID 2192884), which every
// game runs once per reference, the first time it needs the NPC:
// - its call to IsLeveledCreatureResolved (+0xCB): the reference is about to be rolled. We decide what it is to be (the
//   session's roll of it, a friend's spawn's roll as we place our copy, or the session's deterministic pick) and hold it
//   for this thread;
// - TESNPC's TESActorBaseData::CopyFromTemplateForms (its vtable's slot 7), which the resolver calls with the 13 templates
//   it rolled: they are made ours before the NPC is put together from them, and the same array is what the engine then
//   keeps on the reference (ExtraLeveledCreature), saves, and reports. A level that is its player's times a multiplier
//   is made the roll's after.
// Anything unexpected (bytes not as 1.11.240 has them, a template that is no NPC here, a fault reading the game's data)
// leaves the engine's own roll, said once in the log. Off with bSharedRolls = false.
namespace RollHook
{
	// At plugin load, after F4SE::Init: one 14-byte trampoline stub and one vtable slot.
	void Install();

	// kGameDataReady, main thread: the plugins' names, which key a reference in every game whatever the load order.
	void OnDataReady();

	// Welcomed into a session (main thread): its id seeds every roll; a_rule is the host's sNpcLevel.
	void OnWelcome(std::uint64_t a_sessionId, Protocol::NpcLevelRule a_rule);

	// The session's NPC level (kSessionWorld): 0 = none (each game its own player's).
	void SetSessionLevel(std::uint16_t a_level);

	// The session ended: back to the game's own rolls.
	void Clear();

	// A roll of a plugin's leveled NPC: the session's (a_session: from the server, it replaces any we had), or our own
	// game's report (kept only where the session has none yet). A spawn's is not kept (its copy is armed: Placement).
	void Learn(const Protocol::NpcIdentity& a_identity, bool a_session);

	// Main thread, once a frame: the session's rolls written on our references that have one of their own but are not
	// loaded, so the next time their cell loads they are the session's (Rolls::Apply's write; counted as prewritten).
	void Frame();

	// While one lives, a reference placed on this thread from a_identity.originalBase is built as a_identity's roll (our
	// copy of a friend's spawn, Spawns::PlaceCopy). Consumed() after the placement: whether the resolver took it.
	class Placement
	{
	public:
		explicit Placement(const Protocol::NpcIdentity* a_identity);
		~Placement();
		Placement(const Placement&) = delete;
		Placement& operator=(const Placement&) = delete;

		[[nodiscard]] bool Armed() const { return armed; }
		[[nodiscard]] bool Consumed() const;

	private:
		bool armed = false;
	};

	// Whether our game has the session's roll of a_refId as an entry (dev, tests of the game side).
	[[nodiscard]] bool Knows(std::uint32_t a_refId);

	// net status: rollHook= (hooked, off, unhooked) sessionRolls= rollsForced= rollsPlaced= rollsPicked= rollsMissed= ...
	[[nodiscard]] std::string Describe();

	// Dev: what the session's pick of a_ref would be now, as a line (without making it).
	[[nodiscard]] std::string DescribePick(RE::TESObjectREFR* a_ref);

	// Counted by NpcSync::CheckRolls: a runner's report that differs from what our game built (the check the report
	// became: with one roll per session, it should be 0).
	void NoteDiffer();
}
