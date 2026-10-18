#include "Test.h"

#include "IniFile.h"

#include <string>

// The settings file after the in-game menu changes a key: only that line changes.
TEST("IniFile: replaces the key's line and keeps everything else")
{
	const std::string before = "# F4Multiplayer settings\r\nbHost = false\r\n# players can hurt each other\r\nbFriendlyFire = false\r\n";
	const auto        after = IniFile::SetValue(before, "bFriendlyFire", "true");
	CHECK(after == "# F4Multiplayer settings\r\nbHost = false\r\n# players can hurt each other\r\nbFriendlyFire = true\r\n");
}

TEST("IniFile: a key the file lacks is appended with the file's line ending")
{
	CHECK(IniFile::SetValue("bHost = false\r\n", "iKeyMenu", "70") == "bHost = false\r\niKeyMenu = 70\r\n");
	CHECK(IniFile::SetValue("bHost = false\n", "iKeyMenu", "70") == "bHost = false\niKeyMenu = 70\n");
	// No final newline: one is added before the new line.
	CHECK(IniFile::SetValue("bHost = false", "iKeyMenu", "70") == "bHost = false\niKeyMenu = 70\n");
	CHECK(IniFile::SetValue("", "iKeyMenu", "70") == "iKeyMenu = 70\n");
}

TEST("IniFile: keys match exactly, not as a prefix, and only the first match changes")
{
	const std::string before = "bVoiceChat = true\nbVoiceOpenMic = false\n  bVoiceChat=false\n";
	CHECK(IniFile::SetValue(before, "bVoiceChat", "false") == "bVoiceChat = false\nbVoiceOpenMic = false\n  bVoiceChat=false\n");
	CHECK(IniFile::SetValue(before, "bVoice", "x") == before + "bVoice = x\n");
}

TEST("IniFile: a commented-out key is not the key")
{
	const std::string before = "# bHost = true\n; bHost = true\n[Section]\n";
	CHECK(IniFile::SetValue(before, "bHost", "true") == before + "bHost = true\n");
}

TEST("IniFile: the last line without a newline can be replaced")
{
	CHECK(IniFile::SetValue("bHost = false\nbFastLoading = true", "bFastLoading", "false") == "bHost = false\nbFastLoading = false");
}

TEST("IniFile: reads a key's value, for the game's own ini files without regard to case")
{
	const std::string game = "\xEF\xBB\xBF[General]\r\nsLanguage=EN\r\n; sLanguage=fr\r\nuGridsToLoad = 5\r\n";
	CHECK(IniFile::GetValue(game, "sLanguage", true) == "EN");
	CHECK(IniFile::GetValue(game, "slanguage", true) == "EN");
	CHECK(!IniFile::GetValue(game, "slanguage"));  // the plugin's own keys match exactly
	CHECK(IniFile::GetValue(game, "uGridsToLoad") == "5");
	CHECK(!IniFile::GetValue(game, "uGrids"));  // not as a prefix
	CHECK(!IniFile::GetValue("", "sLanguage"));
	CHECK(IniFile::GetValue("sLanguage =\n", "sLanguage") == "");
	CHECK(IniFile::GetValue("a = 1\nsLanguage = ru\nsLanguage = de\n", "sLanguage") == "ru");  // the first, as SetValue changes
	CHECK(IniFile::GetValue("sName = a = b", "sName") == "a = b");
	CHECK(IniFile::GetValue("sLanguage=ja", "sLanguage") == "ja");  // no final line break
}
