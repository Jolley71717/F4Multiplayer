#include "Test.h"

#include "game/MenuMessages.h"

#include <string_view>

using MenuMessages::Refusal;

TEST("menu messages: the workshop menu takes neither a show nor a hide message, in any case of its name")
{
	for (const std::string_view name : { "WorkshopMenu", "workshopmenu", "WORKSHOPMENU" }) {
		REQUIRE(Refusal(name, true) != nullptr);
		REQUIRE(Refusal(name, false) != nullptr);
		// The reason says what to use instead.
		CHECK(std::string_view{ Refusal(name, true) }.find("workshop exit") != std::string_view::npos);
		CHECK(std::string_view{ Refusal(name, false) }.find("workshop enter") != std::string_view::npos);
	}
}

TEST("menu messages: the dialogue menu is not hidden by message, but may be shown, as before")
{
	REQUIRE(Refusal("DialogueMenu", true) != nullptr);
	CHECK(std::string_view{ Refusal("DialogueMenu", true) }.find("conversation ending") != std::string_view::npos);
	CHECK(Refusal("dialoguemenu", true) != nullptr);
	CHECK(Refusal("DialogueMenu", false) == nullptr);
}

TEST("menu messages: every other menu takes both, and a name that only begins like a refused one is another menu")
{
	for (const std::string_view name : { "PipboyMenu", "PauseMenu", "Console", "HUDMenu", "WorkshopMenu2", "Workshop", "DialogueMenuX", "" }) {
		CHECK(Refusal(name, true) == nullptr);
		CHECK(Refusal(name, false) == nullptr);
	}
}

TEST("menu messages: the refused menus are ones the open-menu book knows, by the game's own names")
{
	// A rule named with a typo would refuse nothing: each refused name is one the game sends in its menu events.
	for (const auto& rule : MenuMessages::REFUSED) {
		bool known = false;
		for (const auto name : OpenMenus::NAMES) {
			known = known || OpenMenus::SameName(name, rule.menu);
		}
		CHECK(known);
	}
}
