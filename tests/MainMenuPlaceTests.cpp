#include "Test.h"

#include "game/MainMenuPlace.h"

#include <array>

using MainMenuPlace::Entry;
using MainMenuPlace::ENTRY_INDEX;
using MainMenuPlace::InsertAt;

TEST("main menu entry: goes straight after Load in the main menu")
{
	constexpr std::array entries{ Entry{ "$CONTINUE", 0 }, Entry{ "$NEW", 1 }, Entry{ "$LOAD", 4 }, Entry{ "$SETTINGS", 8 }, Entry{ "$QUIT", 10 } };
	CHECK(InsertAt(entries) == 3u);
}

TEST("main menu entry: goes after Load in the pause menu, which has Quicksave and Save before it")
{
	constexpr std::array entries{ Entry{ "$QUICKSAVE", 2 }, Entry{ "$SAVE", 3 }, Entry{ "$LOAD", 4 }, Entry{ "$SETTINGS", 8 } };
	CHECK(InsertAt(entries) == 3u);
}

TEST("main menu entry: Load last in the list puts ours at the end")
{
	constexpr std::array entries{ Entry{ "$NEW", 1 }, Entry{ "$LOAD", 4 } };
	CHECK(InsertAt(entries) == 2u);
}

TEST("main menu entry: is not added twice")
{
	constexpr std::array entries{ Entry{ "$LOAD", 4 }, Entry{ "MULTIPLAYER", ENTRY_INDEX }, Entry{ "$SETTINGS", 8 } };
	CHECK(!InsertAt(entries));
}

TEST("main menu entry: a list without Load gets none")
{
	constexpr std::array entries{ Entry{ "$NEW", 1 }, Entry{ "$SETTINGS", 8 } };
	CHECK(!InsertAt(entries));
	CHECK(!InsertAt({}));
}

TEST("main menu entry: Load is found whatever its case")
{
	constexpr std::array entries{ Entry{ "$Load", 4 }, Entry{ "$SETTINGS", 8 } };
	CHECK(InsertAt(entries) == 1u);
}
