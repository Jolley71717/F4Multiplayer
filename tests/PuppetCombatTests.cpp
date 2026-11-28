#include "Test.h"
#include "game/PuppetCombat.h"

TEST("PuppetCombat: the engine's combat update runs for no puppet, except while its AI is let run")
{
	CHECK(PuppetCombat::Runs(false, false));  // an actor of the game's own
	CHECK(PuppetCombat::Runs(false, true));
	CHECK(!PuppetCombat::Runs(true, false));  // a stand-in, a mirrored NPC
	CHECK(PuppetCombat::Runs(true, true));    // getting into power armor
}
