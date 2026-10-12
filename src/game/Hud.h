#pragma once

// Messages for the player (the HUD's top-left notifications).
namespace Hud
{
	// Watches for loading screens: nothing is written to the HUD while one is up or just closed.
	void Install();

	// A save was loaded: the HUD is rebuilt over the next seconds; nothing is written to it meanwhile.
	void OnGameLoaded();

	// Whether the HUD may be written to right now (no loading screen, and not just after one or a load).
	[[nodiscard]] bool IsSettled();

	// Shows what waited for the HUD to be back. Main thread, every frame.
	void Frame();

	// Shows a notification and logs it. Main thread only.
	void Notify(const std::string& a_message);

	[[nodiscard]] std::string Describe();
}
