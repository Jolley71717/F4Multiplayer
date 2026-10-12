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
