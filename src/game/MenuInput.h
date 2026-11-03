#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// A button press handed to one menu alone, as the game's input code hands a menu a frame's button: the menu's own
// BSInputEventUser::OnButtonEvent, after its ShouldHandleEvent. Nothing else hears it (not the other menus, not the
// player's controls), so it does in that menu what the key does there and nothing more. QuestSync closes a settlement
// list a friend's stage opened this way: with its own cancel (Tab), which answers the script waiting on the list.
namespace MenuInput
{
	// A ButtonEvent laid out as the game's. RE::ButtonEvent can't be made here: its base class's virtual functions exist
	// only in the game. The vtable is the game's own. The dev channel's `input` command makes these too.
	struct GameButtonEvent
	{
		std::uintptr_t    vtable = 0;             // 00
		std::int32_t      device = 0;             // 08
		std::int32_t      deviceID = 0;           // 0C
		std::int32_t      eventType = 0;          // 10
		const void*       next = nullptr;         // 18
		std::uint32_t     timeCode = 0xFFFFFFFF;  // 20
		std::int32_t      handled = 0;            // 24
		RE::BSFixedString strUserEvent;           // 28
		std::int32_t      idCode = -1;            // 30
		bool              disabled = false;       // 34
		float             value = 0.0f;           // 38
		float             heldDownSecs = 0.0f;    // 3C
	};
	static_assert(sizeof(GameButtonEvent) == sizeof(RE::ButtonEvent));

	// A button event for a_userEvent on button a_id (-1: none) of a_device (the game's numbering: keyboard 0, mouse 1,
	// gamepad 2): its press (a_down, held 0 s) or its release after a_heldSecs. The event lives in one of a few slots used
	// in turn and never freed, so it stays readable after the call (a receiver might keep the pointer for later in the
	// frame). Main thread.
	GameButtonEvent& NewEvent(std::int32_t a_device, std::string_view a_userEvent, std::int32_t a_id, bool a_down, float a_heldSecs);

	// Hands a_menu alone the press (a_down) or the release of a_userEvent on key a_key, if the menu is up and takes it
	// now. Main thread, with the UI settled (Hud::IsSettled). Empty when it was handed over, else why not.
	std::string Send(std::string_view a_menu, std::string_view a_userEvent, std::uint32_t a_key, bool a_down, float a_heldSecs = 0.0f);

	// The keyboard's Tab (a DirectInput scan code): the control map's Cancel in a menu.
	inline constexpr std::uint32_t TAB = 0x0F;
	// How long a tap holds its key before the release: a quick press of a real key.
	inline constexpr auto TAP_HOLD = std::chrono::milliseconds(100);

	// A tap of a_userEvent on key a_key to a_menu alone: the press now (Send), its release TAP_HOLD later (Frame), as a
	// real key's come in different frames. The release goes only if the menu is still up and takes it then, as a real
	// key's would. Main thread, with the UI settled. Empty when the press was handed over, else why not.
	std::string Tap(std::string_view a_menu, std::string_view a_userEvent, std::uint32_t a_key);

	// Sends the taps' releases that are due. Every frame, on the main thread.
	void Frame();
}
