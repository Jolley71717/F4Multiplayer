// A friend who stands near us and is not shown (game/StandInDraw.h): every way a stand-in can fail to be seen is named; unseen for
// DRAW_WAIT (LOAD_WAIT while its 3D or cell is on the way) it is asked to build its 3D again, then placed again, then placed again
// plain, then left alone and said once; none standing is said once, or, when placing failed, the next is placed plain; seen again,
// it starts over. A stand-in that is only loading (after a placement, a travel, its cell coming drawn) is never acted on.

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
		facts.fade = 0.0f;
		facts.scale = 0.0f;
		facts.parts = 0;
		return facts;
	}

	Facts Culled()
	{
		auto facts = Seen();
		facts.culled = true;
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
	// what is still on the way gets the time a load takes
	CHECK(WaitFor(Why::kNo3D) == LOAD_WAIT);
	CHECK(WaitFor(Why::kCell) == LOAD_WAIT);
	CHECK(WaitFor(Why::kCulled) == DRAW_WAIT);
	CHECK(WaitFor(Why::kBare) == DRAW_WAIT);
	CHECK(LOAD_WAIT > DRAW_WAIT);
}

TEST("StandInDraw: a fresh stand-in whose 3D is still loading is left to load (the rig's slowest was 6.6 s)")
{
	// 3D after 6.6 s, fading in from nothing with only the body on at first: nothing done, nothing said
	Track track;
	Placed(track);
	auto t = t0;
	for (; t < t0 + 6600ms; t += 100ms) {
		CHECK(Frame(track, No3D(), t) == Then::kNothing);
	}
	auto arriving = Seen();
	arriving.fade = 0.0f;
	arriving.parts = 0;
	CHECK(Frame(track, arriving, t) == Then::kNothing);
	arriving.parts = 2;
	CHECK(Frame(track, arriving, t + 500ms) == Then::kNothing);
	CHECK(Frame(track, Seen(), t + 1s) == Then::kNothing);  // never acted on, so no "shown again" either
	CHECK(!track.acted);
	CHECK(!track.rebuilt);

	// one whose 3D comes just before LOAD_WAIT runs out, still faded: it gets DRAW_WAIT from then, not the time already spent
	Track late;
	Placed(late);
	CHECK(Frame(late, No3D(), t0) == Then::kNothing);
	CHECK(Frame(late, No3D(), t0 + LOAD_WAIT - 1ms) == Then::kNothing);
	auto faded = Seen();
	faded.fade = 0.0f;
	CHECK(Frame(late, faded, t0 + LOAD_WAIT) == Then::kNothing);
	CHECK(Frame(late, faded, t0 + LOAD_WAIT + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(late, faded, t0 + LOAD_WAIT + DRAW_WAIT) == Then::kRebuild);  // still faded out DRAW_WAIT after its 3D came
}

TEST("StandInDraw: a stand-in whose cell comes drawn late, or seen again after our own load, gets a whole load's wait")
{
	// placed into a cell not drawn yet (a travel's cells attaching): the cell comes at 10 s, then its 3D loads for 12 s more
	Track track;
	Placed(track);
	auto cell = No3D();
	cell.cellDrawn = false;
	CHECK(Frame(track, cell, t0) == Then::kNothing);
	CHECK(Frame(track, cell, t0 + 10s) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 10s + 1ms) == Then::kNothing);  // the cell came: the 3D's load starts now
	CHECK(Frame(track, No3D(), t0 + 22s) == Then::kNothing);
	CHECK(Frame(track, Seen(), t0 + 22s + 1ms) == Then::kNothing);
	CHECK(!track.acted);
	// ... but one that never gets its 3D after the cell came is mended a load's wait later
	CHECK(Frame(track, { .wanted = false }, t0 + 30s) == Then::kNothing);  // our own loading screen
	CHECK(Frame(track, No3D(), t0 + 40s) == Then::kNothing);                // out of it: the stand-in's 3D on its way again
	CHECK(Frame(track, No3D(), t0 + 40s + LOAD_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 40s + LOAD_WAIT) == Then::kRebuild);
}

TEST("StandInDraw: unseen for its wait is a rebuild, then a fresh stand-in, then a plain one, then left alone and said once")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, No3D(), t0) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + LOAD_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + LOAD_WAIT) == Then::kRebuild);
	// the rebuild gets a wait of its own
	CHECK(Frame(track, No3D(), t0 + LOAD_WAIT + 1s) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 2 * LOAD_WAIT) == Then::kReplace);
	CHECK(track.replaces == 1);
	CHECK(!track.plain);
	// the fresh one, watched afresh: its own wait, its own rebuild, then the plain one
	Placed(track);
	auto t = t0 + 2 * LOAD_WAIT + 100ms;
	CHECK(Frame(track, No3D(), t) == Then::kNothing);
	CHECK(Frame(track, No3D(), t + LOAD_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t + 2 * LOAD_WAIT) == Then::kPlain);
	CHECK(track.replaces == 2);
	CHECK(track.plain);
	Placed(track);
	t = t + 2 * LOAD_WAIT + 100ms;
	CHECK(Frame(track, No3D(), t) == Then::kNothing);
	CHECK(Frame(track, No3D(), t + LOAD_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t + 2 * LOAD_WAIT) == Then::kGiveUp);
	CHECK(Frame(track, No3D(), t + 3 * LOAD_WAIT) == Then::kNothing);
	CHECK(track.replaces == 2);
	CHECK(track.plain);
}

