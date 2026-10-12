#include "Test.h"
#include "game/ArmorClimb.h"

using namespace std::chrono_literals;
using ArmorClimb::Next;
using ArmorClimb::Step;

TEST("armor climb: in once the engine has the stand-in in its armor and it stands free")
{
	CHECK(Next(true, true, true, 0ms) == Step::kDone);
	CHECK(Next(true, true, true, 4s) == Step::kDone);
}

TEST("armor climb: still climbing in until both, however long the first part takes")
{
	CHECK(Next(true, false, true, 0ms) == Step::kWait);   // not in the armor yet
	CHECK(Next(true, true, false, 3s) == Step::kWait);    // in, but still in the frame's animation
	CHECK(Next(true, false, true, 9s) == Step::kWait);    // a slow game: longer than the old fixed six seconds
	CHECK(Next(true, false, false, 14s) == Step::kWait);
}

TEST("armor climb: out once the engine has taken the armor off and it stands free")
{
	CHECK(Next(false, false, true, 2s) == Step::kDone);
	CHECK(Next(false, true, true, 2s) == Step::kWait);
	CHECK(Next(false, false, false, 2s) == Step::kWait);
}

TEST("armor climb: a climb that never finishes is cut short at the limit")
{
	CHECK(Next(true, false, true, ArmorClimb::LIMIT) == Step::kCutShort);
	CHECK(Next(true, true, false, ArmorClimb::LIMIT + 1s) == Step::kCutShort);
	CHECK(Next(false, true, true, ArmorClimb::LIMIT) == Step::kCutShort);
	CHECK(Next(true, true, true, ArmorClimb::LIMIT + 1s) == Step::kDone);  // finished counts, however late
}
