// A friend who stands near us and is not shown (game/StandInDraw.h): every way a stand-in can fail to be seen is named; unseen for
// DRAW_WAIT it is asked to build its 3D again, then placed again, then placed again plain, then left alone and said once; none
// standing is said once, or, when placing failed, the next is placed plain; seen again, it starts over.

#include "Test.h"

#include "game/StandInDraw.h"

#include <limits>
#include <string_view>
#include <vector>

using namespace StandInDraw;
using namespace std::chrono_literals;

namespace
{
	const auto t0 = Clock::time_point{} + 1h;

	// A stand-in that is seen: placed, in a drawn cell, with a 3D that has its parts, where they are.
	Facts Seen()
	{
		return { .wanted = true, .placed = true, .cellDrawn = true, .has3D = true, .culled = false, .fade = 1.0f, .scale = 1.0f, .parts = 6, .off = 10.0f };
	}

	Facts No3D()
	{
		auto facts = Seen();
		facts.has3D = false;
		return facts;
	}

	Facts NoneStanding(bool a_spawnFailed = false)
	{
		return { .wanted = true, .placed = false, .spawnFailed = a_spawnFailed };
	}
}

TEST("StandInDraw: each way of not being seen has its name, and a whole stand-in is seen")
{
	CHECK(WhyNot(Seen()) == Why::kSeen);
	CHECK(WhyNot(NoneStanding()) == Why::kNone);
	auto facts = Seen();
	facts.cellDrawn = false;
	CHECK(WhyNot(facts) == Why::kCell);
	CHECK(WhyNot(No3D()) == Why::kNo3D);
	facts = Seen();
	facts.culled = true;
	CHECK(WhyNot(facts) == Why::kCulled);
	facts = Seen();
	facts.fade = 0.0f;
	CHECK(WhyNot(facts) == Why::kFaded);
	facts.away = FADE_NEAR + 1.0f;  // far enough for the engine's own actor fade: not ours to mend
	CHECK(WhyNot(facts) == Why::kSeen);
	facts.away = 100.0f;
	facts.fade = 0.2f;  // fading in or out is seen
	CHECK(WhyNot(facts) == Why::kSeen);
	facts = Seen();
	facts.scale = 0.0f;
	CHECK(WhyNot(facts) == Why::kTiny);
	facts.scale = 0.9f;  // a short friend is seen
	CHECK(WhyNot(facts) == Why::kSeen);
	facts = Seen();
	facts.parts = 0;
	CHECK(WhyNot(facts) == Why::kBare);
	facts = Seen();
	facts.off = ELSEWHERE + 1.0f;
	CHECK(WhyNot(facts) == Why::kElsewhere);
	// a broken number is never "seen"
	facts = Seen();
	facts.fade = std::numeric_limits<float>::quiet_NaN();
	CHECK(WhyNot(facts) == Why::kFaded);
	facts = Seen();
	facts.scale = std::numeric_limits<float>::quiet_NaN();
	CHECK(WhyNot(facts) == Why::kTiny);
	facts = Seen();
	facts.off = std::numeric_limits<float>::quiet_NaN();
	CHECK(WhyNot(facts) == Why::kElsewhere);
	for (const auto why : { Why::kSeen, Why::kNone, Why::kCell, Why::kNo3D, Why::kCulled, Why::kFaded, Why::kTiny, Why::kBare, Why::kElsewhere }) {
		CHECK(std::string_view(Name(why)) != "?");
	}
}

TEST("StandInDraw: unseen for DRAW_WAIT is a rebuild, then a fresh stand-in, then a plain one, then left alone and said once")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, No3D(), t0) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + DRAW_WAIT) == Then::kRebuild);
	// the rebuild gets DRAW_WAIT of its own
	CHECK(Frame(track, No3D(), t0 + DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 2 * DRAW_WAIT) == Then::kReplace);
	CHECK(track.replaces == 1);
	CHECK(!track.plain);
	// the fresh one, watched afresh: its own wait, its own rebuild, then the plain one
	Placed(track);
	auto t = t0 + 2 * DRAW_WAIT + 100ms;
	CHECK(Frame(track, No3D(), t) == Then::kNothing);
	CHECK(Frame(track, No3D(), t + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t + 2 * DRAW_WAIT) == Then::kPlain);
	CHECK(track.replaces == 2);
	CHECK(track.plain);
	Placed(track);
	t = t + 2 * DRAW_WAIT + 100ms;
	CHECK(Frame(track, No3D(), t) == Then::kNothing);
	CHECK(Frame(track, No3D(), t + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t + 2 * DRAW_WAIT) == Then::kGiveUp);
	CHECK(Frame(track, No3D(), t + 3 * DRAW_WAIT) == Then::kNothing);
	CHECK(track.replaces == 2);
	CHECK(track.plain);
}

