#include "Test.h"

#include "BanList.h"
#include "IniFile.h"
#include "SourceTree.h"
#include "net/IdentityFile.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/locking.h>
#include <sys/stat.h>
#endif

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

// The plugin reads the last line of a key (Config::Read), so a save that changed only the first would not hold in
// a file that has the key twice.
TEST("IniFile: keys match exactly, not as a prefix, and every line of the key changes")
{
	const std::string before = "bVoiceChat = true\nbVoiceOpenMic = false\n  bVoiceChat=false\n";
	CHECK(IniFile::SetValue(before, "bVoiceChat", "true") == "bVoiceChat = true\nbVoiceOpenMic = false\nbVoiceChat = true\n");
	CHECK(IniFile::SetValue(before, "bVoiceChat", "false") == "bVoiceChat = false\nbVoiceOpenMic = false\nbVoiceChat = false\n");
	CHECK(IniFile::SetValue("bVoiceChat = true\r\nbVoiceChat = true", "bVoiceChat", "false") == "bVoiceChat = false\r\nbVoiceChat = false");
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
	CHECK(IniFile::GetValue("a = 1\nsLanguage = ru\nsLanguage = de\n", "sLanguage") == "ru");  // the first, as the game reads its own files
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
// into an empty file, every key once, and the plugin reads each of them (Config::Read, src/ConfigRead.h).
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
	const auto config = SourceTree::Read(*root / "src" / "ConfigRead.h");
	REQUIRE(!config.empty());
	for (const auto& key : fresh.added) {
		std::size_t lines = 0;
		for (std::size_t start = 0; start < defaults.size();) {
			const auto newline = defaults.find('\n', start);
			const auto end = newline == std::string::npos ? defaults.size() : newline;
			lines += IniFile::detail::KeyOf(std::string_view{ defaults }.substr(start, end - start)) == key;
			start = newline == std::string::npos ? defaults.size() : newline + 1;
		}
		CHECK(lines == 1);  // the file has the key once
		CHECK(config.find("\"" + key + "\"") != std::string::npos);  // and Config::Read reads it
	}
	// What a new install gets, read again, lacks nothing.
	CHECK(IniFile::AddMissing(fresh.text, defaults).added.empty());
}

namespace
{
	// A folder of this test's own, removed when the test ends.
	struct TempFolder
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / std::format("f4mp-ini-{:016X}", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());

		TempFolder() { std::filesystem::create_directories(path); }
		~TempFolder()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	std::string Slurp(const std::filesystem::path& a_path)
	{
		std::ifstream in{ a_path, std::ios::binary };
		return std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
	}
}

// What the plugin does with the settings file when the game starts (Config::Load): the zip ships none.
TEST("IniFile: a new install's settings file is written from the defaults, byte for byte")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string defaults = std::string{ "\xEF\xBB\xBF" } + "# Hosting.\r\nbHost = false\r\n";
	const auto        loaded = IniFile::LoadOrCreate(path, defaults, "# added");
	CHECK(loaded.what == IniFile::Kept::Created);
	CHECK(loaded.text == defaults);
	CHECK(loaded.added.empty());
	CHECK(Slurp(path) == defaults);
	// The next start finds it complete and leaves it alone.
	const auto again = IniFile::LoadOrCreate(path, defaults, "# added");
	CHECK(again.what == IniFile::Kept::Complete);
	CHECK(again.text == defaults);
	CHECK(Slurp(path) == defaults);
}

TEST("IniFile: an update's new settings are appended to the player's file, and every byte of theirs stays")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "# my notes\r\nbHost = true\r\n  sPlayerName=Sam  \r\nbGone = 1";  // odd spacing, an old key, no final line break
	REQUIRE(IniFile::WriteText(path, mine));
	const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
	CHECK(loaded.what == IniFile::Kept::Extended);
	CHECK((loaded.added == std::vector<std::string>{ "bFriendlyFire", "iMaxRevives", "iReviveWindowMinutes" }));
	const auto onDisk = Slurp(path);
	CHECK(onDisk == loaded.text);
	CHECK(onDisk.starts_with(mine));
	CHECK(onDisk == IniFile::AddMissing(mine, DEFAULTS, "# added").text);
	CHECK(IniFile::GetValue(onDisk, "bHost") == "true");
	CHECK(IniFile::GetValue(onDisk, "sPlayerName") == "Sam");
	CHECK(IniFile::GetValue(onDisk, "bFriendlyFire") == "false");
	// Started again, and again with the same version: nothing more is added.
	const auto again = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
	CHECK(again.what == IniFile::Kept::Complete);
	CHECK(Slurp(path) == onDisk);
}

TEST("IniFile: a settings file that lacks nothing is not written to")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "sPlayerName = Sam\nbHost = true\nbFriendlyFire = true\niMaxRevives = 2\niReviveWindowMinutes = 10\n";
	REQUIRE(IniFile::WriteText(path, mine));
	const auto before = std::filesystem::last_write_time(path);
	const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
	CHECK(loaded.what == IniFile::Kept::Complete);
	CHECK(loaded.text == mine);
	CHECK(Slurp(path) == mine);
	CHECK(std::filesystem::last_write_time(path) == before);
}

