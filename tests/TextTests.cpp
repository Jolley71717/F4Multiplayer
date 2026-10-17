#include "Test.h"

#include "Protocol.h"
#include "SourceTree.h"
#include "Text.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

// The mod's own texts: the placeholder formatter, the language files and the guard that keeps a new message from being
// written as a bare English literal.

namespace
{
	// Leaves the language as the plugin has it by default (English, nothing loaded) however a test ends.
	struct English
	{
		English() { Text::Use("en", {}); }
		~English() { Text::Use("en", {}); }
	};

	std::string FileOf(const std::filesystem::path& a_path)
	{
		return SourceTree::Read(a_path);
	}

	std::vector<std::filesystem::path> LanguageFiles(const std::filesystem::path& a_root)
	{
		std::vector<std::filesystem::path> files;
		std::error_code                    error;
		for (const auto& entry : std::filesystem::directory_iterator{ a_root / "dist" / "lang", error }) {
			if (entry.is_regular_file() && entry.path().extension() == ".txt") {
				files.push_back(entry.path());
			}
		}
		std::ranges::sort(files);
		return files;
	}

	std::vector<std::string> CodesOf(const std::vector<std::filesystem::path>& a_files)
	{
		std::vector<std::string> codes;
		for (const auto& file : a_files) {
			codes.push_back(file.stem().string());
		}
		return codes;
	}
}

TEST("text: placeholders are filled by name, and a missing one stays visible")
{
	CHECK(Text::Expand("{player} killed {victim}", { { "victim", "a Raider" }, { "player", "Sam" } }) == "Sam killed a Raider");
	CHECK(Text::Expand("{n} and {n} again", { { "n", 3 } }) == "3 and 3 again");
	// An argument the text lacks is ignored; a placeholder the arguments lack is left as written.
	CHECK(Text::Expand("Hello {player}", { { "other", "x" } }) == "Hello {player}");
	CHECK(Text::Expand("Hello {player}", {}) == "Hello {player}");
	CHECK(Text::Expand("no placeholders", { { "n", 1 } }) == "no placeholders");
	// Braces that open no name are text.
	CHECK(Text::Expand("a { b } c {} d {", { { "b", "x" } }) == "a { b } c {} d {");
	CHECK(Text::Expand("{n}{n}", { { "n", std::size_t{ 12 } } }) == "1212");
	// What an argument holds is not read again.
	CHECK(Text::Expand("{a}", { { "a", "{b}" }, { "b", "x" } }) == "{b}");
	CHECK(Text::Expand("{player}: 100%", { { "player", "Zed" } }) == "Zed: 100%");
}

TEST("text: a language file is key = text lines")
{
	const auto table = Text::Parse("\xEF\xBB\xBF# a comment\r\n\r\nparty.called = Du hast deine Gruppe gerufen\r\n  menu.close=Schlie\xC3\x9F" "en  \r\nmenu.none_hosting = eins\\nzwei \\\\ drei\r\nno equals sign\r\n= no key\r\n# party.killed = ignored\r\nparty.xp = +{xp} EP\r\n");
	CHECK(table.size() == 4u);
	CHECK(table.at("party.called") == "Du hast deine Gruppe gerufen");
	CHECK(table.at("menu.close") == "Schlie\xC3\x9F" "en");
	CHECK(table.at("menu.none_hosting") == "eins\nzwei \\ drei");
	CHECK(table.at("party.xp") == "+{xp} EP");
	CHECK(!table.contains("party.killed"));
	CHECK(Text::Parse("").empty());
	// A text may hold '=' and '#'.
	CHECK(Text::Parse("k = a = b # c\n").at("k") == "a = b # c");
	CHECK(Text::Escape("a\nb\\c") == "a\\nb\\\\c");
	CHECK(Text::Parse("k = " + Text::Escape("one\ntwo\\n three")).at("k") == "one\ntwo\\n three");
}

