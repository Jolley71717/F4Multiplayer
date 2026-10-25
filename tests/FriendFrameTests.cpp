// Which power armor frames our player (or a companion of theirs) is kept out of (FriendFrame): every copy the session
// placed of a friend's frame, never our own, a plugin's or a quest's.

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

TEST("friend frame: every copy of a frame a friend's game shares is theirs, whoever used it last")
{
	using FriendFrame::KeptOutFor;
	auto shared = COPY;
	shared.sharedBy = FRIEND;  // the one they got out of last, a raider's they never used, or a 0.9.1 leftover their game shares
	CHECK(KeptOutFor(shared) == FRIEND);
}

TEST("friend frame: our player's own frame, an unowned frame, a plugin's frame and a quest's frame are never kept from them")
{
	using FriendFrame::KeptOutFor;
	CHECK(KeptOutFor(COPY) == 0);  // a runtime frame that is no copy: our own world made it
	CHECK(KeptOutFor({}) == 0);    // a plugin's frame (the museum's suit in Concord), whatever FrameShare did with it
	auto with = [](auto a_change) {
		auto facts = COPY;
		facts.parkedFor = FRIEND;
		facts.leftFor = FRIEND;
		facts.sharedBy = FRIEND;
		a_change(facts);
		return KeptOutFor(facts);
	};
	CHECK(with([](auto&) {}) == FRIEND);
	CHECK(with([](auto& f) { f.ours = true; }) == 0);      // our player is in it or got out of it last: a copy taken before stays theirs
	CHECK(with([](auto& f) { f.quest = true; }) == 0);     // a quest alias of ours holds it
	CHECK(with([](auto& f) { f.created = false; }) == 0);  // every game has its own reference of it
}

TEST("friend frame: a companion ordered into a friend's frame copy is sent out again; our player, a stand-in and a friend's NPC are not")
{
	using FriendFrame::Occupant;
	using FriendFrame::SendsOut;
	constexpr Occupant COMPANION{ .runsHere = true };
	CHECK(SendsOut(COMPANION, FRIEND));
	CHECK(!SendsOut(COMPANION, 0));                                          // a frame our player may use: theirs to be in
	CHECK(!SendsOut({ .standIn = true, .runsHere = true }, FRIEND));         // a friend's stand-in in its own frame copy
	CHECK(!SendsOut({ .player = true, .runsHere = true }, FRIEND));          // our player is kept out as they get in
	CHECK(!SendsOut({}, FRIEND));                                            // an NPC a friend's game runs: their game says
}
