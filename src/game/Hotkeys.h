#pragma once

// Multiplayer hotkeys (set in the ini). Read only while the game window is in front and no menu
// or the console is open, so typing elsewhere never triggers them.
namespace Hotkeys
{
	enum class Action
	{
		kPlayerList,    // pressed
		kTeleportPick,  // tapped (released within HOLD_TIME)
		kTeleportGo,    // held for HOLD_TIME
		kPing,          // pressed
	};

	// Actions triggered since the last call. Call once per frame from the main thread.
	[[nodiscard]] std::vector<Action> Poll();
}