TEST("IniFile: a settings file that can't be written or read leaves the defaults in use and nothing behind")
{
	const TempFolder folder;
	// No folder to write it in (a broken install): the defaults are used.
	const auto missing = IniFile::LoadOrCreate(folder.path / "no-such-folder" / "F4Multiplayer.ini", DEFAULTS);
	CHECK(missing.what == IniFile::Kept::NotCreated);
	CHECK(missing.text == DEFAULTS);
	CHECK(!std::filesystem::exists(folder.path / "no-such-folder"));
	// Something else by that name (a folder): not read, not replaced.
	std::filesystem::create_directories(folder.path / "F4Multiplayer.ini");
	const auto odd = IniFile::LoadOrCreate(folder.path / "F4Multiplayer.ini", DEFAULTS);
	CHECK(odd.what == IniFile::Kept::Unreadable);
	CHECK(odd.text == DEFAULTS);
	CHECK(std::filesystem::is_directory(folder.path / "F4Multiplayer.ini"));
}

// sPlayerName in the plugin: the defaults say "character", a file without the key shows the Steam name, and an update
// must not turn the one into the other (Config::WHEN_ABSENT; tests/ConfigReadTests.cpp holds that list to every key).
TEST("IniFile: a key that reads unlike a missing key is appended with the value a missing key has")
{
	static constexpr std::pair<std::string_view, std::string_view> WHEN_ABSENT[] = { { "sPlayerName", "" }, { "iMaxRevives", "3" } };
	const auto merged = IniFile::AddMissing("bHost = true\n", DEFAULTS, {}, WHEN_ABSENT);
	CHECK((merged.added == std::vector<std::string>{ "sPlayerName", "bFriendlyFire", "iMaxRevives", "iReviveWindowMinutes" }));
	CHECK(merged.text == "bHost = true\n\n# Your name.\nsPlayerName =\n\n# Friendly fire.\nbFriendlyFire = false\n\n# Revives per fight, and the window.\niMaxRevives = 3\niReviveWindowMinutes = 0\n");
	// Once there the line is the player's: nothing is added again, whatever they make of it.
	CHECK(IniFile::AddMissing(merged.text, DEFAULTS, {}, WHEN_ABSENT).added.empty());
	CHECK(IniFile::AddMissing("sPlayerName = character\nbHost = true\nbFriendlyFire = true\niMaxRevives = 0\niReviveWindowMinutes = 0\n", DEFAULTS, {}, WHEN_ABSENT).added.empty());

	// A new install is another matter: it gets the defaults' own lines.
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer.ini";
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added", WHEN_ABSENT).what == IniFile::Kept::Created);
	CHECK(Slurp(path) == DEFAULTS);
	// And an update of a file without the key appends the other.
	REQUIRE(IniFile::WriteText(path, "bHost = true\n"));
	const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, {}, WHEN_ABSENT);
	CHECK(loaded.what == IniFile::Kept::Extended);
	CHECK(Slurp(path) == merged.text);
	CHECK(IniFile::GetValue(Slurp(path), "sPlayerName") == "");
}

