// A friend's line held for a copy of its speaker about to be placed (src/game/LineHold.h): held until the copy stands
// with its 3D, let go when its spawn is over or the hold is up, never said at a copy without its 3D.

#include "Test.h"

#include "game/LineHold.h"

using namespace std::chrono_literals;

TEST("line hold: a line is held while its speaker's copy is not standing with its 3D, and there is room")
{
	using LineHold::Holds;
	CHECK(Holds(true, false, false, 0));   // no copy yet
	CHECK(Holds(true, true, false, 0));    // a copy placed, its 3D not in yet (the rig: the line ended silently here)
	CHECK(!Holds(true, true, true, 0));    // standing: said now
	CHECK(!Holds(false, false, false, 0)); // no such spawn, or over: said now, and ends as any line without a speaker
	CHECK(Holds(true, false, false, LineHold::MAX_HELD - 1));
	CHECK(!Holds(true, false, false, LineHold::MAX_HELD));  // no room
}

TEST("line hold: a held line is said once the copy stands, let go when its spawn is over or the hold is up")
{
	using LineHold::Decide;
	using LineHold::Step;
	CHECK(Decide(true, false, false, 0ms) == Step::kHold);
	CHECK(Decide(true, true, false, 300ms) == Step::kHold);  // placed, no 3D yet: kept
	CHECK(Decide(true, true, true, 300ms) == Step::kSay);
	CHECK(Decide(true, true, true, LineHold::HOLD_FOR) == Step::kSay);  // standing as the hold ends: still said
	CHECK(Decide(true, true, false, LineHold::HOLD_FOR) == Step::kDrop);
	CHECK(Decide(true, false, false, LineHold::HOLD_FOR + 1s) == Step::kDrop);
	CHECK(Decide(false, true, true, 100ms) == Step::kDrop);  // over (dead, let go of): not said at a corpse
	CHECK(Decide(false, false, false, 100ms) == Step::kDrop);
}
