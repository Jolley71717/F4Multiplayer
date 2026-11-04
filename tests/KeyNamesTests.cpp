#include "Test.h"

#include "game/KeyNames.h"

TEST("key names: letters, digits, function keys and the mouse's side buttons read as players know them")
{
	CHECK(KeyNames::Name('B') == "B");
	CHECK(KeyNames::Name('7') == "7");
	CHECK(KeyNames::Name(0x70) == "F1");
	CHECK(KeyNames::Name(0x7B) == "F12");
	CHECK(KeyNames::Name(0x05) == "Mouse 4");
	CHECK(KeyNames::Name(0xDC) == "\\");
	CHECK(KeyNames::Name(0xFE) == "Key FE");
}

TEST("key names: quicksave, quickload, the menu's own keys and chord keys can't be bound")
{
	for (const std::uint32_t key : { 0x1Bu, 0x09u, 0x0Du, 0x74u, 0x78u, 0x11u, 0x12u, 0x5Bu, 0xA4u, 0x01u }) {
		CHECK(KeyNames::Reserved(key));
	}
	for (const std::uint32_t key : { 0x70u, 0x71u, 0x79u, 0x05u, static_cast<std::uint32_t>('B'), 0x14u }) {
		CHECK(!KeyNames::Reserved(key));
	}
}

TEST("key names: the mouse's clicks aren't taken, its other buttons are")
{
	CHECK(KeyNames::FromMouse(0) == 0);
	CHECK(KeyNames::FromMouse(1) == 0);
	CHECK(KeyNames::FromMouse(3) == 0x05);
	CHECK(KeyNames::FromMouse(4) == 0x06);
}
