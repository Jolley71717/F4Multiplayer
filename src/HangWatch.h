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

	// Where the main thread (the one that made a_window) is at this moment, as the hang report puts it
	// ("at Fallout4.exe+0x1A2B3C; return addresses on its stack: ..."), for the frame watch's report of a
	// main thread that answers its window but runs no frame of ours. The thread is stopped for the look,
	// as for a hang. Any thread but the main one.
	[[nodiscard]] std::string DescribeMainThread(void* a_window);

	// The id of the thread that made a_window (the main thread), 0 when unknown.
	[[nodiscard]] std::uint32_t WindowThread(void* a_window);

	// One of the game's threads at one moment, stopped for the look the way the hang report stops the main thread:
	// where it was and the likely return addresses on its stack (nearest first). Not looked (`looked` false) when the
	// id is the caller's own thread, not the game's, or can't be opened.
	struct ThreadLook
	{
		bool                        looked = false;
		std::uintptr_t              ip = 0;
		std::vector<std::uintptr_t> callers;
	};
	[[nodiscard]] ThreadLook LookAtThread(std::uint32_t a_threadId, std::size_t a_maxCallers);

	// Copies a_size bytes of the game's memory at a_address, which may not be mapped (read rather than dereferenced:
	// an address taken from another thread's register, or a list another thread changes). False when it can't be read.
	[[nodiscard]] bool ReadMemory(std::uintptr_t a_address, void* a_out, std::size_t a_size);
}
