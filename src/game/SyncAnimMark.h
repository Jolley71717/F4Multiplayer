#pragma once

#include <string>

// The player's synced-animation mark (ActorState::inSyncAnim): left on with no synced animation behind it, the player
// gets no input at all (no walking, looking, shooting or Pip-Boy). From each game's start until settled, a mark with
// nothing running is cleared, once (see StuckSyncAnim.h for why it happens and what counts as running). For every
// player, not just the rig: a save made while stuck loads stuck.
namespace SyncAnimMark
{
	// A game started: a new game, or a save loaded. (The run's first game is looked at without it.) Main thread.
	void OnGameStart();

	// Each frame, on the main thread.
	void Frame();

	// The mark and the guard's state, for the playerinput dev command.
	[[nodiscard]] std::string Describe();

	// Test (dev channel): turns the mark on with nothing behind it, as a bad start does; the guard is left settled, so the
	// player stays stuck until the next game start (a save made now loads stuck). Main thread.
	std::string SetForTest();
}