// A write that stopped half way (the PC lost power as the update's settings were appended) leaves a last line with
// no line break. Its value, when it is only the beginning of the default's, would be read as it stands for ever
// ("tr" is not true): the key counts as lacking, and is appended again on a line of its own.
TEST("IniFile: a cut-off last line is repaired by appending its key again, whole")
{
	for (const std::string_view cut : { "bFriendlyFire = fal", "bFriendlyFire = f", "bFriendlyFire = ", "bFriendlyFire =", "bFriendlyFire=fals" }) {
		const std::string mine = "sPlayerName = Sam\nbHost = true\niMaxRevives = 2\niReviveWindowMinutes = 10\n\n# Friendly fire.\n" + std::string{ cut };
		const auto        merged = IniFile::AddMissing(mine, DEFAULTS, "# added");
		CHECK(merged.added == std::vector<std::string>{ "bFriendlyFire" });
		CHECK(merged.text == mine + "\n\n# added\n\n# Friendly fire.\nbFriendlyFire = false\n");
		// Started again, the file ends in a whole line: nothing more is added.
		CHECK(IniFile::AddMissing(merged.text, DEFAULTS, "# added").added.empty());
	}
	CHECK(IniFile::AddMissing("bOn = t", "bOn = true\n").text == "bOn = t\n\nbOn = true\n");
	CHECK(IniFile::AddMissing("\xEF\xBB\xBF" "bOn = tru", "bOn = true\n").added.size() == 1);  // a one-line file under a byte order mark
	CHECK(IniFile::AddMissing("iPort =", "iPort = 7779\n").text == "iPort =\n\niPort = 7779\n");  // nothing at all of a number
	CHECK(IniFile::AddMissing("fXpShare = ", "fXpShare = 1.0\n").added.size() == 1);
	// Nothing at all of a yes/no key may be the player's own line, which reads as no: no is what follows it, whatever the default.
	CHECK(IniFile::AddMissing("bOn =", "bOn = true\n").text == "bOn =\n\nbOn = false\n");
	CHECK(IniFile::AddMissing("bOn = ", "# On.\nbOn = true\n").text == "bOn = \n\n# On.\nbOn = false\n");
	// Text may be empty, and for some settings that is a choice (no password, the Steam name): the player's.
	CHECK(IniFile::AddMissing("sTransport = ", "sTransport = auto\n").added.empty());
	CHECK(IniFile::AddMissing("sTransport =", "sTransport = auto\n").added.empty());

	// A whole value is the player's, with or without a line break after it.
	CHECK(IniFile::AddMissing("bOn = true", "bOn = true\n").added.empty());
	CHECK(IniFile::AddMissing("bOn = false", "bOn = true\n").added.empty());
	CHECK(IniFile::AddMissing("bOn = f", "bOn = true\n").added.empty());  // not the beginning of the default
	CHECK(IniFile::AddMissing("bOn = 1", "bOn = true\n").added.empty());
	// So is the beginning of a number or a name: "77" may be the port the player chose, "char" their name.
	CHECK(IniFile::AddMissing("iPort = 77", "iPort = 7779\n").added.empty());
	CHECK(IniFile::AddMissing("sPlayerName = char", "sPlayerName = character\n").added.empty());
	// A line that ends in a line break was written whole: left alone, whatever it says.
	CHECK(IniFile::AddMissing("bOn = tr\n", "bOn = true\n").added.empty());
	CHECK(IniFile::AddMissing("bOn = tr\r\n", "bOn = true\n").added.empty());
	// A key the file also has on an earlier line is not lacking.
	CHECK(IniFile::AddMissing("bOn = false\nbOn = tr", "bOn = true\n").added.empty());
	// A key that is appended empty (it reads unlike a missing key) is whole when it is empty.
	static constexpr std::pair<std::string_view, std::string_view> WHEN_ABSENT[] = { { "sPlayerName", "" } };
	CHECK(IniFile::AddMissing("sPlayerName =", "sPlayerName = character\n", {}, WHEN_ABSENT).added.empty());
	// Only the last line is looked at, and only a key of the defaults.
	CHECK(IniFile::AddMissing("bOn = tr\nbOther = 1", "bOn = true\nbOther = 1\n").added.empty());
	CHECK(IniFile::AddMissing("bMine = tr", "bOn = true\n").text == "bMine = tr\n\nbOn = true\n");

	// On the file itself: the cut-off line stays where it is (nothing of the player's is rewritten) and the whole one follows.
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string torn = "sPlayerName = Sam\r\nbHost = true\r\n\r\n# added\r\n\r\n# Friendly fire.\r\nbFriendlyFire = fa";
	REQUIRE(IniFile::WriteText(path, torn));
	const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
	CHECK(loaded.what == IniFile::Kept::Extended);
	CHECK((loaded.added == std::vector<std::string>{ "bFriendlyFire", "iMaxRevives", "iReviveWindowMinutes" }));
	CHECK(Slurp(path).starts_with(torn + "\r\n"));
	CHECK(Slurp(path).ends_with("# Friendly fire.\r\nbFriendlyFire = false\r\n\r\n# Revives per fight, and the window.\r\niMaxRevives = 0\r\niReviveWindowMinutes = 0\r\n"));
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::Complete);
}

// Notepad's "UTF-16 LE" (older ones call it "Unicode"): the plugin reads none of such a file, and UTF-8 lines appended
// to it would make it neither. It is left as it is, byte for byte, and the defaults are the settings (the log says why).
TEST("IniFile: a settings file saved as UTF-16 is left alone, and the defaults are the settings")
{
	const auto utf16 = [](std::string_view a_text, bool a_bigEndian, bool a_mark) {
		std::string out;
		if (a_mark) {
			out += a_bigEndian ? "\xFE\xFF" : "\xFF\xFE";
		}
		for (const char letter : a_text) {
			if (a_bigEndian) {
				out += '\0';
			}
			out += letter;
			if (!a_bigEndian) {
				out += '\0';
			}
		}
		return out;
	};
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer.ini";
	for (const bool bigEndian : { false, true }) {
		for (const bool mark : { true, false }) {
			const auto mine = utf16("bHost = true\r\nsPlayerName = Sam\r\n", bigEndian, mark);
			REQUIRE(IniFile::WriteText(path, mine));
			CHECK(IniFile::IsUtf16(mine));
			const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
			CHECK(loaded.what == IniFile::Kept::Utf16);
			CHECK(loaded.text == DEFAULTS);
			CHECK(loaded.added.empty());
			CHECK(Slurp(path) == mine);
		}
	}
	// Only the mark and nothing else, as an emptied file has it.
	CHECK(IniFile::IsUtf16("\xFF\xFE"));
	// UTF-8, with or without its mark, and plain text are not.
	CHECK(!IniFile::IsUtf16(std::string{ "\xEF\xBB\xBF" } + "bHost = true\n"));
	CHECK(!IniFile::IsUtf16("bHost = true\n"));
	CHECK(!IniFile::IsUtf16("# \xD0\x98\xD0\xBC\xD1\x8F\n"));
	CHECK(!IniFile::IsUtf16("#"));
	CHECK(!IniFile::IsUtf16(""));
}

