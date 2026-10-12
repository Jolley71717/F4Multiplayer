#pragma once

// Messages for the player (the HUD's top-left notifications).
namespace Hud
{
	// Watches for loading screens: nothing is written to the HUD while one is up or just closed. The
	// game's one loading screen watcher: the frame, the compass and the hang watch go by it too.
	void Install();

	// Whether a loading screen is up. Any thread.
	[[nodiscard]] bool LoadingScreenUp();

	// A save was loaded: the HUD is rebuilt over the next seconds; nothing is written to it meanwhile.
	// (While the load itself is under way the save/load hold keeps the HUD alone; see SaveLoadHold.)
	void OnGameLoaded();

	// Whether the HUD may be written to right now (no loading screen, save or load, and not just after
	// a loading screen or a load).
	[[nodiscard]] bool IsSettled();

	// Shows the next waiting message. Called only from inside the HUD's own update (Compass hooks it),
	// the one place the HUD is written without racing the engine.
	void OnHudAdvance();

	// Queues a notification for the HUD and logs it. Any thread.
	void Notify(const std::string& a_message);

	[[nodiscard]] std::string Describe();
}