TEST("StandInDraw: a culled 3D is rebuilt after DRAW_WAIT; the new 3D loads in its own time and is judged when it comes")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, Seen(), t0) == Then::kNothing);
	CHECK(Frame(track, Culled(), t0 + 1s) == Then::kNothing);
	CHECK(Frame(track, Culled(), t0 + 1s + DRAW_WAIT) == Then::kRebuild);
	auto t = t0 + 1s + DRAW_WAIT;
	// Reset3D took the 3D away; it loads for 8 s and comes back culled again
	CHECK(Frame(track, No3D(), t + 100ms) == Then::kNothing);
	CHECK(Frame(track, No3D(), t + 8s) == Then::kNothing);
	CHECK(Frame(track, Culled(), t + 8s) == Then::kNothing);  // the new 3D came: judged from now
	CHECK(Frame(track, Culled(), t + 8s + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, Culled(), t + 8s + DRAW_WAIT) == Then::kReplace);
	// once more, and seen this time: the culled one was mended
	Track once;
	Placed(once);
	CHECK(Frame(once, Culled(), t0) == Then::kNothing);
	CHECK(Frame(once, Culled(), t0 + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(once, No3D(), t0 + DRAW_WAIT + 100ms) == Then::kNothing);
	CHECK(Frame(once, Seen(), t0 + DRAW_WAIT + 2s) == Then::kSeenAgain);
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
		const auto wait = WaitFor(WhyNot(way));
		Track track;
		Placed(track);
		CHECK(Frame(track, way, t0) == Then::kNothing);
		CHECK(Frame(track, way, t0 + wait - 1ms) == Then::kNothing);
		CHECK(Frame(track, way, t0 + wait) == Then::kRebuild);
		CHECK(Frame(track, way, t0 + 2 * wait) == Then::kReplace);
	}
}

TEST("StandInDraw: seen again, the spell is over; not wanted or in power armor, nothing is counted")
{
	Track track;
	Placed(track);
	CHECK(Frame(track, No3D(), t0) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + LOAD_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t0 + 2 * LOAD_WAIT) == Then::kReplace);
	Placed(track);
	// the fresh one is seen: replaces forgotten
	CHECK(Frame(track, Seen(), t0 + 2 * LOAD_WAIT + 1s) == Then::kSeenAgain);
	CHECK(Frame(track, Seen(), t0 + 2 * LOAD_WAIT + 2s) == Then::kNothing);  // said once
	CHECK(track.replaces == 0);
	// seen, then not wanted a while (a load, far away), then unseen: the wait starts from when they are wanted again
	auto t = t0 + 3 * LOAD_WAIT;
	CHECK(Frame(track, { .wanted = false }, t) == Then::kNothing);
	CHECK(Frame(track, { .wanted = false }, t + 10 * LOAD_WAIT) == Then::kNothing);
	t += 10 * LOAD_WAIT + 1s;
	CHECK(Frame(track, Culled(), t) == Then::kNothing);
	CHECK(Frame(track, Culled(), t + DRAW_WAIT - 1ms) == Then::kNothing);
	CHECK(Frame(track, Culled(), t + DRAW_WAIT) == Then::kRebuild);
	// in a power armor frame its 3D is the frame's: never counted, however long
	Track armored;
	Placed(armored);
	auto busy = No3D();
	busy.busy = true;
	CHECK(Frame(armored, busy, t0) == Then::kNothing);
	CHECK(Frame(armored, busy, t0 + 100 * LOAD_WAIT) == Then::kNothing);
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
	CHECK(Frame(track, Culled(), t0 + 1s) == Then::kNothing);
	CHECK(Frame(track, Culled(), t0 + 1s + DRAW_WAIT) == Then::kRebuild);
	CHECK(Frame(track, Culled(), t0 + 1s + 2 * DRAW_WAIT) == Then::kReplace);
	Placed(track);
	CHECK(Frame(track, No3D(), t0 + 2s + 2 * DRAW_WAIT) == Then::kNothing);
	CHECK(Frame(track, No3D(), t0 + 2s + 2 * DRAW_WAIT + LOAD_WAIT) == Then::kRebuild);
	CHECK(Frame(track, No3D(), t0 + 2s + 2 * DRAW_WAIT + 2 * LOAD_WAIT) == Then::kReplace);
	Afresh(track);
	CHECK(!track.plain);
	CHECK(track.replaces == 0);
}
