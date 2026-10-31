#include "Test.h"

#include "LaunchReport.h"
#include "game/KeyWatch.h"

#include <algorithm>

using namespace std::chrono_literals;

namespace
{
	bool Has(const std::vector<LaunchReport::Line>& a_lines, std::string_view a_text, bool a_warn)
	{
		return std::ranges::any_of(a_lines, [&](const LaunchReport::Line& a_line) { return a_line.warn == a_warn && a_line.text.find(a_text) != std::string::npos; });
	}

	std::size_t Warnings(const std::vector<LaunchReport::Line>& a_lines)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_lines, [](const LaunchReport::Line& a_line) { return a_line.warn; }));
	}
}

TEST("launch report: Windows started by f4se_loader says Windows and the app id, and warns of nothing")
{
	LaunchReport::Facts facts;
	facts.steamAppId = "377160";
	facts.steamGameId = "377160";
	const auto lines = LaunchReport::Lines(facts);
	CHECK(Has(lines, "Platform: Windows (not Wine)", false));
	CHECK(Has(lines, "SteamAppId 377160, SteamGameId 377160", false));
	// The overlay may come in after the plugin loads on Windows: Steam's own answer decides there, later.
	CHECK(Has(lines, "overlay is not in the game yet", false));
	CHECK(Warnings(lines) == 0);
}

TEST("launch report: Proton started by Steam names Wine, its build and the system, sees the preloaded overlay, and warns of nothing")
{
	LaunchReport::Facts facts;
	facts.wineVersion = "9.0";
	facts.wineBuild = "wine-9.0 (Proton-9.0-4)";
	facts.hostSystem = "Linux";
	facts.hostRelease = "6.11.5-300.fc41.x86_64";
	facts.steamAppId = "377160";
	facts.steamGameId = "377160";
	facts.overlayPreloaded = true;
	facts.steamCompat = true;
	const auto lines = LaunchReport::Lines(facts);
	CHECK(Has(lines, "Platform: Wine 9.0 (wine-9.0 (Proton-9.0-4)) on Linux 6.11.5-300.fc41.x86_64", false));
	CHECK(Has(lines, "Steam preloaded its overlay; Proton was started by Steam", false));
	CHECK(Warnings(lines) == 0);
}

TEST("launch report: Proton started by a mod manager, not Steam, warns that the overlay and its invites are missing")
{
	LaunchReport::Facts facts;
	facts.wineVersion = "9.0";
	facts.hostSystem = "Linux";
	facts.steamAppId = "377160";
	const auto lines = LaunchReport::Lines(facts);
	CHECK(Has(lines, "Steam did not preload its overlay; Proton was not started by Steam", false));
	CHECK(Has(lines, "this game was not started by Steam, so Steam's overlay is not in it", true));
	CHECK(Has(lines, "SteamGameId not set", false));
	CHECK(Warnings(lines) == 1);
}

TEST("launch report: a Windows Steam in the same Wine prefix (CrossOver) put its overlay in the game, and nothing is warned of")
{
	LaunchReport::Facts facts;
	facts.wineVersion = "11.17";
	facts.hostSystem = "Darwin";
	facts.steamAppId = "377160";
	facts.steamGameId = "377160";
	facts.overlayModule = true;
	const auto lines = LaunchReport::Lines(facts);
	CHECK(Has(lines, "Steam's overlay is in the game", false));
	CHECK(Warnings(lines) == 0);
}

TEST("launch report: Wine with no app id at all, and another working folder, warns of both")
{
	LaunchReport::Facts facts;
	facts.wineVersion = "8.0";
	facts.overlayPreloaded = true;
	facts.workingFolderIsGame = false;
	const auto lines = LaunchReport::Lines(facts);
	CHECK(Has(lines, "on an unknown system", false));
	CHECK(Has(lines, "got no Steam app id", true));
	CHECK(Has(lines, "working folder is not its own folder", true));
	CHECK(Warnings(lines) == 2);
}

TEST("launch report: no line holds a path or a line break, nor an app id that isn't digits")
{
	LaunchReport::Facts facts;
	facts.wineVersion = "9.0\nPlatform: forged";
	facts.wineBuild = "/home/alice/.steam/proton";
	facts.hostSystem = "Linux";
	facts.hostRelease = "C:\\Users\\alice";
	facts.steamAppId = "/home/alice";
	const auto lines = LaunchReport::Lines(facts);
	for (const auto& line : lines) {
		CHECK(line.text.find('\n') == std::string::npos);
		CHECK(line.text.find("alice") == std::string::npos);
		CHECK(line.text.find("/home") == std::string::npos);
	}
	CHECK(Has(lines, "SteamAppId set,", false));
	CHECK(LaunchReport::Clean(std::string(200, 'x')).size() == 64);
}

TEST("key watch: a player who walks is never warned; one whose keys never arrive is warned once after three minutes of play, and told when a key comes after all")
{
	using KeyWatch::Event;
	const auto start = KeyWatch::Clock::time_point{} + 1h;

	// Walking within the first minute: nothing, ever.
	{
		KeyWatch::Watch watch;
		auto            now = start;
		bool            any = false;
		for (int frame = 0; frame < 60 * 60 * 10; ++frame, now += 16ms) {
			any |= watch.Tick(now, true, frame == 3000) != Event::kNone;
		}
		CHECK(!any);
		CHECK(watch.Seen());
	}

	// Nothing seen: one warning at three minutes of play, not counting menus or long gaps (a load).
	{
		KeyWatch::Watch watch;
		auto            now = start;
		int             warnings = 0;
		auto            warnedAt = start;
		for (int frame = 0; frame < 60 * 60 * 8; ++frame) {
			now += 16ms;
			if (frame == 100) {
				now += 10min;  // a long load: counts one second at most
			}
			const bool inPlay = frame % 2 == 0;  // half the frames in a menu
			if (watch.Tick(now, inPlay, false) == Event::kNothingSeen) {
				++warnings;
				warnedAt = now;
			}
		}
		CHECK(warnings == 1);
		// Half the frames in play: three minutes of play take about six, after the ten-minute load (one second of it).
		CHECK(warnedAt - start > 15min);
		CHECK(warnedAt - start < 17min);
		CHECK(watch.Tick(now + 16ms, true, true) == Event::kSeenAfterAll);
		CHECK(watch.Tick(now + 32ms, true, true) == Event::kNone);
	}

	// Never in play (the main menu all along): never warned.
	{
		KeyWatch::Watch watch;
		auto            now = start;
		bool            any = false;
		for (int frame = 0; frame < 60 * 60 * 10; ++frame, now += 16ms) {
			any |= watch.Tick(now, false, false) != Event::kNone;
		}
		CHECK(!any);
	}
}
