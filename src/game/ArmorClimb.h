#pragma once

#include <chrono>

// A stand-in climbing into or out of its power armor frame copy. The engine does the climb in the
// stand-in's own AI update: it lines the stand-in up with the frame, plays the animation, puts it in
// the armor (the ExtraPowerArmor the game keeps on an actor in a frame) and only then lets it stand
// free again. So the AI runs until the engine says the climb is over, not for a fixed time. A climb that
// outlasted the six seconds it was once given (a slow game loads the armor's body and animations
// slowly) could leave the stand-in half in, and the pieces it was given then hung on a body that was
// not in the frame: knock-kneed, with the shoulder plates standing out.
namespace ArmorClimb
{
	// The longest a climb is given. A stand-in that is not in (or out) by then is not waited for.
	constexpr auto LIMIT = std::chrono::seconds(15);

	enum class Step
	{
		kWait,      // still climbing: its AI keeps running
		kDone,      // the engine has finished it
		kCutShort,  // it took too long: its AI stops anyway
	};

	// a_in: the climb is into the frame (else out of it). a_engineIn: the engine has the stand-in in
	// power armor. a_settled: it is done with the frame as furniture (its sitting state is back to normal).
	constexpr Step Next(bool a_in, bool a_engineIn, bool a_settled, std::chrono::steady_clock::duration a_since)
	{
		if (a_engineIn == a_in && a_settled) {
			return Step::kDone;
		}
		return a_since >= LIMIT ? Step::kCutShort : Step::kWait;
	}
}
