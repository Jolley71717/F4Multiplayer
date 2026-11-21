#include "Test.h"

#include "game/ActionState.h"

#include <string_view>

using namespace std::chrono_literals;
using ActionState::Answer;
using ActionState::Ask;
using ActionState::Drink;
using ActionState::Drinking;
using ActionState::Next;
using ActionState::Say;
using ActionState::Track;

namespace
{
	const auto START = ActionState::Clock::time_point{} + 1h;

	// A minute of frames of an owner in the state, on a graph that takes the ask or never does: the asks made.
	int Asked(Track& a_track, bool a_want, bool a_takes, ActionState::Clock::time_point a_from, Ask a_which)
	{
		int asked = 0;
		for (int ms = 0; ms < 60000; ms += 16) {
			const auto ask = Next(a_track, a_want, a_from + std::chrono::milliseconds(ms));
			if (ask != Ask::kNothing) {
				Answer(a_track, a_takes);
			}
			asked += ask == a_which;
		}
		return asked;
	}
}

TEST("action state: a stand-in whose owner never swims is asked nothing")
{
	Track track;
	CHECK(Asked(track, false, true, START, Ask::kEnter) == 0);
	CHECK(track.asks == 0);
	CHECK(!track.in);
}

TEST("action state: the swim is asked once as the owner swims, and the stop once as they no longer do")
{
	Track track;
	CHECK(Next(track, false, START) == Ask::kNothing);
	CHECK(Next(track, true, START + 1s) == Ask::kEnter);
	Answer(track, true);
	CHECK(track.in);
	CHECK(Asked(track, true, true, START + 1s + 16ms, Ask::kEnter) == 0);
	CHECK(Next(track, false, START + 2min) == Ask::kLeave);
	Answer(track, true);
	CHECK(!track.in);
	CHECK(Asked(track, false, true, START + 2min + 16ms, Ask::kLeave) == 0);
	CHECK(track.asks == 2);
	CHECK(track.taken == 2);
}

TEST("action state: an ask the graph refuses is asked again a few times, half a second apart, then let be")
{
	Track track;
	CHECK(Asked(track, true, false, START, Ask::kEnter) == ActionState::MAX_TRIES);
	CHECK(track.giveUps == 1);
	CHECK(track.taken == 0);
	// Spaced by RETRY.
	Track spaced;
	CHECK(Next(spaced, true, START) == Ask::kEnter);
	CHECK(Next(spaced, true, START + 200ms) == Ask::kNothing);
	CHECK(Next(spaced, true, START + ActionState::RETRY) == Ask::kEnter);
	// Given up on, the owner leaving the water asks the stop (the graph may have taken a start we were told it refused),
	// and the next swim gets its asks back.
	CHECK(Asked(track, false, false, START + 2min, Ask::kLeave) == ActionState::MAX_TRIES);
	CHECK(Asked(track, true, true, START + 4min, Ask::kEnter) == 1);
	CHECK(track.in);
}

TEST("action state: an ask taken at a later try ends the tries")
{
	Track track;
	CHECK(Next(track, true, START) == Ask::kEnter);
	Answer(track, false);
	CHECK(Next(track, true, START + ActionState::RETRY) == Ask::kEnter);
	Answer(track, true);
	CHECK(Asked(track, true, false, START + 1s, Ask::kEnter) == 0);
	CHECK(track.giveUps == 0);
}

TEST("action state: a graph built anew under a swimming owner is asked into the swim again")
{
	Track track;
	CHECK(Next(track, true, START) == Ask::kEnter);
	Answer(track, true);
	ActionState::Reset(track);
	CHECK(Next(track, true, START + 1s) == Ask::kEnter);  // at once, whatever the gap
	Answer(track, true);
	CHECK(Asked(track, true, true, START + 1s + 16ms, Ask::kEnter) == 0);
}

TEST("action state: a graph put back in its base state is asked out only of a state it was in")
{
	// It swam, was held, and its owner is on land as it is let go: it may still swim, so the stop is asked.
	Track swam;
	CHECK(Next(swam, true, START) == Ask::kEnter);
	Answer(swam, true);
	ActionState::Reset(swam);
	CHECK(Next(swam, false, START + 10s) == Ask::kLeave);
	Answer(swam, false);  // a graph in its base state refuses it
	CHECK(Asked(swam, false, false, START + 11s, Ask::kLeave) == ActionState::MAX_TRIES - 1);
	CHECK(!swam.in);
	// It never swam: nothing is asked of its new graph.
	Track dry;
	CHECK(Next(dry, false, START) == Ask::kNothing);
	ActionState::Reset(dry);
	CHECK(Asked(dry, false, true, START + 1s, Ask::kLeave) == 0);
	CHECK(dry.asks == 0);
}

TEST("action state: an owner in and out of the water by the frame asks twice a second, not every frame")
{
	Track track;
	int   asks = 0;
	for (int frame = 0; frame < 625; ++frame) {  // ten seconds
		const auto ask = Next(track, frame % 2 == 0, START + std::chrono::milliseconds(frame * 16));
		if (ask != Ask::kNothing) {
			Answer(track, true);
			++asks;
		}
	}
	CHECK(asks <= 21);
	CHECK(asks >= 2);
}

