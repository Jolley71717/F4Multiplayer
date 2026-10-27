#include "game/AutoSave.h"

#include "game/AutoSaveRules.h"
#include "game/DownRules.h"
#include "game/Downed.h"

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

		// The player's own setting for travel autosaves (Settings > Gameplay, "[SaveGame] bSaveOnTravel"), from their
		// Prefs ini, or the game's ini when it is not there.
		bool SaveOnTravel()
		{
			static RE::Setting* const found = [] {
				const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
				const auto inPrefs = prefs ? prefs->GetSetting("bSaveOnTravel:SaveGame") : nullptr;
				const auto setting = inPrefs ? inPrefs : RE::GetINISetting("bSaveOnTravel:SaveGame");
				if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
					REX::WARN("Save: the game's bSaveOnTravel setting was not found; no autosave after a teleport");
					return static_cast<RE::Setting*>(nullptr);
				}
				return setting;
			}();
			return found && found->GetBinary();
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