// A file the player (or a mod manager, or a synced folder) made read-only: the plugin can't add this version's keys
// to it. Nothing breaks: the file stays as it is, and the settings read are the player's plus the keys it lacks at
// their defaults (Config::Load logs one line naming them).
TEST("IniFile: a read-only settings file that lacks keys is left as it is, and the defaults apply to what it lacks")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "bHost = true\r\nsPlayerName = Sam\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	const auto writing = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
	std::filesystem::permissions(path, writing, std::filesystem::perm_options::remove);
	// An account that may write to any file (root, where a Linux build runs as it) can't make this case.
	const bool locked = !std::ofstream{ path, std::ios::binary | std::ios::app };
#ifdef _WIN32
	CHECK(locked);
#endif
	if (locked) {
		const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
		CHECK(loaded.what == IniFile::Kept::NotExtended);
		CHECK((loaded.added == std::vector<std::string>{ "bFriendlyFire", "iMaxRevives", "iReviveWindowMinutes" }));
		CHECK(loaded.text == IniFile::AddMissing(mine, DEFAULTS, "# added").text);
		CHECK(IniFile::GetValue(loaded.text, "bHost") == "true");
		CHECK(IniFile::GetValue(loaded.text, "sPlayerName") == "Sam");
		CHECK(IniFile::GetValue(loaded.text, "bFriendlyFire") == "false");
		CHECK(IniFile::GetValue(loaded.text, "iMaxRevives") == "0");
		CHECK(Slurp(path) == mine);
		// Every start says the same and does the same, until the file can be written.
		CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::NotExtended);
		CHECK(Slurp(path) == mine);
		// The menu's save fails too (Config::Write says so), with the file as it was.
		CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::NotWritten);
		CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
		CHECK(Slurp(path) == mine);
	}
	std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
	// Writable again, the next start adds them.
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::Extended);
	CHECK(Slurp(path) == IniFile::AddMissing(mine, DEFAULTS, "# added").text);
}

// A read-only file that lacks nothing is never opened for writing, so it is simply read.
TEST("IniFile: a read-only settings file that lacks nothing is read as any other")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "sPlayerName = Sam\nbHost = true\nbFriendlyFire = true\niMaxRevives = 2\niReviveWindowMinutes = 10\n";
	REQUIRE(IniFile::WriteText(path, mine));
	const auto writing = std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
	std::filesystem::permissions(path, writing, std::filesystem::perm_options::remove);
	const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
	CHECK(loaded.what == IniFile::Kept::Complete);
	CHECK(loaded.text == mine);
	CHECK(Slurp(path) == mine);
	std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
}

// The in-game menu's save (Config::Write): one line of the player's file changes, by way of a file beside it that
// takes its place, so no moment has half a file under the settings file's name.
TEST("IniFile: the menu's save changes one line, keeps every other byte and leaves no other file behind")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const auto        temp = folder.path / "F4Multiplayer.ini.tmp";
	const std::string head = "\xEF\xBB\xBF# my notes\r\nbHost = true\r\n  sPlayerName=Sam  \r\n; old\r\nbGone = 1\r\n\r\n";
	REQUIRE(IniFile::WriteText(path, head + "bFriendlyFire = false"));
	CHECK(IniFile::SaveValue(path, "bFriendlyFire", "true", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == head + "bFriendlyFire = true");
	CHECK(!std::filesystem::exists(temp));
	// A key the file lacks goes at its end; the defaults play no part in a file that is there.
	CHECK(IniFile::SaveValue(path, "iKeyMenu", "70", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == head + "bFriendlyFire = true\r\niKeyMenu = 70\r\n");
	// What an earlier save that never finished left beside it is not taken for anything, and is gone after the next.
	REQUIRE(IniFile::WriteText(temp, "bHost = fal"));
	CHECK(IniFile::LoadOrCreate(path, "bHost = false\n").what == IniFile::Kept::Complete);
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == "\xEF\xBB\xBF# my notes\r\nbHost = false\r\n  sPlayerName=Sam  \r\n; old\r\nbGone = 1\r\n\r\nbFriendlyFire = true\r\niKeyMenu = 70\r\n");
	CHECK(!std::filesystem::exists(temp));
	CHECK(std::distance(std::filesystem::directory_iterator{ folder.path }, std::filesystem::directory_iterator{}) == 1);
}

