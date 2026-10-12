#pragma once

// The hang watch. A freeze leaves no crash log, so a thread of ours looks at the game once a second:
// when the main thread hasn't read its window messages for iHangSeconds (a loading screen gets four
// times as long), it logs that and writes a small dump of every thread's stack next to the log
// (F4Multiplayer-hang-<time>.dmp, the newest three are kept), and logs again when the game answers.
namespace HangWatch
{
	// Starts the watch thread (off with iHangSeconds = 0). Main thread, once the game's data is loaded:
	// the startup before that is not watched.
	void Start();

	// A loading screen opened or closed. Main thread.
	void SetLoading(bool a_loading);
}
