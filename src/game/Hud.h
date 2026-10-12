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

	// Shows the next waiting message. Called only from inside the HUD's own update (Compass hooks it),
	// the one place the HUD is written without racing the engine.
	void OnHudAdvance();

	// Queues a notification for the HUD and logs it. Any thread.
	void Notify(const std::string& a_message);

	[[nodiscard]] std::string Describe();
}