TEST("IniFile: the menu's save makes the settings file only when there is none, as a new install's with the value")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer.ini";
	CHECK(IniFile::SaveValue(path, "bHost", "true", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == IniFile::SetValue(DEFAULTS, "bHost", "true"));
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::Complete);
	// No folder to make it in: nothing is written, and nothing is left.
	CHECK(IniFile::SaveValue(folder.path / "no-such-folder" / "F4Multiplayer.ini", "bHost", "true", DEFAULTS) == IniFile::Saved::NotWritten);
	CHECK(!std::filesystem::exists(folder.path / "no-such-folder"));
}

// What is there by the file's name but can't be read is not a missing file: a save that took it for one would write
// back one line in place of every setting the player had.
TEST("IniFile: the menu's save does not write over what it could not read")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer.ini";
	// Something else by that name (a folder).
	std::filesystem::create_directories(path);
	CHECK(IniFile::ReadText(path).what == IniFile::Found::Unreadable);
	CHECK(IniFile::SaveValue(path, "bHost", "true", DEFAULTS) == IniFile::Saved::Unreadable);
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::Unreadable);
	CHECK(std::filesystem::is_directory(path));
	CHECK(std::filesystem::is_empty(path));
	CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
	// A file saved as UTF-16 is read, and not written to.
	const auto        other = folder.path / "utf16.ini";
	const std::string utf16{ "\xFF\xFE" "b\0H\0o\0s\0t\0 \0=\0 \0" "1\0\r\0\n\0", 24 };
	REQUIRE(IniFile::WriteText(other, utf16));
	CHECK(IniFile::SaveValue(other, "bHost", "true", DEFAULTS) == IniFile::Saved::Utf16);
	CHECK(Slurp(other) == utf16);
	// A file that is not there is told from both.
	CHECK(IniFile::ReadText(folder.path / "none.ini").what == IniFile::Found::Absent);
	const auto read = IniFile::ReadText(other);
	CHECK(read.what == IniFile::Found::Read);
	CHECK(read.text == utf16);
}

// A mod manager can put a mod's files in the game's folder as hard links to its own copies: a save that put a new
// file under the name would cut the link, and the manager's copy would keep the old settings.
TEST("IniFile: the menu's save writes a hard-linked settings file in place, so both names hold the new value")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const auto        staged = folder.path / "staged.ini";
	const std::string mine = "bHost = true\r\nsPlayerName = Sam\r\n";
	REQUIRE(IniFile::WriteText(staged, mine));
	std::error_code error;
	std::filesystem::create_hard_link(staged, path, error);
	if (!error) {  // a disk without hard links can't make this case
		CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
		CHECK(Slurp(path) == "bHost = false\r\nsPlayerName = Sam\r\n");
		CHECK(Slurp(staged) == Slurp(path));
		CHECK(std::filesystem::hard_link_count(path) == 2u);
		CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
	}
}

// The player id file (Identity::Get, src/net/IdentityFile.h): made once and read from then on. The identity is how a
// host's session knows the player again, so the file is never written over while it can't be read.
TEST("identity file: made when there is none, read from then on, and not written over when it can't be read")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer_player.id";
	std::uint64_t    asked = 0;
	const auto       random = [&] { return asked++ * 0x1111; };  // 0 the first time: asked again
	int              waits = 0;  // the moment a file that can't be read is given before the second try
	const auto       wait = [&] { ++waits; };
	auto             loaded = IdentityFile::LoadOrCreate(path, random, wait);
	CHECK(loaded.what == IdentityFile::Kept::Created);
	CHECK(loaded.id == 0x1111u);
	CHECK(Slurp(path) == "0000000000001111");
	CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer_player.id.tmp"));
	loaded = IdentityFile::LoadOrCreate(path, random, wait);
	CHECK(loaded.what == IdentityFile::Kept::Read);
	CHECK(loaded.id == 0x1111u);
	CHECK(asked == 2u);  // no new one was made
	// One with a line break after it (saved by hand) is read, and left as it is.
	REQUIRE(IniFile::WriteText(path, "00000000000a11ce\r\n"));
	loaded = IdentityFile::LoadOrCreate(path, random, wait);
	CHECK(loaded.what == IdentityFile::Kept::Read);
	CHECK(loaded.id == 0xA11CEu);
	CHECK(Slurp(path) == "00000000000a11ce\r\n");
	// A file that holds no identity (empty, zero, not a number) gets a new one.
	for (const std::string_view none : { "", "0", "\r\n", "xyz" }) {
		REQUIRE(IniFile::WriteText(path, none));
		loaded = IdentityFile::LoadOrCreate(path, random, wait);
		CHECK(loaded.what == IdentityFile::Kept::Created);
		CHECK(loaded.id != 0u);
		CHECK(Slurp(path) == std::format("{:016x}", loaded.id));
	}
	// Something else by the file's name (a folder) is there and can't be read: this run has an identity, and nothing
	// is written.
	const auto taken = folder.path / "taken.id";
	std::filesystem::create_directories(taken);
	CHECK(waits == 0);  // nothing so far was waited for: every file was read, or was not there
	loaded = IdentityFile::LoadOrCreate(taken, random, wait);
	CHECK(waits == 1);  // tried again once, after a moment
	CHECK(loaded.what == IdentityFile::Kept::Unreadable);
	CHECK(loaded.id != 0u);
	CHECK(std::filesystem::is_directory(taken));
	CHECK(std::filesystem::is_empty(taken));
	CHECK(!std::filesystem::exists(folder.path / "taken.id.tmp"));
	// No folder to write it in: the identity holds for this run.
	loaded = IdentityFile::LoadOrCreate(folder.path / "no-such-folder" / "F4Multiplayer_player.id", random, wait);
	CHECK(loaded.what == IdentityFile::Kept::NotCreated);
	CHECK(loaded.id != 0u);
	CHECK(!std::filesystem::exists(folder.path / "no-such-folder"));
}

