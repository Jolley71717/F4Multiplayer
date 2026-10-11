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