TEST("text: a key the language file lacks shows the English")
{
	English english;
	Text::Use("de", Text::Parse("party.called = Du hast deine Gruppe gerufen\nmenu.close =\nmenu.back = Zur\xC3\xBC" "ck\n"));
	CHECK(Text::Get("party.called") == "Du hast deine Gruppe gerufen");
	CHECK(Text::Get("menu.back") == "Zur\xC3\xBC" "ck");
	CHECK(Text::Get("menu.close") == "Close");   // an empty text is a text left to translate
	CHECK(Text::Get("menu.settings") == "Settings");  // not in the file at all
	CHECK(Text::Format("party.teleported", { { "player", "Sam" } }) == "Teleported to Sam");
	CHECK(Text::Language() == "de");
	// A key nobody has shows as itself, so the mistake is seen where it is read.
	CHECK(Text::Get("no.such.key") == "no.such.key");
	// The English is what the log keeps, whatever the player reads.
	Text::Use("de", Text::Parse("party.teleported = Teleportiert zu {player}\n"));
	CHECK(Text::Format("party.teleported", { { "player", "Sam" } }) == "Teleportiert zu Sam");
	CHECK(Text::FormatEnglish("party.teleported", { { "player", "Sam" } }) == "Teleported to Sam");
	// A translation that dropped or misspelled a placeholder shows the fault instead of a gap.
	Text::Use("de", Text::Parse("party.teleported = Teleportiert zu {spieler}\n"));
	CHECK(Text::Format("party.teleported", { { "player", "Sam" } }) == "Teleportiert zu {spieler}");
}

TEST("text: the game's language names map to the language files")
{
	CHECK(Text::CodeFor("en") == "en");
	CHECK(Text::CodeFor("EN") == "en");
	CHECK(Text::CodeFor("fr") == "fr");
	CHECK(Text::CodeFor("de") == "de");
	CHECK(Text::CodeFor("it") == "it");
	CHECK(Text::CodeFor("es") == "es");
	CHECK(Text::CodeFor("esmx") == "es");
	CHECK(Text::CodeFor("pl") == "pl");
	CHECK(Text::CodeFor("ru") == "ru");
	CHECK(Text::CodeFor("ptbr") == "pt-br");
	CHECK(Text::CodeFor("pt-BR") == "pt-br");
	CHECK(Text::CodeFor("ja") == "ja");
	CHECK(Text::CodeFor("cn") == "zh-cn");
	CHECK(Text::CodeFor("zh-CN") == "zh-cn");
	CHECK(Text::CodeFor("zhhant") == "zh-tw");
	CHECK(Text::CodeFor("tchinese") == "zh-tw");
	CHECK(Text::CodeFor("ar") == "ar");
	CHECK(Text::CodeFor("en_GB") == "en-gb");
	CHECK(Text::CodeFor("  Russian ") == "ru");
	// What is not known keeps its name, for a language file the player adds.
	CHECK(Text::CodeFor("ko") == "ko");
	CHECK(Text::IsFileCode("zh-tw"));
	CHECK(!Text::IsFileCode(""));
	CHECK(!Text::IsFileCode("..\\..\\x"));
	CHECK(!Text::IsFileCode("a/b"));
	CHECK(!Text::IsFileCode("two words"));
}

