#include "Test.h"

#include "ConfigRead.h"
#include "IniFile.h"
#include "SourceTree.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The settings file's text read as the plugin reads it (Config::Read is what Config::Load runs), and the rule an
// update of the file rests on: a key the plugin appends to a player's file must read exactly as the missing key did.
namespace
{
	struct Said
	{
		std::vector<std::string> warnings;
		std::vector<std::string> notes;
	};

	Config::Settings ReadText(std::string_view a_text, Said* a_said = nullptr)
	{
		Config::Settings settings;
		Config::Read(a_text, settings, [&](bool a_warn, const std::string& a_line) {
			if (a_said) {
				(a_warn ? a_said->warnings : a_said->notes).push_back(a_line);
			}
		});
		return settings;
	}

	bool ReadsUnlikeMissing(std::string_view a_key)
	{
		return std::ranges::any_of(Config::WHEN_ABSENT, [&](const auto& a_entry) { return a_entry.first == a_key; });
	}
}

TEST("Config: every key an update appends to a player's file reads as the missing key did")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto defaults = SourceTree::Read(*root / "dist" / "F4Multiplayer.ini");
	REQUIRE(!defaults.empty());
	const Config::Settings missing;  // a file with no key at all

	// What an update appends to a file that has none of the keys: every key of the defaults, in the form it appends.
	const auto appended = IniFile::AddMissing("# mine\n", defaults, "# note", Config::WHEN_ABSENT);
	REQUIRE(appended.added.size() > 50);
	Said all;
	CHECK(ReadText(appended.text, &all) == missing);
	CHECK(all.warnings.empty());
	CHECK(all.notes.empty());

	// Key by key, so a failure names its key: the appended line alone changes nothing and draws no complaint.
	for (const auto& key : appended.added) {
		const auto value = IniFile::GetValue(appended.text, key);
		REQUIRE(value);
		Said       said;
		const auto read = ReadText(key + " = " + *value + "\n", &said);
		if (!(read == missing) || !said.warnings.empty() || !said.notes.empty()) {
			Test::Fail(__FILE__, __LINE__, key + " = " + *value + ", appended to a file without the key, does not read as the missing key did (Config::WHEN_ABSENT)");
		}
	}

	// The list itself: a key is on it exactly when the defaults' own line reads unlike the missing key, so a key
	// that needs to be there can't be forgotten and one that no longer does can't linger. A new install gets the
	// defaults' lines as they are.
	const auto fresh = IniFile::AddMissing("", defaults);
	CHECK(fresh.added == appended.added);
	for (const auto& key : fresh.added) {
		const auto value = IniFile::GetValue(defaults, key);
		REQUIRE(value);
		const bool unlike = !(ReadText(key + " = " + *value + "\n") == missing);
		if (unlike != ReadsUnlikeMissing(key)) {
			Test::Fail(__FILE__, __LINE__, key + " = " + *value + (unlike ? " reads unlike the missing key and is not in Config::WHEN_ABSENT" : " reads as the missing key and is in Config::WHEN_ABSENT"));
		}
	}
	for (const auto& [key, value] : Config::WHEN_ABSENT) {
		CHECK(std::ranges::find(fresh.added, std::string{ key }) != fresh.added.end());  // a key of the defaults
		CHECK(IniFile::GetValue(appended.text, key) == value);
	}

	// The one there is: a missing sPlayerName is the Steam name, which is what empty says; "character" is the new install's.
	CHECK(ReadsUnlikeMissing("sPlayerName"));
	CHECK(missing.playerName.empty());
	CHECK(ReadText("sPlayerName =\n").playerName.empty());
	CHECK(ReadText("sPlayerName = character\n").playerName == "character");
	CHECK(IniFile::GetValue(appended.text, "sPlayerName") == "");
	CHECK(IniFile::GetValue(defaults, "sPlayerName") == "character");
}

TEST("Config: the settings file is read line by line, the last line of a key counting")
{
	Said             said;
	const auto       read = ReadText("\xEF\xBB\xBF# mine\r\n  bHost = true  \r\n; bFriendlyFire = true\r\n[General]\r\niPort=7000\r\nsPlayerName = Sam Smith\r\niKeyMenu = 0x71\r\nfXpShare = 2\r\nno equals sign\r\n", &said);
	Config::Settings expected;
	expected.host = true;
	expected.port = 7000;
	expected.playerName = "Sam Smith";
	expected.keyMenu = 0x71;
	expected.xpShare = 1.0f;  // clamped
	CHECK(read == expected);
	CHECK(said.warnings.empty());

	// A value that can't be used leaves the setting as it was; an unknown key is said, an old one noted.
	said = {};
	const auto odd = ReadText("iPort = many\nsStory = theirs\nbNoSuchKey = 1\nbVoiceLines = true\niCatchUpXp = 900\n", &said);
	CHECK(odd.port == Config::Settings{}.port);
	CHECK(odd.sharedStory == Config::Settings{}.sharedStory);
	CHECK(odd.catchUpXp == Protocol::SessionRules::MAX_CATCH_UP_XP);
	CHECK(said.warnings.size() == 3);  // sStory, the unknown key, the percent past its ceiling
	CHECK(said.notes.size() == 1);

	// The last line of a key counts.
	CHECK(ReadText("bHost = true\nbHost = false\n").host == false);
	CHECK(ReadText("bHost = false\nbHost = true\n").host == true);
	CHECK(ReadText("sIgnoredPlugins = a.esp, b.esp\nsIgnoredPlugins =\n").ignoredPlugins.empty());
	CHECK((ReadText("sIgnoredPlugins = a.esp , b.esp\n").ignoredPlugins == std::vector<std::string>{ "a.esp", "b.esp" }));
}

// The whole of a cut-off append (tests/IniFileTests.cpp): the half line alone would read as false for ever; the next
// start appends the key again, whole, and the plugin reads that one; and the in-game menu's save then changes both
// lines, so its value holds.
TEST("Config: a cut-off line's key reads at its default once repaired, and the menu's save still holds")
{
	const std::string defaults = "bFriendlyFire = false\n# Shared building.\nbSharedWorkshop = true\n";
	const std::string torn = "bFriendlyFire = true\n\n# Shared building.\nbSharedWorkshop = tr";
	CHECK(ReadText(torn).sharedWorkshop == false);  // what was wrong
	const auto repaired = IniFile::AddMissing(torn, defaults, "# note");
	CHECK(repaired.added == std::vector<std::string>{ "bSharedWorkshop" });
	CHECK(repaired.text.starts_with(torn + "\n"));
	const auto read = ReadText(repaired.text);
	CHECK(read.sharedWorkshop == true);
	CHECK(read.friendlyFire == true);  // the player's own, kept
	CHECK(ReadText(IniFile::SetValue(repaired.text, "bSharedWorkshop", "false")).sharedWorkshop == false);
	CHECK(ReadText(IniFile::SetValue(IniFile::SetValue(repaired.text, "bSharedWorkshop", "false"), "bSharedWorkshop", "true")).sharedWorkshop == true);
}