TEST("action state: a player's swim flag or the move mode's swim bit says an actor swims")
{
	CHECK(ActionState::Swims(true, 0));
	CHECK(ActionState::Swims(false, 0x0400));
	CHECK(ActionState::Swims(false, 0x0441));
	CHECK(!ActionState::Swims(false, 0x0041));
	CHECK(!ActionState::Swims(false, 0));
}

TEST("action state: the bare swim start is the one the action's idles pick for the graph and the weapon")
{
	using namespace std::string_view_literals;
	CHECK(ActionState::SwimStartEvent(true, false, false) == "g_SwimStart_MT"sv);
	CHECK(ActionState::SwimStartEvent(true, false, true) == "g_SwimStart"sv);
	CHECK(ActionState::SwimStartEvent(true, true, false) == "g_SwimStart_MT_PA"sv);
	CHECK(ActionState::SwimStartEvent(true, true, true) == "g_SwimStart_PA"sv);
	CHECK(ActionState::SwimStartEvent(false, false, false) == "SwimStart"sv);  // a dog, a mirelurk king
	CHECK(ActionState::SwimStartEvent(false, false, true) == "SwimStart"sv);
	CHECK(ActionState::DrinkStartEvent(false) == "g_drinkStart"sv);
	CHECK(ActionState::DrinkStartEvent(true) == "swimDrinkStart"sv);
}

TEST("action state: the graph's drink tags are told apart from every other tag")
{
	CHECK(ActionState::DrinkOf("g_drinkStart") == Drink::kBegin);
	CHECK(ActionState::DrinkOf("swimDrinkStart") == Drink::kBegin);
	CHECK(ActionState::DrinkOf("LoopingActivate") == Drink::kBegin);
	CHECK(ActionState::DrinkOf("drinkEnd") == Drink::kEnd);
	CHECK(ActionState::DrinkOf("reloadStateEnter") == Drink::kNone);
	CHECK(ActionState::DrinkOf("drink") == Drink::kNone);
	CHECK(ActionState::DrinkOf("") == Drink::kNone);
}

TEST("action state: a drink is said as it starts, again every second while it lasts, and as it ends")
{
	Drinking drinking;
	CHECK(ActionState::Heard(drinking, Drink::kBegin, START) == Say::kStart);
	int starts = 0;
	for (int ms = 16; ms < 5000; ms += 16) {
		starts += ActionState::Tick(drinking, false, START + std::chrono::milliseconds(ms)) == Say::kStart;
	}
	CHECK(starts == 4);  // at 1, 2, 3 and 4 s
	CHECK(ActionState::Heard(drinking, Drink::kEnd, START + 5s) == Say::kStop);
	CHECK(ActionState::Tick(drinking, false, START + 6s) == Say::kNothing);
	CHECK(ActionState::Heard(drinking, Drink::kEnd, START + 7s) == Say::kNothing);  // an end with no drink
}

TEST("action state: a drink's tags that come by the loop say it no more often than the renewal")
{
	Drinking drinking;
	int      starts = 0;
	for (int ms = 0; ms < 5000; ms += 100) {
		starts += ActionState::Heard(drinking, Drink::kBegin, START + std::chrono::milliseconds(ms)) == Say::kStart;
		starts += ActionState::Tick(drinking, false, START + std::chrono::milliseconds(ms + 50)) == Say::kStart;
	}
	CHECK(starts == 5);
}

TEST("action state: a drink whose end the graph never announced ends as the player walks, or when nothing was heard for long")
{
	Drinking walked;
	CHECK(ActionState::Heard(walked, Drink::kBegin, START) == Say::kStart);
	CHECK(ActionState::Tick(walked, true, START + 300ms) == Say::kStop);
	CHECK(ActionState::Tick(walked, true, START + 316ms) == Say::kNothing);
	CHECK(!walked.on);

	Drinking quiet;
	CHECK(ActionState::Heard(quiet, Drink::kBegin, START) == Say::kStart);
	int  stops = 0;
	auto stoppedAt = START;
	for (int ms = 16; ms < 60000; ms += 16) {
		if (ActionState::Tick(quiet, false, START + std::chrono::milliseconds(ms)) == Say::kStop) {
			++stops;
			stoppedAt = START + std::chrono::milliseconds(ms);
		}
	}
	CHECK(stops == 1);
	CHECK(stoppedAt - START >= ActionState::UNHEARD);
	CHECK(stoppedAt - START < ActionState::UNHEARD + 100ms);
}

TEST("action state: a stand-in drinks a moment past the last start it heard, so a lost stop ends and a lost start is made up")
{
	// Starts come every RENEW; one is lost; the stop is lost too.
	auto until = START + ActionState::HOLD;                             // heard at 0
	CHECK(ActionState::Drinks(until, false, false, START + 1900ms));    // the one at 1 s was lost
	until = START + 2s + ActionState::HOLD;                             // heard at 2 s
	CHECK(ActionState::Drinks(until, false, false, START + 4s));
	CHECK(!ActionState::Drinks(until, false, false, START + 2s + ActionState::HOLD));  // nothing more came: over
	CHECK(ActionState::HOLD > 2 * ActionState::RENEW);
	// Its friend walks or is in the air: over, whatever was heard.
	CHECK(!ActionState::Drinks(until, true, false, START + 2s));
	CHECK(!ActionState::Drinks(until, false, true, START + 2s));
	// A stop heard clears it.
	CHECK(!ActionState::Drinks({}, false, false, START));
}
