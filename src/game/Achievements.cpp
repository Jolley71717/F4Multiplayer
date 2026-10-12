#include "game/Achievements.h"

#include "game/WorldSync.h"
#include "steam/SteamApi.h"

#include <bitset>

namespace Achievements
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The game names its Steam achievements NEW_ACHIEVEMENT_<n>_0 (the format string is in Fallout4.exe).
		// The game and its add-ons have fewer than this; the log says how many Steam knows.
		constexpr int MAX_ID = 100;
		using Unlocked = std::bitset<MAX_ID + 1>;

		// A stage's script runs a moment after the stage is set. Steam is asked this long after the last
		// stage of a run, and no later than CHECK_BY after its first.
		constexpr auto CHECK_AFTER = 3s;
		constexpr auto CHECK_BY = 10s;

		Unlocked                         unlocked;
		bool                             known = false;  // unlocked holds what Steam said
		bool                             loadCheck = false;
		std::optional<bool>              eligible;
		std::optional<Clock::time_point> checkAt;
		std::optional<Clock::time_point> checkBy;
		std::uint32_t                    lastQuest = 0;
		std::uint16_t                    lastStage = 0;
		std::uint32_t                    stagesSet = 0;
		std::uint32_t                    checks = 0;
		std::uint32_t                    newlyUnlocked = 0;  // seen unlocking while this game runs

		// What Steam says this player has unlocked, and how many achievements it knows; nothing if it
		// can't say (no Steam, or this player's stats haven't arrived yet).
		std::optional<std::pair<Unlocked, int>> ReadSteam()
		{
			const auto api = SteamApi::Get();
			if (!api || !api->userStats || !api->GetAchievementAndUnlockTime) {
				return std::nullopt;
			}
			Unlocked out;
			int      count = 0;
			for (int id = 0; id <= MAX_ID; ++id) {
				const auto    name = std::format("NEW_ACHIEVEMENT_{}_0", id);
				bool          achieved = false;
				std::uint32_t unlockTime = 0;
				if (api->GetAchievementAndUnlockTime(api->userStats, name.c_str(), &achieved, &unlockTime)) {
					++count;
					out[id] = achieved;
				}
			}
			if (count == 0) {
				return std::nullopt;
			}
			return std::pair{ out, count };
		}

		// The check Game.AddAchievement makes before it awards one: TESDataHandler::CheckModsLoaded(true)
		// (ID 2192323, +2E8F90) is true with a plugin loaded that is neither one of the game's eight
		// masters nor a Creation it knows, or with the save marked as ever modded (PlayerCharacter::
		// everModded, +E02 bit 6), and then the game awards nothing. It only reads.
		void CheckEligible()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!handler || !player) {
				return;
			}
			const bool plugins = handler->CheckModsLoaded(false);
			eligible = !handler->CheckModsLoaded(true);
			REX::INFO("Achievements: eligible={} (mod plugins={}, save marked modded={})", *eligible ? "yes" : "no", plugins ? "yes" : "no",
				player->everModded ? "yes" : "no");
		}

		void Check(bool a_load)
		{
			++checks;
			const auto steam = ReadSteam();
			if (!steam) {
				if (a_load) {
					REX::INFO("Achievements: Steam has no achievements for this player yet");
				}
				return;
			}
			const auto& [now, count] = *steam;
			if (known) {
				const auto last = lastQuest ? std::format("{:08X} stage {}", lastQuest, lastStage) : std::string("none");
				for (int id = 0; id <= MAX_ID; ++id) {
					if (now[id] && !unlocked[id]) {
						++newlyUnlocked;
						REX::INFO("Achievements: NEW_ACHIEVEMENT_{}_0 unlocked (last stage set from a friend: {})", id, last);
					} else if (!now[id] && unlocked[id]) {
						REX::INFO("Achievements: NEW_ACHIEVEMENT_{}_0 no longer unlocked", id);
					}
				}
			}
			if (a_load || !known) {
				REX::INFO("Achievements: Steam: {} of {} unlocked", now.count(), count);
			}
			unlocked = now;
			known = true;
		}
	}

	void OnGameLoaded()
	{
		loadCheck = true;
		checkAt.reset();
		checkBy.reset();
	}

	void AfterStageSet(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		lastQuest = a_quest;
		lastStage = a_stage;
		++stagesSet;
		const auto now = Clock::now();
		checkAt = now + CHECK_AFTER;
		if (!checkBy) {
			checkBy = now + CHECK_BY;
		}
	}

	void Frame()
	{
		if (!WorldSync::InWorld()) {
			return;
		}
		if (loadCheck) {
			loadCheck = false;
			CheckEligible();
			Check(true);
			return;
		}
		const auto now = Clock::now();
		if (checkAt && (now >= *checkAt || now >= *checkBy)) {
			checkAt.reset();
			checkBy.reset();
			Check(false);
		}
	}

	std::string Describe()
	{
		return std::format("achievements: eligible={} unlocked={} new={} checks={} afterStages={}", eligible ? (*eligible ? "yes" : "no") : "?",
			known ? static_cast<int>(unlocked.count()) : -1, newlyUnlocked, checks, stagesSet);
	}
}