// The host's ban list: what is there by the file's name and can't be read is not an empty list to save in its place.
TEST("ban list: something else by the file's name is not written over, and a list that is not there is saved as before")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer_bans.txt";
	std::filesystem::create_directories(path);
	BanList bans;
	CHECK(!bans.Load(path).empty());
	CHECK(bans.Add(0x5EA1, 0xB0B, 1792000000, "") != nullptr);  // made-up identities
	CHECK(!bans.Save(path));
	CHECK(std::filesystem::is_directory(path));
	CHECK(std::filesystem::is_empty(path));
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	// No file at all is an empty list, and the first ban writes it.
	const auto none = folder.path / "none.txt";
	CHECK(bans.Load(none).empty());
	CHECK(bans.Size() == 0u);
	CHECK(bans.Add(0x5EA1, 0xB0B, 1792000000, "") != nullptr);
	CHECK(bans.Save(none));
	BanList after;
	CHECK(after.Load(none).empty());
	CHECK(after.Size() == 1u);
}

// The same for a symbolic link (a mod manager that deploys by them, or a player who keeps the file elsewhere).
TEST("IniFile: the menu's save writes a symbolic link's file in place, and the link stays a link")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const auto        staged = folder.path / "staged.ini";
	const std::string mine = "bHost = true\r\nsPlayerName = Sam\r\n";
	REQUIRE(IniFile::WriteText(staged, mine));
	std::error_code error;
	std::filesystem::create_symlink(staged, path, error);
#ifndef _WIN32
	CHECK(!error);  // Windows makes one only for an administrator or in developer mode
#endif
	if (!error) {
		CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
		CHECK(std::filesystem::is_symlink(path));
		CHECK(Slurp(staged) == "bHost = false\r\nsPlayerName = Sam\r\n");
		CHECK(Slurp(path) == Slurp(staged));
		CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
		// The start-up step reads through it and appends to the file it names.
		CHECK(IniFile::LoadOrCreate(path, "bHost = false\nbNew = true\n").what == IniFile::Kept::Extended);
		CHECK(std::filesystem::is_symlink(path));
		CHECK(Slurp(staged) == "bHost = false\r\nsPlayerName = Sam\r\n\r\nbNew = true\r\n");
	}
}

namespace
{
	// A disk that fills up, or fails, part way through a write: half the bytes go in.
	bool PutHalf(std::ostream& a_out, std::string_view a_text)
	{
		a_out.write(a_text.data(), static_cast<std::streamsize>(a_text.size() / 2));
		a_out.flush();
		return false;
	}
}

// No save may leave the player's file shorter than it was. The temporary file takes the short write, and the
// settings file is not touched: writing it in place after that would fail the same way, with the file already emptied.
TEST("IniFile: a save whose write comes up short leaves the settings file byte for byte, and no temporary file")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const auto        temp = folder.path / "F4Multiplayer.ini.tmp";
	const std::string mine = "# my notes\r\nbHost = true\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS, PutHalf) == IniFile::Saved::NotWritten);
	CHECK(Slurp(path) == mine);
	CHECK(!std::filesystem::exists(temp));
	CHECK(!IniFile::WriteWhole(path, "bHost = false\r\n", PutHalf));
	CHECK(Slurp(path) == mine);
	CHECK(!std::filesystem::exists(temp));
	// A new install's file that can't be written whole is not left as half a file: the next start writes it.
	const auto fresh = folder.path / "fresh.ini";
	CHECK(!IniFile::WriteWhole(fresh, DEFAULTS, PutHalf));
	CHECK(!std::filesystem::exists(fresh));
	CHECK(!std::filesystem::exists(folder.path / "fresh.ini.tmp"));
	// The disk has room again: the save goes through.
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == "# my notes\r\nbHost = false\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n");
}

