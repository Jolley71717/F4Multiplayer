// When the power armor HUD's compass shows nothing any more (src/game/Compass.h, Compass::Stopped): its updates stop when
// the player gets out of the armor, and the count of markers it showed must not keep the HUD busy every frame.

#include "Test.h"

#include "game/Compass.h"

TEST("compass: the power armor HUD whose updates stopped while the HUD's go on shows nothing, after a few frames; a missed frame is not a stop")
{
	Compass::Stopped stopped;
	std::uint32_t    hud = 0, armor = 0;
	// In power armor: both update every frame.
	for (int i = 0; i < 20; ++i) {
		CHECK(!stopped.Frame(++hud, ++armor));
	}
	// The armor HUD missed one frame (its update ran just after ours): not a stop.
	CHECK(!stopped.Frame(++hud, armor));
	CHECK(!stopped.Frame(++hud, ++armor));
	// A menu or a loading screen stops both: not a stop either.
	for (int i = 0; i < 10; ++i) {
		CHECK(!stopped.Frame(hud, armor));
	}
	// Out of the armor: the HUD goes on, the armor HUD doesn't.
	for (std::uint32_t i = 1; i < Compass::Stopped::FRAMES; ++i) {
		CHECK(!stopped.Frame(++hud, armor));
	}
	CHECK(stopped.Frame(++hud, armor));
	CHECK(stopped.Frame(++hud, armor));
	// Back in: it updates again.
	CHECK(!stopped.Frame(++hud, ++armor));
}
