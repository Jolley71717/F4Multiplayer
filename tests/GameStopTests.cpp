// How long our game has been stopped by a menu (GameStop::Clock, as NpcSync::Frame advances it each frame).

#include "Test.h"

#include "SourceTree.h"
#include "game/GameStop.h"

#include <cctype>
#include <format>
#include <string_view>

using namespace std::chrono_literals;

TEST("game stop: a stop's length counts from the frame it began")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	CHECK(clock.Advance(t0, false) == 0s);  // playing: no stop
	CHECK(clock.Advance(t0 + 1s, true) == 0s);  // the menu's first frame
	CHECK(clock.Advance(t0 + 1500ms, true) == 500ms);
	CHECK(clock.Advance(t0 + 7s, true) == 6s);
}

TEST("game stop: a frame of play ends it, and the next stop counts afresh")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	clock.Advance(t0, true);
	CHECK(clock.Advance(t0 + 4s, true) == 4s);
	CHECK(clock.Advance(t0 + 4100ms, false) == 0s);  // the menu closed for a frame
	CHECK(clock.Advance(t0 + 4200ms, true) == 0s);   // a new stop: four seconds before it don't count
	CHECK(clock.Advance(t0 + 6200ms, true) == 2s);
}

TEST("game stop: a load resets it")
{
	GameStop::Clock clock;
	const auto      t0 = std::chrono::steady_clock::now();
	clock.Advance(t0, true);
	CHECK(clock.Advance(t0 + 3s, true) == 3s);
	clock.Reset();  // a loading screen
	CHECK(clock.Advance(t0 + 20s, true) == 0s);  // stopped after the load: counted from here, not from before it
	CHECK(clock.Advance(t0 + 21s, true) == 1s);
}

// The UI's menu count is read in GameStop.cpp only: once some menus run in real time (F4Multiplayer-tnl5), a count
// above 0 no longer means a stopped game, and a reader of its own would go on dropping hits and holding our NPCs' states.
namespace
{
	// How often a_code uses a_name as a whole name (itemMenuMode is not menuMode).
	int Uses(std::string_view a_code, std::string_view a_name)
	{
		const auto ident = [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_'; };
		int        found = 0;
		for (auto at = a_code.find(a_name); at != std::string_view::npos; at = a_code.find(a_name, at + 1)) {
			const auto end = at + a_name.size();
			if ((at > 0 && ident(a_code[at - 1])) || (end < a_code.size() && ident(a_code[end]))) {
				continue;
			}
			++found;
		}
		return found;
	}
}

TEST("game stop: the guard finds the menu count read, and not a name in a comment, a string or another name")
{
	using SourceTree::CodeOnly;
	CHECK(Uses(CodeOnly("return ui && ui->menuMode > 0;"), "menuMode") == 1);
	CHECK(Uses(CodeOnly("const auto n = ui->menuMode; // menuMode"), "menuMode") == 1);
	CHECK(Uses(CodeOnly(R"x(REX::INFO("menuMode={}", n); ui->itemMenuMode; GameStop::MenuMode(); /* menuMode */)x"), "menuMode") == 0);
}

TEST("game stop: the plugin reads the UI's menu count in GameStop.cpp only")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	bool seen = false;
	for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
		const auto reads = Uses(code, "menuMode");
		if (file == "src/game/GameStop.cpp") {
			seen = reads > 0;
		} else if (reads) {
			Test::Fail(__FILE__, __LINE__, std::format("{} reads the UI's menu count {} time(s): ask GameStop", file, reads));
		}
	}
	CHECK(seen);
}
