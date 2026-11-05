#pragma once

// The multiplayer menu (F1 by default; iKeyMenu): host a session, join a friend, see who's in the
// session and change the main settings, without the ini or the console. Its pages show in the panel
// (MPMenu), or, if the panel's movie won't load, in the game's own message box (a header, a body and
// up to four buttons), one box per page with longer lists getting a "More" button. That fallback goes
// once the panel has been out for a version. A button's action runs on the next frame, and most show
// the next page. Main thread only.
namespace Menu
{
	// Shows the main page in a message box.
	void Open();

	// The menu key: the multiplayer menu of our own in the middle of the screen, once the UI has settled. If its movie won't
	// load, the message box (Open).
	void OpenFromKey();

	// Shows the main page in the multiplayer menu of our own (MPMenu), which shows every page from then on until it closes.
	void OpenInPanel();

	// From the panel's movie, on any thread: row a_row of page a_serial was pressed (rows leave out the page's way back).
	void PanelPress(std::uint32_t a_serial, std::uint32_t a_row);

	// Main thread: Esc, Tab, the gamepad's B or the back hint in the panel: the page's way back, or closed with no page.
	void PanelBack();

	// Main thread: the key or mouse button heard for a keybind (MPMenu::CaptureKey): a Windows virtual key, or the game's
	// code for a mouse button.
	void PanelKey(bool a_keyboard, std::uint32_t a_code);

	// Main thread. A key pressed while a password is being typed: a_key its virtual key, a_character what it types (0: nothing).
	void PanelTyped(std::uint32_t a_key, std::uint32_t a_character);

	// Carries out the button pressed on the page showing and shows what follows. Every frame.
	void Frame();

	// Dev: presses button a_index of the page showing, as the player would.
	void Press(std::uint32_t a_index);

	[[nodiscard]] std::string Describe();
}
