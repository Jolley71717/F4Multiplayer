#pragma once

#include <chrono>
#include <cstddef>

// A friend's line whose speaker is a spawn of theirs with no copy standing here yet (Conversations::Apply). A random
// encounter's first line comes as its actors are shared, and the copies follow a moment later: on the rig (0.14.0) the
// accusation's opening line came 0.3 s before the copies were placed, and nobody was here to say it. Held, then, until
// the copy stands with its 3D (one just placed has a cell before its 3D, and a line said at it then finds no speaker:
// the held line was let go the frame the copy was placed and ended silently, the rig again), or until the hold is up:
// a copy that takes longer is far from us, or not to be placed. One whose spawn is over meanwhile (dead, let go of) is
// let go, not said at a corpse. Kept free of the game so it can be tested (tests/LineHoldTests.cpp).
namespace LineHold
{
	inline constexpr auto        HOLD_FOR = std::chrono::seconds(2);
	inline constexpr std::size_t MAX_HELD = 16;

	// Whether a line as it comes is held rather than said now: its speaker's spawn is expected (known, alive, theirs), no
	// copy of it stands here with its 3D yet, and there is room. One not expected goes on to be said, and ends as any
	// line without a speaker here does.
	constexpr bool Holds(bool a_expects, bool a_placed, bool a_loaded, std::size_t a_heldNow)
	{
		return a_expects && !(a_placed && a_loaded) && a_heldNow < MAX_HELD;
	}

	enum class Step
	{
		kHold,  // keep it
		kSay,   // the copy stands with its 3D: said now
		kDrop,  // its spawn is over, or the hold is up: let go unsaid
	};

	constexpr Step Decide(bool a_expects, bool a_placed, bool a_loaded, std::chrono::steady_clock::duration a_since)
	{
		if (!a_expects) {
			return Step::kDrop;
		}
		if (a_placed && a_loaded) {
			return Step::kSay;
		}
		return a_since >= HOLD_FOR ? Step::kDrop : Step::kHold;
	}
}