// A file written in place (a link, or one the temporary file could not replace) is never emptied first: a write that
// stops part way leaves the new text as far as it got and the old from there on, every line still there.
TEST("IniFile: a write in place that comes up short leaves the file no shorter than it was")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "# my notes\r\nbHost = true\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	const auto longer = IniFile::SetValue(mine, "bFriendlyFire", "false");
	CHECK(!IniFile::WriteOver(path, longer, PutHalf));
	CHECK(Slurp(path) == longer.substr(0, longer.size() / 2) + mine.substr(longer.size() / 2));
	CHECK(Slurp(path) == mine);  // the half that went in is the half the two share
	const auto shorter = IniFile::SetValue(mine, "sPlayerName", "S");
	CHECK(!IniFile::WriteOver(path, shorter, PutHalf));
	CHECK(Slurp(path).size() == mine.size());
	CHECK(Slurp(path) == shorter.substr(0, shorter.size() / 2) + mine.substr(shorter.size() / 2));
	// Whole, it is the new text, cut to its length or grown to it; with no file there, one is made.
	CHECK(IniFile::WriteOver(path, shorter));
	CHECK(Slurp(path) == shorter);
	CHECK(IniFile::WriteOver(path, longer));
	CHECK(Slurp(path) == longer);
	CHECK(IniFile::WriteOver(path, ""));
	CHECK(Slurp(path).empty());
	const auto none = folder.path / "none.ini";
	CHECK(IniFile::WriteOver(none, mine));
	CHECK(Slurp(none) == mine);
	// A hard link is written this way: the save that stops part way leaves both names with every line.
	const auto staged = folder.path / "staged.ini";
	REQUIRE(IniFile::WriteText(staged, mine));
	const auto linked = folder.path / "linked.ini";
	std::error_code error;
	std::filesystem::create_hard_link(staged, linked, error);
	if (!error) {  // a disk without hard links can't make this case
		CHECK(IniFile::SaveValue(linked, "bFriendlyFire", "false", DEFAULTS, PutHalf) == IniFile::Saved::NotWritten);
		CHECK(Slurp(linked) == mine);
		CHECK(Slurp(staged) == mine);
		CHECK(std::filesystem::hard_link_count(linked) == 2u);
		CHECK(!std::filesystem::exists(folder.path / "linked.ini.tmp"));
	}
}

#ifdef _WIN32
// Another program has the file open and lets nobody else read it (an editor, an antivirus scan, a cloud sync, for a
// moment or for long). Writing is still allowed here, so the save that takes a failed read for an empty file would
// go through: the file must be what it was, byte for byte.
TEST("IniFile: a settings file another program holds open against reading is left byte for byte")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "# my notes\r\nbHost = true\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	for (const int share : { _SH_DENYRD, _SH_DENYRW }) {
		std::FILE* held = _wfsopen(path.c_str(), L"rb", share);
		REQUIRE(held != nullptr);
		CHECK(IniFile::ReadText(path).what == IniFile::Found::Unreadable);
		CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Unreadable);
		// The start-up step: the defaults are the settings for this run, and nothing is appended.
		const auto loaded = IniFile::LoadOrCreate(path, DEFAULTS, "# added");
		CHECK(loaded.what == IniFile::Kept::Unreadable);
		CHECK(loaded.text == DEFAULTS);
		std::fclose(held);
		CHECK(Slurp(path) == mine);
		CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
	}
	// Let go of, it is saved to as any other.
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == "# my notes\r\nbHost = false\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n");
}

// A program that locks a part of the file: it opens, and the read stops where the lock begins, which looks like the
// end of a shorter file. Taken for the whole file, the save would write back only that part, and the start-up step
// would append the keys of the rest again, at their defaults, below the player's.
TEST("IniFile: a settings file that can only be read in part is left byte for byte")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "# my notes\r\nbHost = true\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n" + std::string(9000, '#') + "\r\niMaxRevives = 2\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	int fd = -1;
	REQUIRE(_wsopen_s(&fd, path.c_str(), _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0);
	REQUIRE(_lseek(fd, 8000, SEEK_SET) == 8000);
	REQUIRE(_locking(fd, _LK_NBLCK, 100) == 0);
	CHECK(IniFile::ReadText(path).what == IniFile::Found::Unreadable);
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Unreadable);
	CHECK(IniFile::LoadOrCreate(path, DEFAULTS, "# added").what == IniFile::Kept::Unreadable);
	REQUIRE(_lseek(fd, 8000, SEEK_SET) == 8000);
	REQUIRE(_locking(fd, _LK_UNLCK, 100) == 0);
	_close(fd);
	CHECK(Slurp(path) == mine);
	CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
	CHECK(IniFile::ReadText(path).text == mine);
}

