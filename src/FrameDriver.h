#pragma once

// Old-gen's stand-in for F4SE's permanent task. F4SE 0.6.23 (the old-gen game's) has no AddTaskPermanent, so a thread of
// our own calls the task's work in its place, about once a millisecond, until a_done says the game is quitting.
//
// The task's work (main.cpp's OnFrame) never touches the game world: from the game's job threads it reads the drawn-frame
// count and posts the frame message, and the window procedure does the game work on the main thread. So the same work
// from a thread of ours runs as it did, off the main thread, and the main thread's work is unchanged. Chosen over:
// - re-adding a one-shot F4SE task each time it runs: when 0.6.23 runs its queue in a frame, and whether a task added
//   while the queue runs is run in the same pass (a spin on the main thread), was never read;
// - a call patched into Main::OnIdle: it needs old-gen's address and a hand-read call site first, and a patch in the
//   loop every part of the plugin hangs off is the riskiest kind;
// this needs no engine address and no patch. The frame watch already posts the frame message from its own thread when
// the task stops, so the chain was built to be driven from outside the game's threads.
namespace FrameDriver
{
	// Starts the thread. False if it couldn't be (logged).
	bool Start(void (*a_tick)(), bool (*a_done)());
}
