// Which spoken lines a game reports (src/game/LineShare.h): the player's always, an NPC's it runs, never a combat bark,
// and a creature's only when the game has a recording of it (a growl is a sound descriptor with no voice file).

#include "Test.h"

#include "game/LineShare.h"

TEST("line share: the player's lines go, combat barks never")
{
	using LineShare::Reported;
	CHECK(Reported(false, true, false, false, false));
	CHECK(Reported(false, true, true, true, false));
	CHECK(!Reported(true, true, false, false, true));
	CHECK(!Reported(true, false, true, false, true));
	CHECK(!Reported(true, false, true, true, true));
}

TEST("line share: an NPC's line goes when ours, recorded or not; a creature's only with a recording")
{
	using LineShare::Reported;
	// A settler ours to run, with or without a recording (shown as text then); not one another game runs.
	CHECK(Reported(false, false, true, false, true));
	CHECK(Reported(false, false, true, false, false));
	CHECK(!Reported(false, false, false, false, true));
	// An eyebot's broadcast and Liberty Prime's line are recorded: shared; a feral dog's growl names a sound descriptor
	// and no file: not.
	CHECK(Reported(false, false, true, true, true));
	CHECK(!Reported(false, false, true, true, false));
	CHECK(!Reported(false, false, false, true, true));
}