// The host's ban list is read once, when the server starts, and written at each ban: a list that could not be read
// then is not the list to put in the file's place later.
TEST("ban list: a file that could not be read when the server started is not replaced by a later ban")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer_bans.txt";
	BanList          before{ 0x5A17 };
	REQUIRE(before.Add(0x5EA1, 0xB0B, 1792000000, "") != nullptr);  // made-up identities
	REQUIRE(before.Save(path));
	const auto mine = Slurp(path);

	std::FILE* held = _wfsopen(path.c_str(), L"rb", _SH_DENYRD);
	REQUIRE(held != nullptr);
	BanList bans;
	CHECK(!bans.Load(path).empty());
	CHECK(bans.Size() == 0u);
	std::fclose(held);
	CHECK(bans.Add(0x5EA2, 0, 1792000001, "") != nullptr);
	CHECK(!bans.Save(path));
	CHECK(Slurp(path) == mine);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	// Read on the next start, it is written to as before.
	CHECK(bans.Load(path).empty());
	CHECK(bans.Size() == 1u);
	CHECK(bans.Add(0x5EA2, 0, 1792000001, "") != nullptr);
	CHECK(bans.Save(path));
	BanList after;
	CHECK(after.Load(path).empty());
	CHECK(after.Size() == 2u);
}

// The player id file held by another program for a moment: the identity in it is the player's, and stays.
TEST("identity file: one another program holds open against reading keeps the player's identity")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer_player.id";
	REQUIRE(IniFile::WriteText(path, "00000000000a11ce"));
	const auto random = [] { return std::uint64_t{ 0xB0B }; };
	int        waits = 0;
	const auto wait = [&] { ++waits; };
	for (const int share : { _SH_DENYRD, _SH_DENYRW }) {
		std::FILE* held = _wfsopen(path.c_str(), L"rb", share);
		REQUIRE(held != nullptr);
		const auto loaded = IdentityFile::LoadOrCreate(path, random, wait);
		CHECK(loaded.what == IdentityFile::Kept::Unreadable);
		CHECK(loaded.id == 0xB0Bu);  // this run's own
		std::fclose(held);
		CHECK(Slurp(path) == "00000000000a11ce");
		CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer_player.id.tmp"));
	}
	CHECK(waits == 2);  // each was tried again once, after a moment
	// The next start reads it again.
	const auto again = IdentityFile::LoadOrCreate(path, random, wait);
	CHECK(again.what == IdentityFile::Kept::Read);
	CHECK(again.id == 0xA11CEu);
	CHECK(waits == 2);
}

// A sync tool or a virus scan holds a file for a moment: the second try, a moment later, reads the player's identity.
TEST("identity file: one held for a moment is read on the second try")
{
	const TempFolder folder;
	const auto       path = folder.path / "F4Multiplayer_player.id";
	REQUIRE(IniFile::WriteText(path, "00000000000a11ce"));
	std::FILE* held = _wfsopen(path.c_str(), L"rb", _SH_DENYRD);
	REQUIRE(held != nullptr);
	int        waits = 0;
	const auto loaded = IdentityFile::LoadOrCreate(
		path, [] { return std::uint64_t{ 0xB0B }; }, [&] {
			++waits;
			std::fclose(held);  // the other program lets go during the wait
		});
	CHECK(waits == 1);
	CHECK(loaded.what == IdentityFile::Kept::Read);
	CHECK(loaded.id == 0xA11CEu);
	CHECK(Slurp(path) == "00000000000a11ce");
}

// Another program has the file open in a way that lets it be read and written but not replaced (most editors and
// viewers): the temporary file can't take its name, and the save goes into the file itself, whole.
TEST("IniFile: a save that can't put its temporary file in the file's place writes the file itself, whole")
{
	const TempFolder  folder;
	const auto        path = folder.path / "F4Multiplayer.ini";
	const std::string mine = "# my notes\r\nbHost = true\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n";
	const std::string want = "# my notes\r\nbHost = false\r\nsPlayerName = Sam\r\nbFriendlyFire = true\r\n";
	REQUIRE(IniFile::WriteText(path, mine));
	std::FILE* held = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);  // shares reading and writing, not deleting or renaming
	REQUIRE(held != nullptr);
	CHECK(IniFile::SaveValue(path, "bHost", "false", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == want);
	CHECK(!std::filesystem::exists(folder.path / "F4Multiplayer.ini.tmp"));
	// The other program's handle is still the file's: it reads the new text, so the file was written, not replaced.
	std::string seen(want.size() + 16, '\0');
	std::rewind(held);
	seen.resize(std::fread(seen.data(), 1, seen.size(), held));
	CHECK(seen == want);
	// A shorter text over it is cut to its length.
	CHECK(IniFile::SaveValue(path, "sPlayerName", "S", DEFAULTS) == IniFile::Saved::Written);
	CHECK(Slurp(path) == "# my notes\r\nbHost = false\r\nsPlayerName = S\r\nbFriendlyFire = true\r\n");
	std::fclose(held);
	CHECK(std::distance(std::filesystem::directory_iterator{ folder.path }, std::filesystem::directory_iterator{}) == 1);
}
#endif
