#pragma once

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// What the log says at startup about what the game runs on and how it was started (Platform.cpp gathers the facts):
// Windows or Wine (Proton), the Steam app id the game was given, and whether Steam's overlay was put into it. A player
// on Linux whose F1 or invites do nothing sends this log; these lines tell a maintainer which of the usual causes it
// is. No path is ever in a line (they hold the Windows or Linux user name), only yes/no and numbers. Pure, so it is
// unit tested.
namespace LaunchReport
{
	struct Facts
	{
		std::string wineVersion;  // ntdll's wine_get_version; empty: not Wine
		std::string wineBuild;    // ntdll's wine_get_build_id ("wine-9.0 (Proton-9.0-4)"), when it has one
		std::string hostSystem;   // wine_get_host_version: the system under Wine ("Linux")
		std::string hostRelease;  // and its release (the kernel's version)
		std::string steamAppId;   // the SteamAppId environment variable as the game got it ("" = not set)
		std::string steamGameId;  // SteamGameId
		bool        overlayModule = false;     // GameOverlayRenderer64.dll is in the game already (Windows)
		bool        overlayPreloaded = false;  // LD_PRELOAD names Steam's gameoverlayrenderer (Steam started this Proton game)
		bool        steamCompat = false;       // STEAM_COMPAT_CLIENT_INSTALL_PATH is set: Proton was started by Steam (or umu)
		bool        workingFolderIsGame = true;
	};

	struct Line
	{
		bool        warn = false;
		std::string text;
	};

	// a_value cut to what a log line may hold of a version string or an id: printable characters, no line break,
	// no path (a '/' or '\\' ends it), at most 64 characters.
	[[nodiscard]] inline std::string Clean(std::string_view a_value)
	{
		std::string out;
		for (const char c : a_value) {
			if (c == '/' || c == '\\' || out.size() >= 64) {
				break;
			}
			if (std::isprint(static_cast<unsigned char>(c))) {
				out += c;
			}
		}
		return out;
	}

	// An app id is digits; anything else is shown as "set" (it is not ours to print).
	[[nodiscard]] inline std::string AppId(std::string_view a_value)
	{
		if (a_value.empty()) {
			return "not set";
		}
		const bool digits = a_value.size() <= 20 && std::ranges::all_of(a_value, [](char a_c) { return a_c >= '0' && a_c <= '9'; });
		return digits ? std::string{ a_value } : std::string{ "set" };
	}

	[[nodiscard]] inline bool Wine(const Facts& a_facts)
	{
		return !a_facts.wineVersion.empty();
	}

	[[nodiscard]] inline std::vector<Line> Lines(const Facts& a_facts)
	{
		std::vector<Line> lines;
		if (Wine(a_facts)) {
			const auto build = Clean(a_facts.wineBuild);
			const auto system = Clean(a_facts.hostSystem);
			const auto release = Clean(a_facts.hostRelease);
			lines.push_back({ false, std::format("Platform: Wine {}{} on {}{}", Clean(a_facts.wineVersion), build.empty() ? "" : std::format(" ({})", build),
										 system.empty() ? "an unknown system" : system, release.empty() ? "" : " " + release) });
		} else {
			lines.push_back({ false, "Platform: Windows (not Wine)" });
		}

		std::string overlay;
		if (a_facts.overlayModule) {
			overlay = "Steam's overlay is in the game";
		} else if (a_facts.overlayPreloaded) {
			overlay = "Steam preloaded its overlay";
		} else {
			overlay = Wine(a_facts) ? "Steam did not preload its overlay" : "Steam's overlay is not in the game yet";
		}
		std::string started;
		if (Wine(a_facts)) {
			started = a_facts.steamCompat ? "; Proton was started by Steam (or umu)" : "; Proton was not started by Steam";
		}
		lines.push_back({ false, std::format("Launch: SteamAppId {}, SteamGameId {}; {}{}", AppId(a_facts.steamAppId), AppId(a_facts.steamGameId), overlay, started) });

		// On Wine the overlay comes only with a start through Steam: without it, Shift+Tab, the overlay's Invite to Game
		// and the menu's invite do nothing. Linux's Steam preloads it (LD_PRELOAD); a Windows Steam in the same Wine
		// prefix (CrossOver on a Mac) puts it in as on Windows, so it is in the game already. On Windows it may come in
		// later, so Steam's own answer (logged once Steam is up, and when hosting starts) decides there.
		if (Wine(a_facts) && !a_facts.overlayPreloaded && !a_facts.overlayModule) {
			lines.push_back({ true, "Launch: this game was not started by Steam, so Steam's overlay is not in it: Shift+Tab, Invite to Game and the "
									"F1 menu's invite won't open. Start Fallout 4 from Steam with f4se_loader as its launch command (INSTALL.md, Linux)" });
		}
		if (Wine(a_facts) && a_facts.steamAppId.empty() && a_facts.steamGameId.empty()) {
			lines.push_back({ true, "Launch: the game got no Steam app id (SteamAppId), so Steam may not start in it. Start it through Steam, or with "
									"f4se_loader, which sets one" });
		}
		if (!a_facts.workingFolderIsGame) {
			lines.push_back({ true, "Platform: the game's working folder is not its own folder; the mod reads its ini and texts from the game's folder" });
		}
		return lines;
	}
}
