#pragma once

#include "Config.h"
#include "SessionLevel.h"
#include "StorySetting.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// Reading the settings file's text into Config::Settings, with no game or log behind it, so the tests read the
// defaults the plugin carries the way the plugin does (tests/ConfigReadTests.cpp). Config::Load is this on the
// player's file, with what it says going to the log.
namespace Config
{
	// Keys whose line in dist/F4Multiplayer.ini does not mean what leaving the key out means, with the value that
	// does. A new install gets the file's line; a player's file that lacks the key gets this one appended instead
	// (IniFile::AddMissing), so an update never changes what the player's file meant. sPlayerName: the file's is
	// "character" (the character's name); a file without the key shows the Steam name, which is what empty says.
	// The tests hold this list to every key of the file: one that reads unlike a missing key and is not here fails.
	inline constexpr std::pair<std::string_view, std::string_view> WHEN_ABSENT[] = {
		{ "sPlayerName", "" },
	};

	// What Read says of a line: a_warn for a value it could not use, otherwise a note.
	using Say = std::function<void(bool a_warn, const std::string& a_text)>;

	namespace detail
	{
		inline std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r");
			return a_str.substr(first, last - first + 1);
		}

		inline bool ParseBool(std::string_view a_value)
		{
			return a_value == "1" || a_value == "true" || a_value == "True" || a_value == "TRUE";
		}

