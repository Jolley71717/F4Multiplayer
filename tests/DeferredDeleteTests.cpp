#include "Test.h"
#include "game/DeferredDelete.h"

using namespace std::chrono_literals;

TEST("deferred delete: a stand-in with its 3D loaded can go at once")
{
	CHECK(DeferredDelete::Safe(true, 0ms));
}

TEST("deferred delete: one still loading waits, but not forever")
{
	CHECK(!DeferredDelete::Safe(false, 0ms));
	CHECK(!DeferredDelete::Safe(false, 2900ms));
	CHECK(DeferredDelete::Safe(false, DeferredDelete::MAX_WAIT));
}

TEST("deferred delete: a stand-in in the player's conversation is kept while it lasts")
{
	CHECK(DeferredDelete::Held(true, 0ms));
	CHECK(DeferredDelete::Held(true, 59s));
	CHECK(!DeferredDelete::Held(false, 0ms));
}

TEST("deferred delete: but not for ever")
{
	CHECK(!DeferredDelete::Held(true, DeferredDelete::MAX_HOLD));
	CHECK(!DeferredDelete::Held(true, 10min));
}

TEST("deferred delete: a reference is marked for deletion only a second after it was disabled")
{
	CHECK(!DeferredDelete::MayDelete(std::nullopt));
	CHECK(!DeferredDelete::MayDelete(999ms));
	CHECK(DeferredDelete::MayDelete(DeferredDelete::DELETE_AFTER_DISABLE));
	CHECK(DeferredDelete::MayDelete(5s));
}

TEST("deferred delete: not even before a save: one disabled in the save's flush waits")
{
	// The flush before a save disables what still stands and asks at once: the answer must be no, or it is
	// disabled and deleted in one frame (a physics job crashed on the freed stand-in after a save).
	CHECK(!DeferredDelete::MayDelete(0ms));
}

TEST("a reference marked for deletion is said once: first held, then quiet, and gone when its handle fails")
{
	using DeferredDelete::Outcome;
	CHECK(DeferredDelete::OutcomeOf(true, true, false) == Outcome::kHeld);
	CHECK(DeferredDelete::OutcomeOf(true, true, true) == Outcome::kQuiet);
	CHECK(DeferredDelete::OutcomeOf(false, false, true) == Outcome::kGone);
	CHECK(DeferredDelete::OutcomeOf(false, true, false) == Outcome::kGone);
}

TEST("a reference that resolves and is not marked deleted is told, once")
{
	using DeferredDelete::Outcome;
	CHECK(DeferredDelete::OutcomeOf(true, false, false) == Outcome::kNotMarked);
	CHECK(DeferredDelete::OutcomeOf(true, false, true) == Outcome::kQuiet);
}
