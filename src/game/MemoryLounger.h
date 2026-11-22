#pragma once

#include <string>

// A player left without controls by their own press on a memory's lounger that never seated them (LoungerRescue.h): the
// seat asked again, else the press undone. From each game's start, in a session or not; a save made in that state loads
// with the controls back but the press still marked (160 done, no seat): that press is undone too, with no seat asked.
namespace MemoryLounger
{
	// A game started: a new game, or a save loaded. Main thread.
	void OnGameStart();

	// Each frame, on the main thread (it looks twice a second).
	void Frame();

	// Each door's state and the guard's, for the dev channel's `lounger`.
	[[nodiscard]] std::string Describe();
}
