#include "Test.h"

#include "net/NameChoice.h"

// The name a game says hello with: sPlayerName, the character's name, or the Steam name (net/NameChoice.h).

TEST("name: 'character' sends the name of the character played")
{
	CHECK(NameChoice::Choose("character", "Nate", "SteamUser") == "Nate");
	CHECK(NameChoice::Choose("Character", "Nora", "SteamUser") == "Nora");
	CHECK(NameChoice::Choose("  CHARACTER  ", "Nora", "SteamUser") == "Nora");
	CHECK(NameChoice::Choose("character", "  Nate Jr  ", "SteamUser") == "Nate Jr");
	CHECK(NameChoice::Choose("character", "Nate", "") == "Nate");  // no Steam needed
}

TEST("name: 'character' falls back to Steam, then to Vault Dweller")
{
	// No player yet, or one nobody has named (a new character before the opening's name screen).
	CHECK(NameChoice::Choose("character", "", "SteamUser") == "SteamUser");
	CHECK(NameChoice::Choose("character", "   ", "SteamUser") == "SteamUser");
	CHECK(NameChoice::Choose("character", "", "") == "Vault Dweller");
	CHECK(NameChoice::Choose("character", "  ", "  ") == "Vault Dweller");
	// A name the server would strip to nothing (control characters, markup) is no name.
	CHECK(NameChoice::Choose("character", "\x01\x02", "SteamUser") == "SteamUser");
	CHECK(NameChoice::Choose("character", "<>", "") == "Vault Dweller");
}

TEST("name: a character named in another script is sent under that name")
{
	// The server keeps the letters of every script (common/Name.h), so a Cyrillic, CJK or mixed name is the name.
	CHECK(NameChoice::Choose("character", "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82", "SteamUser") == "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82");  // Нейт
	CHECK(NameChoice::Choose("character", "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82", "") == "\xD0\x9D\xD0\xB5\xD0\xB9\xD1\x82");
	CHECK(NameChoice::Choose("character", "\xE5\x86\x85\xE7\x89\xB9", "SteamUser") == "\xE5\x86\x85\xE7\x89\xB9");  // 内特
	CHECK(NameChoice::Choose("character", "Nate \xD0\x9D", "SteamUser") == "Nate \xD0\x9D");
	// Bytes that aren't UTF-8 are all the server would drop: no name.
	CHECK(NameChoice::Choose("character", "\xFF\xFE", "SteamUser") == "SteamUser");
}

TEST("name: a name in the ini is used as written, whoever the character is")
{
	CHECK(NameChoice::Choose("Sam", "Nate", "SteamUser") == "Sam");
	CHECK(NameChoice::Choose("  Sam  ", "", "") == "Sam");
	CHECK(NameChoice::Choose("Sam", "", "SteamUser") == "Sam");
	// 'character' inside a longer name is a name.
	CHECK(NameChoice::Choose("character one", "Nate", "SteamUser") == "character one");
	CHECK(NameChoice::Choose("my character", "Nate", "SteamUser") == "my character");
}

TEST("name: an empty sPlayerName (older ini files) still means the Steam name")
{
	CHECK(NameChoice::Choose("", "Nate", "SteamUser") == "SteamUser");  // the character is not used unless asked for
	CHECK(NameChoice::Choose("   ", "Nate", "SteamUser") == "SteamUser");
	CHECK(NameChoice::Choose("", "Nate", "") == "Vault Dweller");
	CHECK(NameChoice::Choose("", "", "") == "Vault Dweller");
}

TEST("name: only the word 'character' asks for the character's name")
{
	CHECK(NameChoice::WantsCharacter("character"));
	CHECK(NameChoice::WantsCharacter(" Character\t"));
	CHECK(!NameChoice::WantsCharacter(""));
	CHECK(!NameChoice::WantsCharacter("characters"));
	CHECK(!NameChoice::WantsCharacter("Sam"));
	CHECK(!NameChoice::WantsCharacter("steam"));
}