TEST("text: every English key is unique and gives the literal the mod showed before")
{
	// The texts as the code wrote them before they moved here (one line per key, with the arguments it filled in).
	struct Case
	{
		const char*              key;
		std::vector<Text::Arg>   args;
		const char*              expected;
	};
	const std::vector<Case> cases = {
		{ "session.connected_shared", {}, "Multiplayer: connected (shared story)" },
		{ "session.connected_own", {}, "Multiplayer: connected (everyone has their own story)" },
		{ "session.connected_shared_fire", {}, "Multiplayer: connected (shared story, friendly fire is on)" },
		{ "session.connected_own_fire", {}, "Multiplayer: connected (everyone has their own story, friendly fire is on)" },
		{ "session.notice", { { "text", "Wrong password" } }, "Multiplayer: Wrong password" },
		{ "session.player_joined", { { "player", "Sam" } }, "Sam joined" },
		{ "session.player_left", { { "player", "Sam" } }, "Sam left" },
		{ "session.someone_left", {}, "A player left" },
		{ "session.disconnected", {}, "Multiplayer: disconnected" },
		{ "session.hosting_cant_join", {}, "Multiplayer: you're hosting. Set bHost = false to join friends." },
		{ "session.host_waits_for_world", {}, "Multiplayer: this game hosts once you're in the world (bHost). Stop hosting in the menu to join friends." },
		{ "session.joining_friend", {}, "Multiplayer: joining your friend's game" },
		{ "session.no_steam", {}, "Multiplayer: Steam isn't available" },
		{ "session.host_other_session", {}, "Multiplayer: the host is in another session now (another character, or a fresh start); join again from the menu to play in it" },
		{ "session.character_changed_host", {}, "Multiplayer: you loaded another character; hosting restarts with that character's session" },
		{ "session.hosting_resumes", {}, "Multiplayer: hosting starts again once you're out of Vault 111" },
		{ "session.character_changed_left", {}, "Multiplayer: you loaded another character and left the session; join again from the menu" },
		{ "host.err_opening", {}, "you can host once you're out of Vault 111" },
		{ "host.err_port", { { "port", 7779 } }, "could not host on port 7779" },
		{ "world.history_full", { { "percent", 85 } }, "Multiplayer: the session's shared history is 85% full; loot and kill sharing stop at 100%. The host can start a fresh session by removing the session file (see the docs)" },
		{ "reject.not_client", {}, "Not an F4Multiplayer client" },
		{ "reject.version", { { "server", 31 }, { "you", 30 } }, "Version mismatch: server uses protocol 31, you have 30. Install the same mod version." },
		{ "reject.password", {}, "Wrong password" },
		{ "reject.host_not_ready", {}, "The host isn't in the game yet" },
		{ "reject.full", {}, "Server is full" },
		{ "reject.order", {}, "Your load order is different from the other players." },
		{ "reject.order_all", {}, "Everyone needs the same mods in the same order." },
		{ "reject.only_you", {}, "Only you have:" },
		{ "reject.missing", {}, "You are missing:" },
		{ "reject.same_mods", {}, "Same mods, different order: sort yours like theirs." },
		{ "reject.texture", {}, "Texture plugins may differ only when they load last." },
		{ "party.unstable", {}, "Multiplayer: connection to the host is unstable" },
		{ "party.restored", {}, "Multiplayer: connection restored" },
		{ "party.high_ping", { { "ms", 250 } }, "Multiplayer: high ping to the host (250 ms)" },
		{ "party.ping_info", { { "ms", 40 } }, "ping 40 ms" },
		{ "party.not_in_session", {}, "Multiplayer: not in a session" },
		{ "party.a_friend", {}, "A friend" },
		{ "party.alone", {}, "Multiplayer: nobody else is here yet" },
		{ "party.count_one", { { "ping", " (ping 40 ms)" } }, "Party: 1 other player (ping 40 ms)" },
		{ "party.count_many", { { "n", 3 }, { "ping", "" } }, "Party: 3 other players" },
		{ "player.down", {}, "down" },
		{ "player.dead", {}, "dead" },
		{ "player.status", { { "hp", 80 }, { "level", 12 } }, "80% HP, level 12" },
		{ "player.loading", {}, "loading" },
		{ "player.right_here", {}, "right here" },
		{ "player.distance", { { "m", "230" }, { "dir", "NE" } }, "230 m NE" },
		{ "compass.n", {}, "N" },
		{ "compass.ne", {}, "NE" },
		{ "compass.e", {}, "E" },
		{ "compass.se", {}, "SE" },
		{ "compass.s", {}, "S" },
		{ "compass.sw", {}, "SW" },
		{ "compass.w", {}, "W" },
		{ "compass.nw", {}, "NW" },
		{ "party.somewhere_else", {}, "somewhere else" },
		{ "party.at_place", { { "place", "Sanctuary" } }, "at Sanctuary" },
		{ "party.friend_down_far", { { "player", "Sam" }, { "key", "F7" } }, "Sam is down! Hold F7 to teleport to them and help them up" },
		{ "party.friend_down_near", { { "player", "Sam" }, { "where", "20 m N" } }, "Sam is down! Get to them to help them up (20 m N)" },
		{ "party.friend_died", { { "player", "Sam" } }, "Sam died" },
		{ "party.friend_back_up", { { "player", "Sam" } }, "Sam is back up" },
		{ "party.ping", { { "player", "Sam" }, { "where", "at Sanctuary" } }, "Sam: over here! (at Sanctuary)" },
		{ "party.called", {}, "You called your party over" },
		{ "party.killed", { { "killer", "Sam" }, { "victim", "Raider" } }, "Sam killed Raider" },
		{ "party.xp", { { "xp", "25" } }, "+25 XP from your party's kills" },
		{ "party.no_teleport_target", {}, "Multiplayer: nobody to teleport to" },
		{ "party.teleport_prompt", { { "player", "Sam [a]" }, { "key", "F7" } }, "Teleport to Sam [a]? Hold F7 to go" },
		{ "party.teleport_choose", { { "key", "F7" } }, "Tap F7 to choose who to teleport to" },
		{ "party.cant_teleport_opening", {}, "Can't teleport until you've left Vault 111" },
		{ "party.cant_teleport_talking", {}, "Can't teleport while talking" },
		{ "party.cant_teleport_scene", {}, "Can't teleport during a scene" },
		{ "party.cant_teleport_busy", {}, "Can't teleport while sitting or using something" },
		{ "party.cant_teleport_survival", {}, "Can't teleport in Survival (no fast travel)" },
		{ "party.cant_find", { { "player", "Sam" } }, "Can't find Sam right now" },
		{ "party.still_in_vault", { { "player", "Sam" } }, "Sam hasn't left Vault 111 yet" },
		{ "party.teleported", { { "player", "Sam" } }, "Teleported to Sam" },
		{ "party.cant_teleport_into", { { "player", "Sam" } }, "Can't teleport into Sam's location" },
		{ "party.cant_teleport_to", { { "player", "Sam" } }, "Can't teleport to Sam" },
		{ "party.teleporting", { { "player", "Sam" } }, "Teleporting to Sam" },
		{ "down.went_down", { { "key", "F7" } }, "You're down! A friend can help you up. Hold F7 to give up" },
		{ "down.seconds_left", { { "n", 30 } }, "30 seconds left to be helped up" },
		{ "down.helping", { { "player", "Sam" } }, "Helping Sam up..." },
		{ "down.helped", { { "player", "Sam" } }, "You helped Sam up" },
		{ "down.helped_you", { { "player", "Sam" } }, "Sam helped you up" },
		{ "map.discovered", { { "player", "Sam" }, { "place", "Diamond City" } }, "Sam discovered Diamond City" },
		{ "quest.caught_up", { { "names", "Out of Time, When Freedom Calls" } }, "Caught up with your friends: Out of Time, When Freedom Calls" },
		{ "quest.and_more", { { "n", 2 } }, "and 2 more" },
		{ "quest.catching_up", { { "n", 10 }, { "quest", "Reunions" } }, "Catching up with your friends' story in 10 s: Reunions (it may move you)" },
		{ "quest.a_quest", {}, "a quest" },
		{ "quest.controls_back", {}, "Your friends' story step didn't finish; you have your controls back" },
		{ "quest.stuck_in", { { "quest", "Reunions" }, { "n", 10 } }, "Your main story was stuck; it goes on with Reunions in 10 s (it may move you)" },
		{ "quest.stuck", { { "quest", "Reunions" } }, "Your main story was stuck; it goes on with Reunions" },
		{ "quest.next", {}, "the next quest" },
		{ "quest.completed", { { "player", "Sam" }, { "quest", "Reunions" } }, "Sam completed Reunions" },
		{ "workshop.rescued", {}, "What you kept in a friend's container is in your inventory" },
		{ "menu.unavailable", {}, "Multiplayer: the menu isn't available" },
		{ "menu.more", { { "page", 1 }, { "pages", 2 } }, "More (1/2)" },
		{ "menu.on", {}, "On" },
		{ "menu.off", {}, "Off" },
		{ "menu.close", {}, "Close" },
		{ "menu.back", {}, "Back" },
		{ "menu.settings", {}, "Settings" },
		{ "menu.invite", {}, "Invite friends" },
		{ "menu.stop_hosting", {}, "Stop hosting" },
		{ "menu.leave", {}, "Leave" },
		{ "menu.join_other", {}, "Join someone else" },
		{ "menu.host", {}, "Host a session" },
		{ "menu.join", {}, "Join a friend" },
		{ "menu.look_again", {}, "Look again" },
		{ "menu.join_address", { { "address", "192.168.1.5:7779" } }, "Join 192.168.1.5:7779" },
		{ "menu.address_steam_named", { { "player", "Sam" } }, "Sam's game (Steam)" },
		{ "menu.address_steam_unnamed", { { "id", "76561198000000000" } }, "Steam user 76561198000000000 (Steam)" },
		{ "menu.hosting_udp", { { "port", 7779 } }, "Hosting on UDP port 7779 (friends join by address)." },
		{ "menu.hosting_udp_nosteam", { { "port", 7779 } }, "Hosting on UDP port 7779 (no Steam: friends join by address)." },
		{ "menu.hosting_steam", {}, "Hosting through Steam. Friends join from their friends list (Join Game) or your invite." },
		{ "menu.hosting_steam_opening", {}, "Hosting through Steam; opening the lobby..." },
		{ "menu.own_game_connecting", {}, "Your own game is still connecting to it." },
		{ "menu.host_waiting", {}, "Hosting starts once you're in the world and out of Vault 111." },
		{ "menu.not_in_session", {}, "Not in a session. Host one, or join a friend who is hosting." },
		{ "menu.connecting", { { "address", "Sam's game (Steam)" } }, "Connecting to Sam's game (Steam)..." },
		{ "menu.refused_soon", { { "reason", "The host isn't in the game yet" } }, "Refused: The host isn't in the game yet (trying again in a few seconds)" },
		{ "menu.refused_later", { { "reason", "Wrong password" } }, "Refused: Wrong password (trying again in a minute)" },
		{ "menu.connected", { { "address", "Sam's game (Steam)" } }, "Connected to Sam's game (Steam), joining..." },
		{ "menu.in_session", { { "address", "Sam's game (Steam)" } }, "In Sam's game (Steam)" },
		{ "menu.nobody_else", {}, "Nobody else is here yet." },
		{ "menu.with_you", { { "n", 2 } }, "With you (2):" },
		{ "menu.host_failed", { { "error", "could not host on port 7779" } }, "Could not host: could not host on port 7779." },
		{ "menu.friends_hosting", {}, "Steam friends hosting Fallout 4 co-op right now:" },
		{ "menu.joining_host", { { "player", "Sam" } }, "Multiplayer: joining Sam's game" },
		{ "menu.steam_missing", {}, "Steam isn't available in this game, so friends can't be listed." },
		{ "menu.none_hosting", { { "friends", 12 }, { "online", 3 }, { "ingame", 1 } },
			"None of your Steam friends is hosting right now (12 friends, 3 online, 1 in Fallout 4).\nA host shows up here only when Steam shows them playing Fallout 4 (their profile must say so): start the game through Steam with F4SE as the launcher, and set the profile's Game details to Friends Only or Public.\nOtherwise the host can invite you from the Steam overlay (Shift+Tab), or pick Join Game on them in your friends list." },
		{ "menu.ini_address", { { "address", "1.2.3.4" } }, "Address from the ini: 1.2.3.4" },
		{ "menu.connecting_hud", { { "address", "1.2.3.4" } }, "Multiplayer: connecting to 1.2.3.4" },
		{ "menu.setting_button", { { "name", "Voice chat" }, { "state", "On" } }, "Voice chat: On" },
		{ "menu.setting_line", { { "name", "Voice chat" }, { "state", "On" }, { "what", "hold the voice key to talk to friends" } }, "Voice chat: On (hold the voice key to talk to friends)" },
		{ "menu.settings_saved", {}, "Changes are saved to F4Multiplayer.ini." },
		{ "menu.friendly_fire", {}, "Friendly fire" },
		{ "menu.friendly_fire_what", {}, "players can hurt each other (host's setting, from the next session hosted)" },
		{ "menu.shared_story", {}, "Shared story" },
		{ "menu.shared_story_what", {}, "anyone's main story and faction progress moves everyone along, achievements included, and goes into each save (host's setting, from the next session hosted)" },
		{ "menu.voice_chat", {}, "Voice chat" },
		{ "menu.voice_chat_what", {}, "hold the voice key to talk to friends" },
		{ "menu.spoken_lines", {}, "Spoken lines" },
		{ "menu.spoken_lines_what", {}, "hear what's said near you in a friend's conversations, in the speaker's voice" },
		{ "menu.compass_markers", {}, "Compass markers" },
		{ "menu.compass_markers_what", {}, "friends on the compass" },
		{ "menu.fast_loading", {}, "Fast loading" },
		{ "menu.fast_loading_what", {}, "vsync off during loading screens" },
	};

	English english;
	std::set<std::string_view> listed;
	for (const auto& c : cases) {
		CHECK(listed.insert(c.key).second);
		CHECK(Text::Expand(Text::GetEnglish(c.key), c.args) == c.expected);
		CHECK(Text::Format(c.key, {}).find('{') != std::string::npos || c.args.empty());  // a bare Get keeps its placeholders visible
	}
	// Every key of the table is in the list above (a new text needs its literal pinned here), and none twice.
	std::set<std::string_view> keys;
	for (const auto& entry : Text::English()) {
		CHECK(keys.insert(entry.key).second);
		CHECK(!entry.text.empty());
		CHECK(listed.contains(entry.key));
	}
	CHECK(keys == listed);
}

