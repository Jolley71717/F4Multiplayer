#pragma once

#include "Config.h"

#include <array>

// The switches of the multiplayer menu's two settings pages: which they are, and which page each is on. Settings holds the
// player's own (they change that player's game); Session options, a row of Settings, holds the rules of a session the player
// hosts (sent to everyone who joins, from the next session hosted). Kept free of the game so it can be tested.
namespace UiSettingsRows
{
	// A setting the menu toggles: shown in the body and on its button, applied to the running game at once and written to
	// the ini.
	struct Toggle
	{
		const char* name;  // the key of its name
		const char* what;  // the key of what "On" means
		bool Config::Settings::*value;
		const char* key;
		const char* onValue;
		const char* offValue;
		bool        host;  // a rule of the session hosted, on Session options; else the player's own, on Settings
	};
	inline constexpr std::array<Toggle, 9> TOGGLES{ {
		{ "menu.friendly_fire", "menu.friendly_fire_what", &Config::Settings::friendlyFire, "bFriendlyFire", "true", "false", true },
		{ "menu.shared_story", "menu.shared_story_what", &Config::Settings::sharedStory, "sStory", "shared", "own", true },
		{ "menu.safe_quests", "menu.safe_quests_what", &Config::Settings::safeQuests, "sQuests", "safe", "story", true },
		{ "menu.voice_chat", "menu.voice_chat_what", &Config::Settings::voiceChat, "bVoiceChat", "true", "false", false },
		{ "menu.spoken_lines", "menu.spoken_lines_what", &Config::Settings::spokenLines, "bSpokenLines", "true", "false", false },
		{ "menu.compass_markers", "menu.compass_markers_what", &Config::Settings::compassMarkers, "bCompassMarkers", "true", "false", false },
		{ "menu.developer_hud", "menu.developer_hud_what", &Config::Settings::developerHud, "bDeveloperHud", "true", "false", false },
		{ "menu.fast_loading", "menu.fast_loading_what", &Config::Settings::fastLoading, "bFastLoading", "true", "false", false },
		{ "menu.lend_power_armor", "menu.lend_power_armor_what", &Config::Settings::lendPowerArmor, "bLendPowerArmor", "true", "false", false },
	} };

	// How many switches a page has (a_host: Session options).
	[[nodiscard]] constexpr std::size_t Count(bool a_host)
	{
		std::size_t count = 0;
		for (const auto& toggle : TOGGLES) {
			count += toggle.host == a_host ? 1 : 0;
		}
		return count;
	}
}
