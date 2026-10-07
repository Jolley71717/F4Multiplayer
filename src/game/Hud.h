#pragma once

// Messages for the player (the HUD's top-left notifications).
namespace Hud
{
	// Shows a notification and logs it. Main thread only.
	void Notify(const std::string& a_message);
}