TEST("text: a server's refusals read in the player's language, and a changed server's own text is shown as it came")
{
	English english;
	const std::vector<std::string> sent = {
		"Not an F4Multiplayer client",
		"Wrong password",
		"The host isn't in the game yet",
		"Server is full",
		"Version mismatch: server uses protocol 31, you have 30. Install the same mod version.",
		Protocol::LoadOrderDifference({ "Fallout4.esm", "Evil.esp", "Mine.esp" }, { "Fallout4.esm", "Theirs.esp" }),
		Protocol::LoadOrderDifference({ "Fallout4.esm", "A.esp", "B.esp" }, { "Fallout4.esm", "B.esp", "A.esp" }),
		Protocol::LoadOrderDifference({ "Fallout4.esm", "ccbgsfo4044-hellfirepowerarmor.esl" }, {}),
		Protocol::LoadOrderDifference({ "Fallout4.esm", "HD.esp", "Other.esp" }, { "Fallout4.esm", "Other.esp" }, { "HD.esp" }),
		"Some refusal a changed server made up",
	};
	// In English nothing changes: the text is the server's, byte for byte.
	for (const auto& reason : sent) {
		CHECK(Text::Reject(reason) == reason);
	}
	// In another language the parts this client knows are translated, the plugin names and numbers stay.
	Text::Use("de", Text::Parse(
		"reject.password = Falsches Passwort\n"
		"reject.version = Versionskonflikt: Server {server}, du {you}.\n"
		"reject.order = Deine Ladereihenfolge ist anders.\n"
		"reject.only_you = Nur du hast:\n"
		"reject.missing = Dir fehlt:\n"));
	CHECK(Text::Reject("Wrong password") == "Falsches Passwort");
	CHECK(Text::Reject("Server is full") == "Server is full");  // no German text: the English
	CHECK(Text::Reject(sent[4]) == "Versionskonflikt: Server 31, du 30.");
	CHECK(Text::Reject("Version mismatch: server uses protocol x, you have 30. Install the same mod version.") ==
		  "Version mismatch: server uses protocol x, you have 30. Install the same mod version.");
	CHECK(Text::Reject(sent[5]) == "Deine Ladereihenfolge ist anders. Nur du hast: Evil.esp, Mine.esp. Dir fehlt: Theirs.esp.");
	CHECK(Text::Reject("Some refusal a changed server made up") == "Some refusal a changed server made up");
	CHECK(Text::Reject("") == "");
}