		template <class T>
		void ParseInt(std::string_view a_value, T& a_out, int a_base = 10)
		{
			if (a_base == 16 && (a_value.starts_with("0x") || a_value.starts_with("0X"))) {
				a_value.remove_prefix(2);
			}
			T value{};
			const auto [ptr, ec] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value, a_base);
			if (ec == std::errc{} && ptr == a_value.data() + a_value.size()) {
				a_out = value;
			}
		}

		// A percent key with a ceiling (iCatchUpXp, iGroupDifficulty): past it the ceiling, said; not a number, the
		// value it had.
		inline std::uint8_t ParsePercent(const Say& a_say, std::string_view a_key, std::string_view a_value, std::uint8_t a_max, std::uint8_t a_current)
		{
			std::uint32_t parsed = a_current;
			ParseInt(a_value, parsed);
			if (parsed > a_max) {
				a_say(true, std::format("Config: {} = {} is past {}; it is {}", a_key, parsed, a_max, a_max));
				return a_max;
			}
			return static_cast<std::uint8_t>(parsed);
		}
	}

	// Reads a_text ("key = value" lines; '#' or ';' start a comment) into a_settings: a key it has sets its setting,
	// the last line of a key counting; a key it lacks leaves the setting as a_settings has it. A leading UTF-8 byte
	// order mark (Notepad's, or the shipped file's of old) is ignored.
	inline void Read(std::string_view a_text, Settings& a_settings, const Say& a_say)
	{
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);
		}
		std::istringstream file{ std::string{ a_text } };

		std::string line;
		while (std::getline(file, line)) {
			auto view = detail::Trim(line);
			if (view.empty() || view.front() == '#' || view.front() == ';' || view.front() == '[') {
				continue;
			}

			const auto eq = view.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}

			const auto key = detail::Trim(view.substr(0, eq));
			const auto value = detail::Trim(view.substr(eq + 1));

			if (key == "sPlayerName") {
				a_settings.playerName = value;
			} else if (key == "sLanguage") {
				a_settings.language = value;
			} else if (key == "bHost") {
				a_settings.host = detail::ParseBool(value);
			} else if (key == "sTransport") {
				a_settings.transport = value;
			} else if (key == "sServerAddress") {
				a_settings.serverAddress = value;
			} else if (key == "iPort") {
				detail::ParseInt(value, a_settings.port);
			} else if (key == "iMaxPlayers") {
				detail::ParseInt(value, a_settings.maxPlayers);
			} else if (key == "sPassword") {
				a_settings.password = value;
			} else if (key == "bFriendlyFire") {
				a_settings.friendlyFire = detail::ParseBool(value);
			} else if (key == "sStory") {
				if (const auto shared = StorySetting::Parse(value)) {
					a_settings.sharedStory = *shared;
				} else {
					a_say(true, std::format("Config: sStory = '{}' is neither shared nor own; the story stays {}", value, StorySetting::Name(a_settings.sharedStory)));
				}
			} else if (key == "sQuests") {
				if (const auto safe = StorySetting::ParseQuests(value)) {
					a_settings.safeQuests = *safe;
				} else {
					a_say(true, std::format("Config: sQuests = '{}' is neither story nor safe; it stays {}", value, StorySetting::QuestsName(a_settings.safeQuests)));
				}
			} else if (key == "sIgnoredPlugins") {
				a_settings.ignoredPlugins.clear();
				for (std::size_t start = 0; start <= value.size();) {
					const auto comma = value.find(',', start);
					auto name = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
					const auto first = name.find_first_not_of(" 	");
					const auto last = name.find_last_not_of(" 	");
					if (first != std::string::npos) {
						a_settings.ignoredPlugins.emplace_back(name.substr(first, last - first + 1));
					}
					if (comma == std::string::npos) {
						break;
					}
					start = comma + 1;
				}
			} else if (key == "iMyAppearance") {
				detail::ParseInt(value, a_settings.myAppearance, 16);
			} else if (key == "iPuppetBaseForm") {
				detail::ParseInt(value, a_settings.puppetBaseForm, 16);
			} else if (key == "bFriendFaces") {
				a_settings.friendFaces = detail::ParseBool(value);
			} else if (key == "bGrenadeInHand") {
				a_settings.grenadeInHand = detail::ParseBool(value);
			} else if (key == "bFriendShotTrails") {
				a_settings.friendShotTrails = detail::ParseBool(value);
			} else if (key == "bSyncQuests") {
				a_settings.syncQuests = detail::ParseBool(value);
			} else if (key == "bSharedRadiants") {
				a_settings.sharedRadiants = detail::ParseBool(value);
			} else if (key == "bSharedAttacks") {
				a_settings.sharedAttacks = detail::ParseBool(value);
			} else if (key == "bRevive") {
				a_settings.revive = detail::ParseBool(value);
			} else if (key == "iBleedOutSeconds") {
				detail::ParseInt(value, a_settings.bleedOutSeconds);
			} else if (key == "fReviveSeconds") {
				float seconds = a_settings.reviveSeconds;
				const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
				if (ec == std::errc{} && std::isfinite(seconds)) {
					a_settings.reviveSeconds = std::clamp(seconds, 0.5f, 30.0f);
				}
			} else if (key == "iReviveHealth") {
				detail::ParseInt(value, a_settings.reviveHealth);
			} else if (key == "iMaxRevives") {
				detail::ParseInt(value, a_settings.maxRevives);
			} else if (key == "iReviveWindowMinutes") {
				detail::ParseInt(value, a_settings.reviveWindowMinutes);
			} else if (key == "iReviveCooldownSeconds") {
				detail::ParseInt(value, a_settings.reviveCooldownSeconds);
			} else if (key == "bReviveOutOfCombat") {
				a_settings.reviveOutOfCombat = detail::ParseBool(value);
			} else if (key == "bDownedCrawl") {
				a_settings.downedCrawl = detail::ParseBool(value);
			} else if (key == "bRealTimeVats") {
				a_settings.realTimeVats = detail::ParseBool(value);
			} else if (key == "bGroupVatsSlowdown") {
				a_settings.groupVatsSlowdown = detail::ParseBool(value);
			} else if (key == "fGroupVatsSpeed") {
				float speed = a_settings.groupVatsSpeed;
				const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), speed);
				if (ec == std::errc{} && std::isfinite(speed)) {
					a_settings.groupVatsSpeed = std::clamp(speed, 0.05f, 1.0f);
				}
			} else if (key == "bShareGuestBuilds") {
				a_settings.shareGuestBuilds = detail::ParseBool(value);
			} else if (key == "bSharedStorage") {
				a_settings.sharedStorage = detail::ParseBool(value);
			} else if (key == "bSharedWorkshop") {
				a_settings.sharedWorkshop = detail::ParseBool(value);
			} else if (key == "bSharedWorkshopEdit") {
				a_settings.sharedWorkshopEdit = detail::ParseBool(value);
			} else if (key == "bContainerGifts") {
				a_settings.containerGifts = detail::ParseBool(value);
			} else if (key == "bSaveOnDown") {
				a_settings.saveOnDown = detail::ParseBool(value);
			} else if (key == "sNpcLevel") {
				if (const auto rule = SessionLevel::Parse(value)) {
					a_settings.npcLevel = static_cast<std::uint8_t>(*rule);
				} else {
					a_say(true, std::format("Config: sNpcLevel = '{}' is none of average, highest, host or own; it stays {}", value, SessionLevel::Name(static_cast<Protocol::NpcLevelRule>(a_settings.npcLevel))));
				}
			} else if (key == "sLegendaryDifficulty") {
				if (const auto rule = SessionLevel::ParseLegendary(value)) {
					a_settings.legendary = static_cast<std::uint8_t>(*rule);
				} else {
					a_say(true, std::format("Config: sLegendaryDifficulty = '{}' is none of average, host or own; it stays {}", value, SessionLevel::Name(static_cast<Protocol::LegendaryRule>(a_settings.legendary))));
				}
			} else if (key == "iCatchUpXp") {
				a_settings.catchUpXp = detail::ParsePercent(a_say, key, value, Protocol::SessionRules::MAX_CATCH_UP_XP, a_settings.catchUpXp);
			} else if (key == "iGroupDifficulty") {
				a_settings.groupDifficulty = detail::ParsePercent(a_say, key, value, Protocol::SessionRules::MAX_GROUP_DIFFICULTY, a_settings.groupDifficulty);
			} else if (key == "bSharedRolls") {
				a_settings.sharedRolls = detail::ParseBool(value);
			} else if (key == "bHideTutorials") {
				a_settings.hideTutorials = detail::ParseBool(value);
			} else if (key == "bShareMap") {
				a_settings.shareMap = detail::ParseBool(value);
			} else if (key == "bVatsSkipsFriends") {
				a_settings.vatsSkipsFriends = detail::ParseBool(value);
			} else if (key == "bLendPowerArmor") {
				a_settings.lendPowerArmor = detail::ParseBool(value);
			} else if (key == "bFastLoading") {
				a_settings.fastLoading = detail::ParseBool(value);
			} else if (key == "iFastLoadingSeconds") {
				detail::ParseInt(value, a_settings.fastLoadingSeconds);
			} else if (key == "bRunInBackground") {
				a_settings.runInBackground = detail::ParseBool(value);
			} else if (key == "bCompassMarkers") {
				a_settings.compassMarkers = detail::ParseBool(value);
			} else if (key == "bMenuIcons") {
				a_settings.menuIcons = detail::ParseBool(value);
			} else if (key == "bMenuBadges") {
				a_settings.menuBadges = detail::ParseBool(value);
			} else if (key == "bFriendsOnMap") {
				a_settings.friendsOnMap = detail::ParseBool(value);
			} else if (key == "bFriendsOnMapNames") {
				a_settings.friendsOnMapNames = detail::ParseBool(value);
			} else if (key == "bSyncTime") {
				a_settings.syncTime = detail::ParseBool(value);
			} else if (key == "bSaveSession") {
				a_settings.saveSession = detail::ParseBool(value);
			} else if (key == "bSpokenLines") {
				a_settings.spokenLines = detail::ParseBool(value);
			} else if (key == "bVoiceLines" || key == "bExperimentalVoiceLines") {
				// Not read any more: they asked for the console's Say on our copy of the NPC, which could leave
				// the player held in dialogue. A player's ini is kept across updates (the in-game menu writes to
				// it), and the one from 0.8.6 says bExperimentalVoiceLines = false: read, it would keep the new
				// way of hearing lines off for everyone who updated.
				a_say(false, std::format("Config: {} is no longer read; friends' lines are heard unless bSpokenLines = false", key));
			} else if (key == "fXpShare") {
				float share = a_settings.xpShare;
				const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), share);
				if (ec == std::errc{} && std::isfinite(share)) {
					a_settings.xpShare = std::clamp(share, 0.0f, 1.0f);
				}
			} else if (key == "bVoiceChat") {
				a_settings.voiceChat = detail::ParseBool(value);
			} else if (key == "bVoiceOpenMic") {
				a_settings.voiceOpenMic = detail::ParseBool(value);
			} else if (key == "iKeyVoice") {
				detail::ParseInt(value, a_settings.keyVoice, 16);
			} else if (key == "fVoiceRange") {
				float range = a_settings.voiceRange;
				const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), range);
				if (ec == std::errc{} && std::isfinite(range)) {
					a_settings.voiceRange = std::clamp(range, 0.0f, 100000.0f);
				}
			} else if (key == "iKeyMenu") {
				detail::ParseInt(value, a_settings.keyMenu, 16);
			} else if (key == "iKeyPlayerList") {
				detail::ParseInt(value, a_settings.keyPlayerList, 16);
			} else if (key == "iKeyTeleport") {
				detail::ParseInt(value, a_settings.keyTeleport, 16);
			} else if (key == "iKeyPing") {
				detail::ParseInt(value, a_settings.keyPing, 16);
			} else if (key == "iKeyWhere") {
				detail::ParseInt(value, a_settings.keyWhere, 16);
			} else if (key == "iKeyEmotePoint") {
				detail::ParseInt(value, a_settings.keyEmotePoint, 16);
			} else if (key == "iKeyEmoteCheer") {
				detail::ParseInt(value, a_settings.keyEmoteCheer, 16);
			} else if (key == "iKeyEmoteClap") {
				detail::ParseInt(value, a_settings.keyEmoteClap, 16);
			} else if (key == "iHangSeconds") {
				detail::ParseInt(value, a_settings.hangSeconds);
			} else if (key == "bHangDump") {
				a_settings.hangDump = detail::ParseBool(value);
			} else if (key == "bCrashReport") {
				a_settings.crashReport = detail::ParseBool(value);
			} else if (key == "sLogLevel") {
				a_settings.logLevel = value;
			} else if (key == "bDevChannel") {
				a_settings.devChannel = detail::ParseBool(value);
			} else if (key == "iDevChannelPort") {
				detail::ParseInt(value, a_settings.devChannelPort);
			} else {
				a_say(true, std::format("Config: unknown key '{}'", key));
			}
		}
	}
}
