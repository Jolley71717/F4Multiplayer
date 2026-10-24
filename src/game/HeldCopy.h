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
}