TEST("text: every language file holds only the mod's keys, with the placeholders they have in English")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	const auto files = LanguageFiles(*root);
	REQUIRE(!files.empty());

	std::map<std::string, std::vector<std::string>> placeholders;
	for (const auto& entry : Text::English()) {
		placeholders[std::string{ entry.key }] = Text::Placeholders(entry.text);
	}

	// The languages the mod ships, each with a file.
	const auto codes = CodesOf(files);
	for (const std::string_view wanted : { "en", "en-gb", "ru", "es", "ar", "de", "fr", "it", "pl", "pt-br", "ja", "zh-cn", "zh-tw" }) {
		CHECK(std::ranges::find(codes, wanted) != codes.end());
	}
	// What the game calls its languages finds one of those files.
	for (const std::string_view game : { "en", "fr", "de", "it", "es", "esmx", "pl", "ru", "ptbr", "ja", "cn", "zhhant" }) {
		CHECK(std::ranges::find(codes, Text::CodeFor(game)) != codes.end());
	}

	for (const auto& file : files) {
		const auto name = file.filename().string();
		const auto text = FileOf(file);
		CHECK(Text::IsFileCode(file.stem().string()));
		CHECK(!text.starts_with("\xEF\xBB\xBF"));  // no byte order mark: the plugin and the translators' editors agree on plain UTF-8
		const auto table = Text::Parse(text);
		for (const auto& [key, translated] : table) {
			const auto expected = placeholders.find(key);
			if (expected == placeholders.end()) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: key '{}' is not one the mod uses", name, key));
				continue;
			}
			if (Text::Placeholders(translated) != expected->second) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: '{}' has different placeholders than the English ({})", name, key, translated));
			}
			CHECK(!translated.empty());
		}
		// Whole files: a missing key would show in English in the middle of a translated game.
		for (const auto& [key, ignored] : placeholders) {
			if (!table.contains(key)) {
				Test::Fail(__FILE__, __LINE__, std::format("{}: lacks the key '{}'", name, key));
			}
		}
		// A file that is not English says so, for the players who correct it.
		if (file.stem() != "en" && file.stem() != "en-gb") {
			CHECK(text.find("# machine-translated first pass, corrections welcome") != std::string::npos);
		}
	}
	CHECK(FileOf(files.front().parent_path() / "ar.txt").find("font") != std::string::npos);
}

