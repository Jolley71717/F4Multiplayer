#include "Test.h"

#include "IniFile.h"
#include "SourceTree.h"

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
		CHECK(!IniFile::WriteText(path, "bHost = false\r\n"));
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
