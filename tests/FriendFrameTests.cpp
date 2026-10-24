// Which power armor frames our player is kept out of (FriendFrame): only our copies of the frame a friend used last.

#include "Test.h"
#include "game/FriendFrame.h"

namespace
{
	constexpr std::uint32_t FRIEND = 2;
	constexpr std::uint32_t OTHER_FRIEND = 3;

	// A copy the session placed: made at runtime, nobody's here.
	constexpr FriendFrame::Facts COPY{ .created = true };
}

TEST("friend frame: our copy of a friend's parked frame, or of the one their stand-in got out of, is theirs")
{
	using FriendFrame::KeptOutFor;
	auto parked = COPY;
	parked.parkedFor = FRIEND;
	CHECK(KeptOutFor(parked) == FRIEND);
	auto left = COPY;
	left.leftFor = OTHER_FRIEND;
	CHECK(KeptOutFor(left) == OTHER_FRIEND);
}

TEST("friend frame: our copy of a frame a friend's game made is theirs only when it is the one they used last")
{
	using FriendFrame::KeptOutFor;
	auto shared = COPY;
	shared.sharedBy = FRIEND;
	shared.sharedUsed = true;  // it stands where they say they parked (a 0.9.1 leaked copy they got into counts too)
	CHECK(KeptOutFor(shared) == FRIEND);
	shared.sharedUsed = false;  // one the host's game shares only because it stands loaded there: nobody used it
	CHECK(KeptOutFor(shared) == 0);
}

TEST("friend frame: our player's own frame, an unowned frame, a plugin's frame and a quest's frame are never kept from them")
{
	using FriendFrame::KeptOutFor;
	CHECK(KeptOutFor(COPY) == 0);  // a runtime frame nobody in the session owns
	CHECK(KeptOutFor({}) == 0);    // a plugin's frame (the museum's suit in Concord), whatever FrameShare did with it
	auto with = [](auto a_change) {
		auto facts = COPY;
		facts.parkedFor = FRIEND;
		facts.leftFor = FRIEND;
		facts.sharedBy = FRIEND;
		facts.sharedUsed = true;
		a_change(facts);
		return KeptOutFor(facts);
	};
	CHECK(with([](auto&) {}) == FRIEND);
	CHECK(with([](auto& f) { f.ours = true; }) == 0);      // our player is in it or got out of it last
	CHECK(with([](auto& f) { f.quest = true; }) == 0);     // a quest alias of ours holds it
	CHECK(with([](auto& f) { f.created = false; }) == 0);  // every game has its own reference of it
}
