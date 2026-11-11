#include "SourceTree.h"
#include "Test.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// A multiplayer notice at the HUD's top left plays no sound: it asks the game for a null sound name, as the game's own
// notices do. A name the game looks up ("" once matched a sound record with an empty editor ID: a generator's hum or
// rushing water that stacked with every notice) would play with every notice, and several notices come at once (a
// friend downed and helped up again and again, a join, pings). The rule is on code that runs only in the game, so it is
// checked on the source (src/game/Hud.cpp says how the game plays a notice's sound).

namespace
{
	// The arguments of each call to a_name( in a_code, as written (spaces trimmed), top-level commas only.
	std::vector<std::vector<std::string>> CallArguments(std::string_view a_code, std::string_view a_name)
	{
		const auto trim = [](std::string a_text) {
			const auto first = a_text.find_first_not_of(" \t\r\n");
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return first == std::string::npos ? std::string{} : a_text.substr(first, last - first + 1);
		};
		std::vector<std::vector<std::string>> calls;
		for (auto at = a_code.find(a_name); at != std::string_view::npos; at = a_code.find(a_name, at + 1)) {
			auto open = at + a_name.size();
			while (open < a_code.size() && (a_code[open] == ' ' || a_code[open] == '\t')) {
				++open;
			}
			if (open >= a_code.size() || a_code[open] != '(') {
				continue;  // the name, not a call (a using, a decltype)
			}
			std::vector<std::string> arguments;
			std::string              current;
			int                      depth = 0;
			for (auto i = open + 1; i < a_code.size(); ++i) {
				const char c = a_code[i];
				if (depth == 0 && (c == ',' || c == ')')) {
					arguments.push_back(trim(current));
					current.clear();
					if (c == ')') {
						break;
					}
					continue;
				}
				depth += (c == '(' || c == '[' || c == '{') - (c == ')' || c == ']' || c == '}');
				current += c;
			}
			calls.push_back(std::move(arguments));
		}
		return calls;
	}

	std::string Squeezed(std::string_view a_code)
	{
		std::string out;
		for (const char c : a_code) {
			if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
				out += c;
			}
		}
		return out;
	}
}

TEST("notice sound: the guard reads a call's arguments, not its comments or strings")
{
	const auto calls = CallArguments(SourceTree::CodeOnly(R"x(
		// ShowHUDMessage(a, "loud", b)
		RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), NOTICE_SOUND, false, Pick(1, 2));
		RE::SendHUDMessage::ShowHUDMessage (text.c_str(), "", true, false);
		using f = decltype(&SendHUDMessage::ShowHUDMessage);)x"),
		"ShowHUDMessage");
	REQUIRE(calls.size() == 2u);
	const std::vector<std::string> first{ "a_clean.c_str()", "NOTICE_SOUND", "false", "Pick(1, 2)" };
	CHECK(calls[0] == first);
	REQUIRE(calls[1].size() == 4u);
	CHECK(calls[1][1].empty());  // a string literal, "" or a name, is never NOTICE_SOUND
}

TEST("notice sound: every HUD notice goes out through Hud.cpp with the null sound name")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	int calls = 0;
	for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
		const auto found = CallArguments(code, "ShowHUDMessage");
		if (found.empty()) {
			continue;
		}
		if (file != "src/game/Hud.cpp") {
			Test::Fail(__FILE__, __LINE__, std::format("{} shows a HUD message itself: send it through Hud::Notify, which asks for no sound", file));
			continue;
		}
		for (const auto& arguments : found) {
			++calls;
			if (arguments.size() != 4 || arguments[1] != "NOTICE_SOUND") {
				Test::Fail(__FILE__, __LINE__, std::format("{}: a ShowHUDMessage call's sound is '{}', not NOTICE_SOUND", file, arguments.size() > 1 ? arguments[1] : std::string{}));
			}
		}
		const auto squeezed = Squeezed(code);
		CHECK(squeezed.find("constexprconstchar*NOTICE_SOUND=nullptr;") != std::string::npos);
	}
	CHECK(calls >= 1);
}

TEST("notice sound: the plugin plays a menu sound only on its own menu's presses, or counted")
{
	// A press plays one: these never repeat on their own. A new caller that can play one without a press is a source of
	// repeating sounds: count it in ClickWatch (src/game/ClickRates.h) and add it here.
	// WorkshopSync.cpp's one is the workshop's store sound when the host's game answers that it stored the piece a friend's
	// Store asked for (the press was the friend's, the sound waits for the answer): one an answer, counted as storeSounds.
	const std::map<std::string, int> allowed{ { "src/game/UiMainMenuEntry.cpp", 1 }, { "src/game/UiPanel.cpp", 4 }, { "src/game/WorkshopSync.cpp", 1 } };
	const auto                       root = SourceTree::Root();
	REQUIRE(root);
	for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
		const auto found = static_cast<int>(CallArguments(code, "PlayMenuSound").size());
		if (found == 0) {
			continue;
		}
		const auto it = allowed.find(file);
		if (it == allowed.end() || found > it->second) {
			Test::Fail(__FILE__, __LINE__, std::format("{} plays a menu sound {} time(s) in its code: if it can play without a press, count it in ClickWatch", file, found));
		}
	}
}
