#pragma once

#include <cstdint>
#include <optional>

// A copy the session placed and holds by its handle: a friend's frame copy (the one their stand-in gets into, the one
// left where they got out, the one at their parked frame, our copy of a frame their game made). The form ID and base it
// was placed under are kept beside the handle, for when the handle stops resolving.
// The engine unloads a reference it never made persistent with its cell (the player walked or travelled off, or went
// through a door): the form is freed, so the handle stops resolving, and its record of changes stays under the cell and
// brings it back, under the same id, when the cell loads again. Taken for gone and simply let go of, the copy came back
// on the player's return beside the one placed for the friend's frame then, and every save from then on kept it: a
// frame nobody owned, where a friend had once parked, got out or walked in their armor. So the id is looked up again
// (Look): the same copy back is held again, never a second one placed beside it; otherwise it is forgotten, and SaveGuard
// drops the record the engine keeps of it (SaveGuard::Forget, SaveStrays::Book::KeepForgotten).
// Kept free of the game so the rules can be tested.
namespace HeldCopy
{
	struct Ids
	{
		std::uint32_t id = 0;    // the copy's form ID (0: none held)
		std::uint32_t base = 0;  // the base it was placed from
	};

	// What is under the id now, once the handle no longer resolves.
	struct Facts
	{
		std::optional<std::uint32_t> baseNow;  // the base of the reference under the id (0 for a form that is no reference); none when no form is there
		bool                         deleted = false;  // that reference is marked for deletion
		bool                         ours = false;     // the session's book still has it as placed this run, under that id and base (SaveGuard::Placed)
	};

	enum class Lost
	{
		kNone,    // nothing held, or the handle still resolves
		kBack,    // the same copy, back with its cell: held again (reused)
		kForget,  // gone, or the id names something else now: let go of, and forgotten (its record, if the engine kept one, is dropped)
	};

	constexpr Lost Look(const Ids& a_held, bool a_resolves, const Facts& a_facts)
	{
		if (a_held.id == 0 || a_resolves) {
			return Lost::kNone;
		}
		if (a_facts.baseNow && *a_facts.baseNow == a_held.base && a_facts.ours && !a_facts.deleted) {
			return Lost::kBack;
		}
		return Lost::kForget;
	}

	// ---- A copy an earlier run left where one is about to be placed ---------------------------------------------

	// A save made before this was fixed (or one the engine wrote past us) may hold a copy: after its load it is one of the
	// game's references like any other, and our book of what this run placed knows nothing of it. On rejoin, the friend's
	// frame stands where it stood, and its parked copy was placed again right on the old one: two suits in one spot, one
	// more each session. A frame standing at the very spot a copy of the same frame is about to be placed, made at runtime,
	// enabled, emptied (its inventory made, and nothing in it: every copy is emptied as it is placed, and a frame the world
	// placed fills its inventory from its base only once something reads it), held by no quest, nobody's to use here (our
	// player is not in it and did not leave it last, nobody else here is in it) and not one this run placed, is such a copy:
	// it is taken over as the copy (ours from then on, and out of saves) instead of a second one placed beside it.
	struct Standing
	{
		bool  created = false;   // made at runtime (0xFF......)
		bool  sameBase = false;  // of the frame the copy is placed of
		bool  enabled = false;   // neither disabled nor deleted
		bool  emptied = false;   // its inventory is made, and empty
		bool  quest = false;     // it fills a quest alias of ours
		bool  used = false;      // our player is in it or left it last, or somebody here is in it
		bool  placed = false;    // this run placed it (SaveGuard::Placed): somebody's copy now, or on its way out
		float distance = 0.0f;   // from the spot
	};

	// How close to the spot: a copy is placed at the friend's spot as their game sends it, so one left by an earlier session
	// for a frame that has not moved since stands on it to a fraction of a unit.
	constexpr float LEFTOVER_RADIUS = 2.0f;

	constexpr bool Leftover(const Standing& a_standing)
	{
		return a_standing.created && a_standing.sameBase && a_standing.enabled && a_standing.emptied && !a_standing.quest && !a_standing.used && !a_standing.placed &&
		       a_standing.distance <= LEFTOVER_RADIUS;
	}
}
