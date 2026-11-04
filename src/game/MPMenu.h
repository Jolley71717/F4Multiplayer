#pragma once

#include <cstdint>

// The multiplayer menu of our own: a menu of the game's UI (F4MPMenu) with a movie of ours (Interface/F4Multiplayer/MPMenu.swf,
// built from ui/MPMenu by tools/flash/build.sh), opened over the main or pause menu by its Multiplayer entry
// (MainMenuEntry) and lined up beside the main menu's list, or by the menu key in the middle of the screen. Its input is its own while it is open: the main menu
// underneath keeps its state. It shows the pages of the F1 menu (Menu), which decides what they are and what they do.
namespace MPMenu
{
	// Once the game's data is loaded (the UI exists by then, and the main menu comes after).
	void Register();

	// Main thread. The main menu's panel box on its 1280x720 stage, which ours is laid out beside and scaled to.
	void Open(float a_left, float a_top, float a_right, float a_bottom);

	// Main thread. In the middle of the screen, with no main menu to line up beside (the menu key). The UI must have settled.
	void OpenCentered();

	// Main thread. A page to show: its text, its rows and what each does (a_hints, one per row, "" for nothing to say). It
	// is drawn in the menu's own update. a_keepRow: it's the page showing again, so the same row stays chosen.
	// a_heading: the mod's name and version over the text.
	// a_players: a list at the right of the rows (Party::PanelPlayers), or a line of text with no tabs; empty for none.
	// a_header: one such player at the top of the page, under the status line's place; "" for none.
	void SetPage(std::uint32_t a_serial, std::string a_body, std::vector<std::string> a_rows, std::vector<std::string> a_hints, bool a_keepRow, bool a_heading, std::vector<std::string> a_players, std::string a_header);

	// Main thread. Only the shown page's status line changes, so its rows are left alone. A page asked for since wins.
	void SetStatus(std::string a_text);

	// Main thread. On: the next key or mouse button goes to Menu::PanelKey, not to the menu. A gamepad's still work the menu.
	void CaptureKey(bool a_on);

	// Main thread.
	void Close();

	// Main thread. Esc while open in the middle of the screen, where the game keeps it from the menu: the way back, or
	// cancels a keybind's wait. Menu::Frame reads the key.
	void EscPressed();

	// In the HUD's own update: what's in its middle is hidden while the panel is up in the middle of the screen, where it would sit on the rows.
	void OnHudAdvance(RE::IMenu* a_hud);

	// Opened in the middle of the screen. Any thread.
	[[nodiscard]] bool Centered();

	// The player's HUD color (Settings > Display), 0xRRGGBB. Main thread.
	[[nodiscard]] std::uint32_t HudColor();

	// Opened and not closed since, by us or by the game. Any thread.
	[[nodiscard]] bool Showing();
}