TEST("StandInDraw: every way of not being seen goes down the same steps")
{
	std::vector<Facts> ways;
	ways.push_back(No3D());
	ways.push_back(Seen());
	ways.back().cellDrawn = false;
	ways.push_back(Seen());
	ways.back().culled = true;
	ways.push_back(Seen());
	ways.back().fade = 0.0f;
	ways.push_back(Seen());
	ways.back().scale = 0.0f;
	ways.push_back(Seen());
	ways.back().parts = 0;
	ways.push_back(Seen());
	ways.back().off = 50000.0f;
	for (const auto& way : ways) {
		Track track;
		Placed(track);
		CHECK(Frame(track, way, t0) == Then::kNothing);
		CHECK(Frame(track, way, t0 + DRAW_WAIT) == Then::kRebuild);
		CHECK(Frame(track, way, t0 + 2 * DRAW_WAIT) == Then::kReplace);
	}
}

TEST("StandInDraw: seen again, the spell is over; not wanted or in power armor, nothing is counted")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, No3D(), t0) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t0 + 2 * DRAW_WAIT) == Then::kReplace);
	Placed(track);
	// the fresh one is seen: replaces forgotten
	CHECK(Frame(track, Seen(), t0 + 2 * DRAW_WAIT + 1s) == Then::kSeenAgain);
	CHECK(Frame(track, Seen(), t0 + 2 * DRAW_WAIT + 2s) == Then::kNothing);  // said once
	CHECK(track.replaces == 0);
	// seen, then not wanted a while (a load, far away), then unseen: the wait starts from when they are wanted again
	CHECK(Frame(track, { .wanted = false }, t0 + 3 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, { .wanted = false }, t0 + 10 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 10 * DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 11 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 11 * DRAW_WAIT + 1s) == Then::kRebuild);
	// in a power armor frame its 3D is the frame's: never counted, however long
	Track armored;
	Placed(armored);
	auto busy = No3D();
	busy.busy = true;
	CHECK(Frame(armored, busy, t0) == Then::kNothing);
	CHECK(Frame(armored, busy, t0 + 100 * DRAW_WAIT) == Then::kNothing);
	CHECK(!armored.unseen);
}

TEST("StandInDraw: none standing is said once; placing failed, the next is placed plain, once")
{
	Track track;
	CHECK(Frame(track, NoneStanding(), t0) == Then::kNothing);
	CHECK(Frame(track, NoneStanding(), t0 + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, NoneStanding(), t0 + DRAW_WAIT) == Then::kSay);
	CHECK(Frame(track, NoneStanding(), t0 + 2 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, NoneStanding(), t0 + 20 * DRAW_WAIT) == Then::kNothing);
	CHECK(!track.plain);
	// one is placed and seen, then none again: said again
	Placed(track);
	CHECK(Frame(track, Seen(), t0 + 21 * DRAW_WAIT) == Then::kSeenAgain);
	CHECK(Frame(track, NoneStanding(), t0 + 22 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, NoneStanding(), t0 + 23 * DRAW_WAIT) == Then::kSay);

	Track failing;
	CHECK(Frame(failing, NoneStanding(true), t0) == Then::kNothing);
	CHECK(Frame(failing, NoneStanding(true), t0 + DRAW_WAIT) == Then::kPlain);
	CHECK(failing.plain);
	// the plain one fails too: said once, nothing more to try
	CHECK(Frame(failing, NoneStanding(true), t0 + DRAW_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(failing, NoneStanding(true), t0 + 2 * DRAW_WAIT) == Then::kSay);
	CHECK(Frame(failing, NoneStanding(true), t0 + 3 * DRAW_WAIT) == Then::kNothing);
}

TEST("StandInDraw: a plain stand-in stays plain until a load or a new look")
{
	Track track;
	track.plain = true;
	Placed(track);
	CHECK(track.plain);
	CHECK(Frame(track, Seen(), t0) == Then::kNothing);
	CHECK(track.plain);
	// already plain: the second fresh one is just a fresh one
	CHECK(Frame(track, No3D(), t0 + 1s) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 1s + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t0 + 1s + 2 * DRAW_WAIT) == Then::kReplace);
	Placed(track);
	CHECK(Frame(track, No3D(), t0 + 2s + 2 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 2s + 3 * DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t0 + 2s + 4 * DRAW_WAIT) == Then::kReplace);
	Afresh(track);
	CHECK(!track.plain);
	CHECK(track.replaces == 0);
}
