#pragma once

#include "HangDetector.h"

// The hang watch. A freeze leaves no crash log, so a thread of ours looks at the game once a second:
// when the main thread hasn't read its window messages for iHangSeconds (a save, a load or a loading
// screen gets four times as long), it logs that with where the main thread is stuck and which other
// threads kept a processor busy meanwhile (what it may be waiting for), and where they were, writes the same
// as a short text report next to the log (F4Multiplayer-hang-<time>.txt, the newest three are kept),
// and logs again when the game answers. A minidump of every thread's stack is written as well only
// with bHangDump (HangWatch.cpp says why it is off by default). Once the player quits, the watch stops:
// the engine's shutdown reads no window messages and can take a while.
//
// All of these are thread-safe: the engine calls the plugin back on several of its threads.
namespace HangWatch
{
	// Starts the watch thread (off with iHangSeconds = 0), and removes report files a game closed
	// mid-write left behind. Called once the game's data is loaded, from F4SE's message, which comes on
	// the engine's data-loading thread: the startup before that is not watched.
	void Start();

	// A loading screen opened or closed (the menu event).
	void SetLoading(bool a_loading);

	// A save (a_what = Phase::kSave) or a load (Phase::kLoad) began, or with Phase::kPlay ended: F4SE's
	// messages before and after it.
	void SetSaveOrLoad(Phase a_what);
}
