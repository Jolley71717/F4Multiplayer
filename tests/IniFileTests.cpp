#include "Test.h"

#include "IniFile.h"
#include "SourceTree.h"

#include <string>
#include <vector>

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

// An update brings new settings: the player's file gets them at the end, each with its explanation from the
// defaults, and nothing else of it changes (its values, its comments, its own old keys, its line ending).
namespace
{
	const std::string DEFAULTS =
		"# F4Multiplayer settings\n"
		"\n"
		"# Your name.\n"
		"sPlayerName = character\n"
		"\n"
		"# --- Hosting ---\n"
		"# One player hosts.\n"
		"bHost = false\n"
		"# Friendly fire.\n"
		"bFriendlyFire = false\n"
		"\n"
		"# Revives per fight, and the window.\n"
		"iMaxRevives = 0\n"
		"iReviveWindowMinutes = 0\n";
}

TEST("IniFile: adds the keys the defaults have and the file lacks, with their comments, and changes nothing else")
{
	const std::string mine = "# my settings\r\nbHost = true\r\n# mine\r\nbFriendlyFire=true\r\nbOldKey = 1\r\n";
	const auto        merged = IniFile::AddMissing(mine, DEFAULTS, "# added");
	CHECK((merged.added == std::vector<std::string>{ "sPlayerName", "iMaxRevives", "iReviveWindowMinutes" }));
	CHECK(merged.text == mine + "\r\n# added\r\n\r\n# Your name.\r\nsPlayerName = character\r\n\r\n# Revives per fight, and the window.\r\niMaxRevives = 0\r\niReviveWindowMinutes = 0\r\n");
	// Without a note: the groups alone.
	CHECK(IniFile::AddMissing("bHost = true\n", "# a\nbHost = false\n# b\nbNew = 1\n").text == "bHost = true\n\n# b\nbNew = 1\n");
}

TEST("IniFile: a file that lacks nothing is left as it was, whatever its values")
{
	const std::string mine = "sPlayerName = Sam\nbHost = true\nbFriendlyFire = true\niMaxRevives = 2\niReviveWindowMinutes = 10";  // no final line break
	const auto        merged = IniFile::AddMissing(mine, DEFAULTS, "# added");
	CHECK(merged.added.empty());
	CHECK(merged.text == mine);
}

TEST("IniFile: the comment block above a run of keys serves each key of the run")
{
	// The second key of the run alone: the block comes with it.
	auto merged = IniFile::AddMissing("sPlayerName = x\nbHost = 1\nbFriendlyFire = 0\niMaxRevives = 3\n", DEFAULTS);
	CHECK(merged.added == std::vector<std::string>{ "iReviveWindowMinutes" });
	CHECK(merged.text == "sPlayerName = x\nbHost = 1\nbFriendlyFire = 0\niMaxRevives = 3\n\n# Revives per fight, and the window.\niReviveWindowMinutes = 0\n");
	// The first alone, likewise.
	merged = IniFile::AddMissing("sPlayerName = x\nbHost = 1\nbFriendlyFire = 0\niReviveWindowMinutes = 3\n", DEFAULTS);
	CHECK(merged.added == std::vector<std::string>{ "iMaxRevives" });
	CHECK(merged.text == "sPlayerName = x\nbHost = 1\nbFriendlyFire = 0\niReviveWindowMinutes = 3\n\n# Revives per fight, and the window.\niMaxRevives = 0\n");
	// A comment between two keys belongs to the second: bFriendlyFire comes with "Friendly fire", not the hosting banner.
	merged = IniFile::AddMissing("sPlayerName = x\nbHost = 1\niMaxRevives = 0\niReviveWindowMinutes = 0\n", DEFAULTS);
	CHECK(merged.added == std::vector<std::string>{ "bFriendlyFire" });
	CHECK(merged.text == "sPlayerName = x\nbHost = 1\niMaxRevives = 0\niReviveWindowMinutes = 0\n\n# Friendly fire.\nbFriendlyFire = false\n");
}

TEST("IniFile: a commented-out key is a key the file lacks, and the key is matched exactly")
{
	const auto merged = IniFile::AddMissing("# bHost = true\n; bFriendlyFire = true\nbhost = true\n", "bHost = false\nbFriendlyFire = false\n");
	CHECK((merged.added == std::vector<std::string>{ "bHost", "bFriendlyFire" }));
	CHECK(merged.text == "# bHost = true\n; bFriendlyFire = true\nbhost = true\n\nbHost = false\nbFriendlyFire = false\n");
}

TEST("IniFile: an empty file, one without a final line break, and byte order marks")
{
	CHECK(IniFile::AddMissing("", "# a\nbHost = false\n", "# note").text == "# note\n\n# a\nbHost = false\n");
	CHECK(IniFile::AddMissing("", "# a\nbHost = false\n").text == "# a\nbHost = false\n");
	CHECK(IniFile::AddMissing("bHost = true", "bHost = false\nbNew = 1\n").text == "bHost = true\n\nbNew = 1\n");
	// The defaults' CRLF never reaches a LF file, and the file's own ending is kept.
	CHECK(IniFile::AddMissing("bHost = true\n", "bHost = false\r\n# n\r\nbNew = 1\r\n").text == "bHost = true\n\n# n\nbNew = 1\n");
	// A mark at the top of either is ignored: the key under it is seen, and none is appended.
	CHECK(IniFile::AddMissing("\xEF\xBB\xBF" "bHost = true\n", "bHost = false\n").added.empty());
	CHECK(IniFile::AddMissing("", "\xEF\xBB\xBF" "bHost = false\n").text == "bHost = false\n");
	CHECK(IniFile::AddMissing("\xEF\xBB\xBF" "# mine\n", "\xEF\xBB\xBF" "bHost = false\n").text == "\xEF\xBB\xBF" "# mine\n\nbHost = false\n");
}

// The defaults the plugin carries (dist/F4Multiplayer.ini, compiled into it): merged into themselves they add nothing;
// into an empty file, every key once, and the plugin reads each of them (src/Config.cpp).
TEST("IniFile: dist/F4Multiplayer.ini merges into itself unchanged and into an empty file once per key")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto defaults = SourceTree::Read(*root / "dist" / "F4Multiplayer.ini");
	REQUIRE(!defaults.empty());
	const auto same = IniFile::AddMissing(defaults, defaults, "# note");
	CHECK(same.added.empty());
	CHECK(same.text == defaults);

	const auto fresh = IniFile::AddMissing("", defaults);
	CHECK(fresh.added.size() > 50);
	const auto config = SourceTree::Read(*root / "src" / "Config.cpp");
	for (const auto& key : fresh.added) {
		std::size_t lines = 0;
		for (std::size_t start = 0; start < defaults.size();) {
			const auto newline = defaults.find('\n', start);
			const auto end = newline == std::string::npos ? defaults.size() : newline;
			lines += IniFile::detail::KeyOf(std::string_view{ defaults }.substr(start, end - start)) == key;
			start = newline == std::string::npos ? defaults.size() : newline + 1;
		}
		CHECK(lines == 1);  // the file has the key once
		CHECK(config.find("\"" + key + "\"") != std::string::npos);  // and Config::Load reads it
	}
	// What a new install gets, read again, lacks nothing.
	CHECK(IniFile::AddMissing(fresh.text, defaults).added.empty());
}
