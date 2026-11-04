#include "game/AutoSave.h"

#include "game/AutoSaveRules.h"
#include "game/DownRules.h"
#include "game/Downed.h"

#include <ShlObj.h>
#undef ERROR  // wingdi.h's, which REX::ERROR is

#include <atomic>

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
		// a_seen, for the log when it is not there: how many settings the collection holds and which have "save" in their key.
		RE::Setting* FindIniSetting(RE::INISettingCollection* a_collection, std::string& a_seen)
		{
			if (!a_collection) {
				a_seen += " (no collection)";
				return nullptr;
			}
			std::vector<std::string_view> keys;
			std::vector<RE::Setting*>     settings;
			std::string                   saves;
			for (auto* setting : a_collection->settings) {
				if (setting) {
					keys.push_back(setting->GetKey());
					settings.push_back(setting);
					auto lower = std::string{ setting->GetKey() };
					std::ranges::transform(lower, lower.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
					if (lower.contains("save")) {
						saves += std::format(" {}", setting->GetKey());
					}
				}
			}
			a_seen += std::format(" {} settings, with save in the key:{}", keys.size(), saves.empty() ? " none" : saves);
			const auto at = AutoSaveRules::FindSetting(keys, AutoSaveRules::TRAVEL_SETTING, AutoSaveRules::TRAVEL_SECTION);
			return at ? settings[*at] : nullptr;
		}

		// The player's Prefs ini: the path the game's collection says it reads (when that file is there), else
		// Documents/My Games/<the game's save folder>/<its file name>.
		std::filesystem::path PrefsIni()
		{
			std::string name = "Fallout4Prefs.ini";
			if (const auto prefs = RE::INIPrefSettingCollection::GetSingleton()) {
				const std::string_view given{ prefs->settingFile, strnlen(prefs->settingFile, sizeof(prefs->settingFile)) };
				const std::filesystem::path path{ given };
				std::error_code             ec;
				if (!given.empty() && path.has_filename() && path.extension() == ".ini") {
					if (std::filesystem::is_regular_file(path, ec)) {
						return path;
					}
					name = path.filename().string();
				}
			}
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				return {};
			}
			std::filesystem::path folder{ docs };
			CoTaskMemFree(docs);
			const auto save = F4SE::GetSaveFolderName();
			return folder / "My Games" / (save.empty() ? "Fallout4"sv : save) / name;
		}

		// "[MAIN] bSaveOnTravel" in the player's Prefs ini file; nullopt when the file or the key cannot be read.
		std::optional<bool> TravelFromPrefsFile(std::string& a_why)
		{
			// On the game thread at a teleport: a read that fails or throws is "not read", never an exception into the frame.
			try {
				const auto    path = PrefsIni();
				std::ifstream file{ path, std::ios::binary };
				if (path.empty() || !file) {
					a_why = path.empty() ? "no Documents folder" : std::format("{} could not be opened", path.filename().string());
					return std::nullopt;
				}
				// A Prefs ini is a few kilobytes; the cap keeps a wrong file (a setting file named as something large) from
				// stalling the frame.
				constexpr std::size_t MAX_BYTES = 1 << 20;
				std::string           text(MAX_BYTES, '\0');
				file.read(text.data(), static_cast<std::streamsize>(text.size()));
				text.resize(static_cast<std::size_t>((std::max)(file.gcount(), std::streamsize{ 0 })));
				const auto value = AutoSaveRules::IniBool(text, AutoSaveRules::TRAVEL_SECTION, AutoSaveRules::TRAVEL_SETTING);
				if (!value) {
					a_why = std::format("{} has no [{}] {} as a yes or no", path.filename().string(), AutoSaveRules::TRAVEL_SECTION, AutoSaveRules::TRAVEL_SETTING);
				}
				return value;
			} catch (...) {
				a_why = "reading Fallout4Prefs.ini threw";
				return std::nullopt;
			}
		}

		// The game's own setting for it, if its collections hold it: the Prefs collection does, under the bare name
		// "bSaveOnTravel" with no section. When they do not, the log says what they did hold (once).
		std::optional<bool> TravelFromGame()
		{
			static const auto found = [] {
				std::string seen = "Save: looking for the game's bSaveOnTravel; Prefs collection:";
				const auto  inPrefs = FindIniSetting(RE::INIPrefSettingCollection::GetSingleton(), seen);
				RE::Setting* setting = inPrefs;
				if (!setting) {
					seen += "; ini collection:";
					setting = FindIniSetting(RE::INISettingCollection::GetSingleton(), seen);
				}
				if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
					REX::INFO("{}; {}", seen, setting ? "it is not a yes or no" : "not in either");
					return static_cast<RE::Setting*>(nullptr);
				}
				REX::INFO("Save: the game's travel autosave setting is {} ({}, in {})", setting->GetBinary() ? "on" : "off", setting->GetKey(),
					inPrefs ? "its Prefs ini" : "its ini");
				return setting;
			}();
			return found ? std::optional<bool>{ found->GetBinary() } : std::nullopt;
		}

		// The player's own setting for travel autosaves (Settings > Gameplay), read at each teleport (cheap, and a change in
		// the Settings screen is followed): from the game's collections when they hold it, else from the Prefs ini file,
		// where Fallout 4 keeps it as "[MAIN] bSaveOnTravel"; in neither, the game's default (on). The log says which.
		bool SaveOnTravel()
		{
			const auto game = TravelFromGame();
			std::string why;
			const auto file = game ? std::nullopt : TravelFromPrefsFile(why);
			const auto travel = AutoSaveRules::ResolveTravel(game, file);
			if (travel.source == AutoSaveRules::Source::kDefault) {
				REX::WARN("Save: the travel autosave setting could not be read ({}); taking the game's default (on)", why);
			} else {
				REX::INFO("Save: the game's travel autosave setting is {}, from {}", travel.on ? "on" : "off", AutoSaveRules::SourceName(travel.source));
			}
			return travel.on;
		}

		// Dead or dying. Not down: the game's IsDead answers true for a player down too (DownRules::Dead).
		bool Dead()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && DownRules::Dead(static_cast<std::int32_t>(static_cast<RE::ActorState&>(*player).lifeState), player->IsDead(false));
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
					REX::INFO("Save: no autosave after the teleport ({})", a_facts.survival ? "Survival" :
						a_facts.dead                                                            ? "the player is dead" :
						a_facts.down                                                            ? "the player is down" :
						                                                                          "the game's travel autosave setting is off");
				}
				break;
			case AutoSaveRules::Asked::kWaiting:
				break;
			}
		}

		// Takes an autosave the engine still holds off its queue before it begins; a_because, for the log. The engine makes
		// one at its next chance, which may come late (in a fight, say): one asked for after a teleport could then come with
		// the player down, and any could come after they died, and the death's load would take that save for the newest.
		// Whoever asked for it (the engine's own too): no autosave is wanted of a player going down, dying or dead.
		bool TakeBack(std::string_view a_because, bool a_log = true)
		{
			std::unique_lock l{ lock };
			const auto       ours = book.Withdraw();
			l.unlock();
			const auto manager = RE::BGSSaveLoadManager::GetSingleton();
			if (!manager) {
				return false;
			}
			constexpr auto AUTOSAVE = static_cast<std::uint32_t>(std::to_underlying(RE::BGSSaveLoadManager::QUEUED_TASK::kAutoSave));
			const auto     held = std::atomic_ref<std::uint32_t>{ manager->queuedTasks }.fetch_and(~AUTOSAVE) & AUTOSAVE;
			if (held != 0 && a_log) {
				REX::INFO("Save: the autosave the game held{} is taken back: {}", ours != Why::kNone ? std::format(" (asked for {})", Reason(ours)) : std::string{},
					a_because);
			}
			return held != 0;
		}

		bool tookBackDead = false;  // Frame told of an autosave taken back in this death already
	}

	void AfterTeleport()
	{
		Ask(Why::kTeleport, { .survival = Survival(), .saveOnTravel = SaveOnTravel(), .down = Downed::IsDown(), .dead = Dead() });
	}

	void OnWentDown(bool a_rule)
	{
		// No travel autosave of a downed player: one held and not begun is taken back. With the host's rule, the autosave
		// on going down is asked for just after (a save made while down loads fine: Downed stands the player up).
		TakeBack("the player went down");
		if (a_rule) {
			Ask(Why::kDown, { .survival = Survival(), .saveOnDown = true, .down = true, .dead = Dead() });
		}
	}

	void OnDying()
	{
		TakeBack("the player bleeds out");
	}

	void Frame()
	{
		// Killed outright as well (revives off, or a lethal hit in the cooldown): no autosave the game holds is made of the
		// dead player: every frame while they are dead, told once a death.
		if (Dead()) {
			if (TakeBack("the player is dead", !tookBackDead)) {
				tookBackDead = true;
			}
		} else {
			tookBackDead = false;
		}
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
		// Nothing stops a save once it begins: told, should the game make one of ours that should have been taken back.
		if (whose != Why::kNone && (Dead() || (whose == Why::kTeleport && Downed::IsDown()))) {
			REX::ERROR("Save: the autosave asked for {} begins with the player {}", Reason(whose), Dead() ? "dead" : "down");
		}
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
