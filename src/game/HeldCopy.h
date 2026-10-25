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
	struct Spot
	{
		std::uint32_t cell = 0;  // its parent cell (0: none)
		float         x = 0.0f, y = 0.0f, z = 0.0f;
	};

	// A copy back from its record stands where it was when its cell unloaded it: where it was last seen, to within this.
	constexpr float SAME_SPOT = 8.0f;

	constexpr bool SameSpot(const Spot& a_was, const Spot& a_now)
	{
		const float dx = a_was.x - a_now.x, dy = a_was.y - a_now.y, dz = a_was.z - a_now.z;
		return a_was.cell == a_now.cell && dx * dx + dy * dy + dz * dz <= SAME_SPOT * SAME_SPOT;
	}

	struct Ids
	{
		std::uint32_t id = 0;    // the copy's form ID (0: none held)
		std::uint32_t base = 0;  // the base it was placed from
		std::optional<Spot> at;  // where it was last seen while its handle resolved (kept up by SaveGuard::Refresh)
	};

	// What is under the id now, once the handle no longer resolves.
	struct Facts
	{
		std::optional<std::uint32_t> baseNow;  // the base of the reference under the id (0 for a form that is no reference); none when no form is there
		bool                         deleted = false;  // that reference is marked for deletion
		bool                         ours = false;     // the session's book still has it as placed this run, under that id and base (SaveGuard::Placed)
		std::optional<Spot>          atNow;            // where that reference stands, when it has a cell
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
		// The id and base alone are no proof: a record dropped while the copy was still held frees its id, and a new frame of
		// the same base can be made under it. Only one standing where ours was last seen is ours back.
		if (a_facts.baseNow && *a_facts.baseNow == a_held.base && a_facts.ours && !a_facts.deleted && a_held.at && a_facts.atNow &&
			SameSpot(*a_held.at, *a_facts.atNow)) {
			return Lost::kBack;
		}
		return Lost::kForget;
	}
}
