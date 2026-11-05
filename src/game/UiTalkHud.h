#pragma once

#include <string>

// The push-to-talk HUD (bTalkHud): a microphone and the mic's level, near the bottom of the screen, while the player transmits.
// A menu of the game's UI (F4MPTalkHud) with a movie of ours (ui/TalkHud), which doesn't pause the game, take input or show a
// cursor. It stays open while voice can be used, so a press doesn't have to wait for a movie to load.
namespace UiTalkHud
{
	// Once the game's data is loaded.
	void Register();

	// Main thread, every frame, after Voice's.
	void Frame();

	// For the dev channel. Main thread.
	[[nodiscard]] std::string Describe();
}
