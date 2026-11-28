#pragma once

#include <string>
#include <string_view>

// The friend bot (FriendBotPlan.h says what it does and decides): dev channel only (bDevChannel), off until the dev
// command `friendbot on` turns it on, never a player's feature. Main thread only.
namespace FriendBot
{
	// friendbot ... (FriendBotPlan::USAGE).
	std::string Command(std::string_view a_args);

	// Each frame, after the dev commands' frame.
	void Frame();

	// Before every save and load (main.cpp's steps): the player is let go (AI-driven off, the made package dropped, the
	// pad in the middle), so no save holds an AI-driven player or a package made at run time. The bot stays on and picks
	// up again once the game is back.
	void BeforeSaveOrLoad();
}
