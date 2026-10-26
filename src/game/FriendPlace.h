#pragma once

#include <optional>

// Where a friend is, as our game can tell. A friend in a place our game does not have (a cell or worldspace of a DLC or
// mod we lack) sends states we cannot read: FormTranslate leaves them out, and says whose they were. Those states are
// not missing the way a lagging or gone friend's are (that is handled elsewhere): we know they are somewhere we cannot
// reach. Until a state comes that we can place, their last readable state is out of date, and standing in for where they
// are would put them "right here" in the list and the compass, and send a teleport to where they used to be.
// Kept free of the game so it can be tested.
namespace FriendPlace
{
	struct Track
	{
		bool beyond = false;  // their latest state named a place our game does not have

		// A state of theirs came that named a place we lack.
		void Unplaced() { beyond = true; }
		// A state of theirs came that we could read: wherever they are, it is a place we have.
		void Placed() { beyond = false; }

		// Their last readable state, or none while they are somewhere we cannot reach (it is where they were, not where they are).
		template <class State>
		[[nodiscard]] std::optional<State> Where(const std::optional<State>& a_last) const
		{
			return beyond ? std::nullopt : a_last;
		}
	};
}
