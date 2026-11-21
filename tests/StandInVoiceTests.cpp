// A stand-in says nothing of its own (game/StandInVoice.h): a line the engine has it saying is counted once and ended every
// frame it goes on, and nothing is done with the ending off.

#include "Test.h"

#include "game/StandInVoice.h"

using namespace StandInVoice;

TEST("StandInVoice: a line is counted as it is first seen and ended every frame it goes on")
{
	Track track;
	CHECK(!Frame(track, { .talking = false, .voiceState = 0 }, DEFAULT));
	CHECK(track.lines == 0);
	// requested: not talking yet, but on its way
	CHECK(Frame(track, { .talking = false, .voiceState = 1 }, DEFAULT));
	CHECK(track.lines == 1);
	CHECK(track.ended == 1);
	// the same line going on (an end that did not take): ended again, counted once
	CHECK(Frame(track, { .talking = true, .voiceState = 3 }, DEFAULT));
	CHECK(Frame(track, { .talking = true, .voiceState = 3 }, DEFAULT));
	CHECK(track.lines == 1);
	CHECK(track.ended == 1);
	// quiet, then a second line
	CHECK(!Frame(track, {}, DEFAULT));
	CHECK(Frame(track, { .talking = true, .voiceState = 2 }, DEFAULT));
	CHECK(track.lines == 2);
	CHECK(track.ended == 2);
}

TEST("StandInVoice: with the ending off, lines are counted and left alone")
{
	Track track;
	CHECK(!Frame(track, { .talking = true, .voiceState = 3 }, kQuietFlag));
	CHECK(!Frame(track, { .talking = true, .voiceState = 3 }, 0));
	CHECK(track.lines == 1);
	CHECK(track.ended == 0);
	CHECK(!Frame(track, {}, 0));
	CHECK(!Frame(track, { .talking = true }, 0));
	CHECK(track.lines == 2);
	CHECK(track.ended == 0);
}

TEST("StandInVoice: talking alone, or a voice state alone, is speaking")
{
	CHECK(Speaking({ .talking = true, .voiceState = 0 }));
	CHECK(Speaking({ .talking = false, .voiceState = 1 }));
	CHECK(!Speaking({}));
}