TEST("text: dist/lang/en.txt is the built-in English")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto table = Text::Parse(FileOf(*root / "dist" / "lang" / "en.txt"));
	CHECK(table.size() == Text::English().size());
	for (const auto& entry : Text::English()) {
		const auto it = table.find(entry.key);
		CHECK(it != table.end() && it->second == entry.text);
	}
}

TEST("text: British English differs from the built-in only in spelling")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	const auto table = Text::Parse(FileOf(*root / "dist" / "lang" / "en-gb.txt"));
	for (const auto& [key, translated] : table) {
		const auto english = Text::GetEnglish(key);
		// The same words: the two texts agree once the spellings that differ are put the same.
		auto american = english;
		for (const auto& [us, uk] : std::vector<std::pair<std::string, std::string>>{ { "color", "colour" }, { "armor", "armour" }, { "favor", "favour" }, { "savor", "savour" }, { "neighbor", "neighbour" }, { "honor", "honour" }, { "center", "centre" }, { "defense", "defence" }, { "gray", "grey" }, { "ize", "ise" }, { "izing", "ising" } }) {
			for (std::size_t at = american.find(us); at != std::string::npos; at = american.find(us, at + uk.size())) {
				american.replace(at, us.size(), uk);
			}
		}
		CHECK(american == translated);
	}
}

