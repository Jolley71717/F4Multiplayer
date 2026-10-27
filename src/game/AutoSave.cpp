#include "game/AutoSave.h"

#include "game/AutoSaveRules.h"

namespace AutoSave
{
	namespace
	{
		using Clock = AutoSaveRules::Clock;
		using Why = AutoSaveRules::Why;

		// The load's message comes on its own thread; the rest on the main thread.
		std::mutex                       lock;
		AutoSaveRules::Book              book;
		std::optional<Clock::time_point> saveBegan;  // OnPreSave, for how long the save took

		// The setting in a_collection (the Prefs ini's or the game's ini's), by its name in whatever section it is in.
		RE::Setting* FindIniSetting(RE::INISettingCollection* a_collection)
		{
			if (!a_collection) {
				return nullptr;
			}
			std::vector<std::string_view> keys;
			std::vector<RE::Setting*>     settings;
			for (auto* setting : a_collection->settings) {
				if (setting) {
					keys.push_back(setting->GetKey());
					settings.push_back(setting);
				}
			}
			const auto at = AutoSaveRules::FindSetting(keys, AutoSaveRules::TRAVEL_SETTING, AutoSaveRules::TRAVEL_SECTION);
			return at ? settings[*at] : nullptr;
		}

		// The player's own setting for travel autosaves (Settings > Gameplay), from their Prefs ini, or the game's ini when
		// it is not there. Fallout4Prefs.ini keeps it as "[MAIN] bSaveOnTravel", so it is found by its name (the game keys
		// it "bSaveOnTravel:MAIN"). A setting that is in neither ini is the game's default, on, and the log says it was
		// missing rather than off.
		bool SaveOnTravel()
		{
			static RE::Setting* const found = [] {
				const auto inPrefs = FindIniSetting(RE::INIPrefSettingCollection::GetSingleton());
				const auto setting = inPrefs ? inPrefs : FindIniSetting(RE::INISettingCollection::GetSingleton());
				if (!setting) {
					REX::WARN("Save: the game's {} setting was not found in its Prefs ini or its ini; taking its default ({})", AutoSaveRules::TRAVEL_SETTING,
						AutoSaveRules::TRAVEL_DEFAULT ? "on" : "off");
					return static_cast<RE::Setting*>(nullptr);
				}
				if (setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
					REX::WARN("Save: the game's {} setting is not a yes or no (type {}); taking its default ({})", setting->GetKey(), static_cast<int>(setting->GetType()),
						AutoSaveRules::TRAVEL_DEFAULT ? "on" : "off");
					return static_cast<RE::Setting*>(nullptr);
				}
				REX::INFO("Save: the game's travel autosave setting is {} ({}, in {})", setting->GetBinary() ? "on" : "off", setting->GetKey(),
					inPrefs ? "its Prefs ini" : "its ini");
				return setting;
			}();
			return found ? found->GetBinary() : AutoSaveRules::TRAVEL_DEFAULT;
		}

		bool Survival()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetDifficultyLevel() >= RE::DifficultyLevel::kSurvival;
		}

		std::string_view Reason(Why a_why)
		{
			return a_why == Why::kDown ? "on going down" : "after the teleport";
		}

		// Asks the game for an autosave, as a load door or a bed would: the engine names it (its Autosave rotation) and
		// makes it at its next chance, through the same kPreSaveGame work as any save.
		void Ask(Why a_why, const AutoSaveRules::Facts& a_facts)
		{
			const auto manager = RE::BGSSaveLoadManager::GetSingleton();
			if (!manager) {
				return;
			}
			const auto now = Clock::now();
			std::unique_lock l{ lock };
			const auto asked = book.Ask(a_why, a_facts, now);
			const auto since = book.SinceSave(now);
			l.unlock();
			switch (asked) {
			case AutoSaveRules::Asked::kYes:
				REX::INFO("Save: asking the game for an autosave {}", Reason(a_why));
				manager->QueueSaveLoadTask(RE::BGSSaveLoadManager::QUEUED_TASK::kAutoSave);
				break;
			case AutoSaveRules::Asked::kTooSoon:
				REX::INFO("Save: no autosave {}: the last save began {} s ago", Reason(a_why),
					since ? std::chrono::duration_cast<std::chrono::seconds>(*since).count() : 0);
				break;
			case AutoSaveRules::Asked::kNotWanted:
				if (a_why == Why::kTeleport) {
					REX::INFO("Save: no autosave after the teleport ({})", a_facts.survival ? "Survival" : "the game's travel autosave setting is off");
				}
				break;
			case AutoSaveRules::Asked::kWaiting:
				break;
			}
		}
	}

	void AfterTeleport()
	{
		Ask(Why::kTeleport, { .survival = Survival(), .saveOnTravel = SaveOnTravel() });
	}

	void OnWentDown(bool a_rule)
	{
		if (a_rule) {
			Ask(Why::kDown, { .survival = Survival(), .saveOnDown = true });
		}
	}

	void Frame()
	{
		std::unique_lock l{ lock };
		const auto       missed = book.Unanswered(Clock::now());
		l.unlock();
		if (missed != Why::kNone) {
			REX::WARN("Save: the game made no autosave in {} s for the one asked for {}", std::chrono::duration_cast<std::chrono::seconds>(AutoSaveRules::ANSWER_TIME).count(),
				Reason(missed));
		}
	}

	void OnPreSave(std::string_view a_name)
	{
		const auto now = Clock::now();
		std::unique_lock l{ lock };
		const auto       whose = book.Began(now);
		saveBegan = now;
		l.unlock();
		REX::INFO("Save: {} begins ({})", AutoSaveRules::Label(a_name), AutoSaveRules::WhyName(whose));
	}

	void OnPostSave(std::string_view a_name)
	{
		std::unique_lock l{ lock };
		const auto       began = std::exchange(saveBegan, std::nullopt);
		l.unlock();
		const auto ms = began ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *began).count() : -1;
		REX::INFO("Save: {} written ({} ms)", AutoSaveRules::Label(a_name), ms);
	}

	void OnPreLoad(std::string_view a_name)
	{
		// A death loads the newest save: this line says which one it was.
		REX::INFO("Save: loading {}", AutoSaveRules::Label(a_name));
	}

	std::string Describe()
	{
		std::scoped_lock l{ lock };
		return std::format("autosaves: asked={} made={} unanswered={} tooSoon={}", book.asks, book.made, book.unanswered, book.tooSoon);
	}
}
