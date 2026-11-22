#include "Test.h"

#include "game/UiKeyNames.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>

TEST("key names: letters, digits, function keys and the mouse's side buttons read as players know them")
{
	CHECK(UiKeyNames::Name('B') == "B");
	CHECK(UiKeyNames::Name('7') == "7");
	CHECK(UiKeyNames::Name(0x70) == "F1");
	CHECK(UiKeyNames::Name(0x7B) == "F12");
	CHECK(UiKeyNames::Name(0x05) == "key.mouse4");
	CHECK(UiKeyNames::Name(0xDC) == "\\");
	CHECK(UiKeyNames::Name(0xFE) == "key.code");  // a key with no name: its code, in each language's words
}

TEST("key names: quicksave, quickload, the menu's own keys and chord keys can't be bound")
{
	for (const std::uint32_t key : { 0x1Bu, 0x09u, 0x0Du, 0x74u, 0x78u, 0x11u, 0x12u, 0x5Bu, 0xA4u, 0x01u }) {
		CHECK(UiKeyNames::Reserved(key));
	}
	for (const std::uint32_t key : { 0x70u, 0x71u, 0x79u, 0x05u, static_cast<std::uint32_t>('B'), 0x14u }) {
		CHECK(!UiKeyNames::Reserved(key));
	}
}

TEST("key names: the mouse's clicks aren't taken, its other buttons are")
{
	CHECK(UiKeyNames::FromMouse(0) == 0);
	CHECK(UiKeyNames::FromMouse(1) == 0);
	CHECK(UiKeyNames::FromMouse(3) == 0x05);
	CHECK(UiKeyNames::FromMouse(4) == 0x06);
}

TEST("key names: worded names are language ids, everything else reads as it is")
{
	CHECK(UiKeyNames::IsId(UiKeyNames::Name(0x20)));
	CHECK(UiKeyNames::IsId(UiKeyNames::Name(0x1B)));
	CHECK(!UiKeyNames::IsId(UiKeyNames::Name('B')));
	CHECK(!UiKeyNames::IsId(UiKeyNames::Name(0x7B)));
	CHECK(!UiKeyNames::IsId(UiKeyNames::Name(0xBC)));
	CHECK(UiKeyNames::IsId(UiKeyNames::Name(0xFE)));  // a key with no name: "Key FE" in each language's words (key.code)
	CHECK(UiKeyNames::Name(0x62) == "key.num");
	CHECK(UiKeyNames::Number(0x62) == 2);
	CHECK(UiKeyNames::Number('B') == -1);
}

TEST("key names: shown in English by default, and in the language when it has the word")
{
	Text::Use("en", {});
	CHECK(UiKeyNames::Display(0x20) == "Space");
	CHECK(UiKeyNames::Display(0xA0) == "Left Shift");
	CHECK(UiKeyNames::Display(0x1B) == "Esc");
	CHECK(UiKeyNames::Display(0x67) == "Num 7");
	CHECK(UiKeyNames::Display('B') == "B");
	CHECK(UiKeyNames::Display(0xFE) == "Key FE");
	Text::Use("de", { { "key.space", "Leertaste" }, { "key.num", "Num {n}" } });
	CHECK(UiKeyNames::Display(0x20) == "Leertaste");
	CHECK(UiKeyNames::Display(0x63) == "Num 3");
	CHECK(UiKeyNames::Display(0x1B) == "Esc");  // a word the language lacks reads in English
	Text::Use("en", {});
}

TEST("key names: every id a key can be named by has an English text")
{
	std::set<std::string> ids;
	for (std::uint32_t vk = 0; vk < 0x100; ++vk) {
		if (const auto name = UiKeyNames::Name(vk); UiKeyNames::IsId(name)) {
			ids.insert(name);
		}
	}
	CHECK(ids.size() == 24);
	for (const auto& id : ids) {
		const bool known = std::ranges::any_of(Text::English(), [&](const auto& a_entry) { return a_entry.key == id; });
		if (!known) {
			Test::Fail(__FILE__, __LINE__, std::format("UiKeyNames names a key '{}' that en.txt lacks", id));
		}
	}
}

TEST("key names: a HUD text naming a bound key names a rebound letter or mouse button as the Controls page does")
{
	Text::Use("en", {});
	CHECK(Text::Format("party.teleport_choose", { { "key", UiKeyNames::Display('T') } }) == "Tap T to choose who to teleport to");
	CHECK(Text::Format("mods.differ", { { "n", 2 }, { "key", UiKeyNames::Display(0x05) } }) == "Multiplayer: your mods differ from the host's (2): see Mouse 4");
	CHECK(Text::Format("down.went_down", { { "key", UiKeyNames::Display(0x76) } }) == "You're down! A friend can help you up. Hold F7 to give up");
}

TEST("key names: a key the game has a control on is known as that control's, so the Controls page can warn a bind there does both")
{
	// The game's control map for the keyboard, as the page reads it: W walks, E activates, and a control with no key.
	const std::array<UiKeyNames::GameControl, 4> keyboard{ {
		{ "Forward", 'W' },
		{ "Activate", 'E' },
		{ "Unbound", UiKeyNames::NO_KEY },
		{ "SecondaryAttack", 'E' },
	} };
	CHECK(UiKeyNames::GameControlOn('E', keyboard) == "Activate");  // the first control on the key
	CHECK(UiKeyNames::GameControlOn('W', keyboard) == "Forward");
	CHECK(UiKeyNames::GameControlOn(0x71, keyboard).empty());  // F2: the game has nothing there
	// A control without a key is on no key, whatever code is asked about.
	CHECK(UiKeyNames::GameControlOn(UiKeyNames::NO_KEY, keyboard).empty());
	CHECK(UiKeyNames::GameControlOn('E', {}).empty());  // no control map to read
	// The mouse has its own map and its own codes: the game's button 3 (Mouse 4) only counts there.
	const std::array<UiKeyNames::GameControl, 2> mouse{ { { "PrimaryAttack", 0 }, { "Melee", 3 } } };
	CHECK(UiKeyNames::GameControlOn(3, mouse) == "Melee");
	CHECK(UiKeyNames::FromMouse(3) == 0x05);
	CHECK(UiKeyNames::GameControlOn(3, keyboard).empty());
	CHECK(UiKeyNames::GameControlOn(4, mouse).empty());
}