TEST("text: no player-facing message is a bare English literal in the code")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	int found = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator{ *root / "src" }) {
		const auto ext = entry.path().extension();
		if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
			continue;
		}
		const auto text = SourceTree::Read(entry.path());
		const auto name = std::filesystem::relative(entry.path(), *root).generic_string();
		// A HUD notice is a Text key's text, not a literal. (A friend's chat line, "{}: {}", is not the mod's text.)
		for (std::size_t at = text.find("Hud::Notify("); at != std::string::npos; at = text.find("Hud::Notify(", at + 1)) {
			auto rest = std::string_view{ text }.substr(at + 12);
			rest.remove_prefix(rest.find_first_not_of(" \t\r\n"));
			if (rest.starts_with("std::format(")) {
				rest.remove_prefix(12);
				rest.remove_prefix(rest.find_first_not_of(" \t\r\n"));
			}
			if (rest.starts_with('"') && !rest.starts_with("\"{}: {}\"")) {
				++found;
				Test::Fail(__FILE__, __LINE__, std::format("{} shows a literal on the HUD: {}", name, std::string{ rest.substr(0, 60) }));
			}
		}
	}
	// The menu's pages and buttons: a button label or a page's text starting from a letter is English in the code.
	// (A line break or a lower-case key, as in { "page", n }, is not.)
	const auto menu = SourceTree::Read(*root / "src" / "game" / "Menu.cpp");
	for (const std::string_view start : { "push_back({ \"", "{ { \"", "body = \"", "body += \"" }) {
		for (auto at = menu.find(start); at != std::string::npos; at = menu.find(start, at + 1)) {
			const char next = at + start.size() < menu.size() ? menu[at + start.size()] : '\0';
			if (std::isupper(static_cast<unsigned char>(next)) || (start != "{ { \"" && std::isalpha(static_cast<unsigned char>(next)))) {
				++found;
				Test::Fail(__FILE__, __LINE__, std::format("src/game/Menu.cpp builds text from a literal ({}{}...): use Text::Get", start, next));
			}
		}
	}
	CHECK(found == 0);
}
