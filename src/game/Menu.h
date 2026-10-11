#pragma once

// The multiplayer menu (F1 by default; iKeyMenu): host a session, join a friend, see who's in the
// session and change the main settings, without the ini or the console. It's built from the
// game's own message box (the "Are you sure?" dialog: a header, a body and up to four buttons),
// one box per page; longer lists get a "More" button. A button's action runs on the next frame,
// and most show the next page. Main thread only.
namespace Menu
{
	// Shows the main page.
	void Open();

	// Carries out the button pressed on the page showing and shows what follows. Every frame.
	void Frame();

	// Dev: presses button a_index of the page showing, as the player would.
	void Press(std::uint8_t a_index);

	[[nodiscard]] std::string Describe();
}
