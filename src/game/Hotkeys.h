#pragma once

// Multiplayer hotkeys (set in the ini). Read only while the game window is in front and no menu
// or the console is open, so typing elsewhere never triggers them.
namespace Hotkeys
{
	enum class Action
	{
		kMenu,          // pressed: the multiplayer menu (works outside a session too)
		kPlayerList,    // pressed
		kTeleportPick,  // tapped (released within HOLD_TIME)
		kTeleportGo,    // held for HOLD_TIME
		kPing,          // pressed
		kEmotePoint,    // pressed: emotes, played on your stand-in for others
		kEmoteCheer,
		kEmoteClap,
	};

	// Actions triggered since the last call. Call once per frame from the main thread.
	[[nodiscard]] std::vector<Action> Poll();

	// The key is down and the game window is in front (menus don't matter).
	[[nodiscard]] bool Held(std::uint32_t a_vk);

	// Why a key may be doing nothing right now: focus, menu mode, the console, chord keys held,
	// each hotkey's code and whether it's down, and how often Poll ran.
	[[nodiscard]] std::string Describe();
}
