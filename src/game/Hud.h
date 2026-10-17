#pragma once

#include "game/UiHold.h"

// The game's UI, as far as the plugin writes to it: the one guard on every write (UiHold), the loading screen
// watcher behind it, and the HUD's top-left messages. Everything that writes to the UI (the compass markers, our
// menu, map markers, the developer's gfx commands) asks IsSettled first and waits while the answer is no; a test
// on the source sees to it.
namespace Hud
{
	// Watches for loading screens. The game's one loading screen watcher: the frame, the compass and the hang
	// watch go by it too.
	void Install();

	// Whether a loading screen is up, as far as the plugin heard: from its menu's open event to its close
	// event, or at most a few minutes (a close never heard of must not keep everything off for the rest
	// of the run). Any thread.
	[[nodiscard]] bool LoadingScreenUp();

	// A save was loaded (or a new game begun): its own work ran, on the frame's thread, before its hold ends.
	// The engine rebuilds every menu over the next seconds; nothing is written to the UI meanwhile.
	void OnGameLoaded();

	// Whether the UI may be written to right now: no loading screen, save or load under way, and not just after
	// a loading screen or a load (see UiHold). Any thread.
	[[nodiscard]] bool IsSettled();

	// Why it may not be, or UiHold::Hold::kNone. Any thread.
	[[nodiscard]] UiHold::Hold Holding();

	// Shows the next waiting message. Called only from inside the HUD's own update (Compass hooks it),
	// the one place the HUD is written without racing the engine.
	void OnHudAdvance();

	// Queues a notification for the HUD. Any thread. Not logged: many of these name players.
	void Notify(const std::string& a_message);

	[[nodiscard]] std::string Describe();
}
