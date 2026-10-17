#pragma once

#include "game/OpenMenus.h"

#include <array>
#include <string_view>

// The developer's bare menu messages (the dev channel's `menu` command: a show or a hide message straight to the
// UI's queue, as the game's own code sends them). Some menus the game opens and closes only through a state of its
// own, and a bare message tears the menu down without it: a dialogue menu hidden by message crashed the game in the
// menu's destructor, and the workshop menu comes and goes with workshop mode (Workshop::StartWorkshop and
// RequestExitWorkshop, which end the mode before the menu goes). That menu's destructor is the one that froze a host
// scrapping in Sanctuary, waiting on the renderer's lock when the engine destroyed it mid-draw: nothing of the
// plugin's opens or closes it but the workshop's own enter and exit, so the plugin never decides when it is
// destroyed. Pure, so it is unit tested.
namespace MenuMessages
{
	struct Rule
	{
		std::string_view menu;
		bool             showToo;  // the show message is refused as well as the hide
		const char*      reason;   // what to do instead
	};

	constexpr std::array<Rule, 2> REFUSED{ {
		{ "DialogueMenu", false, "a dialogue is closed by the conversation ending, not by hiding its menu (that crashed the game in the menu's destructor)" },
		{ "WorkshopMenu", true, "workshop mode is entered and left by the workshop itself (workshop enter, workshop exit), never by its menu's message (the menu torn down outside them is the one that froze a host)" },
	} };

	// Why the message may not go to the menu, or null when it may. a_hiding: a hide (or force hide) message, else a
	// show. Case-blind, as the game's menu names are.
	constexpr const char* Refusal(std::string_view a_menu, bool a_hiding)
	{
		for (const auto& rule : REFUSED) {
			if (OpenMenus::SameName(a_menu, rule.menu) && (a_hiding || rule.showToo)) {
				return rule.reason;
			}
		}
		return nullptr;
	}
}
