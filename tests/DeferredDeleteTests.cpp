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
