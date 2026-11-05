#include "Test.h"

#include "game/UiPasswordEntry.h"

TEST("password entry: letters, digits and punctuation go in, and it stops at the most the server takes")
{
	std::string typed;
	CHECK(UiPasswordEntry::Add(typed, 'a', 3));
	CHECK(UiPasswordEntry::Add(typed, '7', 3));
	CHECK(UiPasswordEntry::Add(typed, '#', 3));
	CHECK(!UiPasswordEntry::Add(typed, 'b', 3));
	CHECK(typed == "a7#");
}

TEST("password entry: what the ini or the menu's text would change doesn't go in")
{
	std::string typed;
	for (const std::uint32_t character : { 0x00u, 0x09u, 0x0Du, 0x20u, 0x7Fu, 0xE9u, 0x416u, static_cast<std::uint32_t>('<'), static_cast<std::uint32_t>('>') }) {
		CHECK(!UiPasswordEntry::Add(typed, character, 64));
	}
	CHECK(typed.empty());
}
