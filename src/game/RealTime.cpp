#include "game/RealTime.h"

#include "Background.h"
#include "Config.h"
#include "game/GameStop.h"
#include "game/HostRules.h"
#include "game/OpenMenus.h"
#include "game/RealTimeVats.h"

namespace RealTime
{
	namespace
	{
		// bHideTutorials: the game's bShowTutorials (Interface), read each time it would show a help message.
		RE::Setting* ShowTutorials()
		{
			static RE::Setting* const found = [] {
				const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
				const auto inPrefs = prefs ? prefs->GetSetting("bShowTutorials:Interface") : nullptr;
				const auto setting = inPrefs ? inPrefs : RE::GetINISetting("bShowTutorials:Interface");
				if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
					REX::WARN("RealTime: the game's bShowTutorials setting was not found; its hints show in a session too");
					return static_cast<RE::Setting*>(nullptr);
				}
				return setting;
			}();
			return found;
		}

		Background::Switch tutorials{ false };

		void HideTutorials(bool a_session)
		{
			const auto setting = ShowTutorials();
			if (!setting) {
				return;
			}
			if (const auto value = tutorials.Update(Config::Get().hideTutorials, a_session, setting->GetBinary())) {
				setting->SetBinary(*value);
				REX::INFO("RealTime: {}", *value ? "out of the session, the game's tutorial hints are back" : "in a session, the game's tutorial hints are hidden (bHideTutorials)");
			}
		}

		RealTimeVats::Clock vatsClock;
		std::uint64_t       vatsResets = 0;
		bool                vatsWasUp = false;

		// bRealTimeVats: the slow motion of VATS set back to normal while it is up.
		void Vats()
		{
			const auto vats = RE::VATS::GetSingleton();
			const auto timer = RE::BSTimer::GetSingleton();
			if (!vats || !timer) {
				return;
			}
			const bool  vatsUp = OpenMenus::Open(OpenMenus::Menu::kVats) || vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback);
			const float set = vatsClock.Frame(HostRules::InSession(), HostRules::Get().realTimeVats, vatsUp, RE::BSTimer::QGlobalTimeMultiplier(), vats->magicTimeSlowdown);
			if (vatsUp && !vatsWasUp && vatsClock.SlowdownBeforeVats() > 0.0f && HostRules::InSession() && HostRules::Get().realTimeVats) {
				REX::INFO("RealTime: VATS up on a slowdown of {:.2f} (Jet or the like): left slowed", vatsClock.SlowdownBeforeVats());
			}
			vatsWasUp = vatsUp;
			if (set > 0.0f) {
				timer->SetGlobalTimeMultiplier(set, true);
				if (vatsResets++ == 0) {
					REX::INFO("RealTime: VATS runs in real time (bRealTimeVats)");
				}
			}
		}
	}

	void Frame(bool a_session)
	{
		HideTutorials(a_session);
		Vats();
	}

	std::string Describe()
	{
		std::string text;
		const auto  timer = RE::BSTimer::GetSingleton();
		const auto  vats = RE::VATS::GetSingleton();
		const auto  ui = RE::UI::GetSingleton();
		const auto  main = RE::Main::GetSingleton();
		text += std::format("time: mult={:.4f} target={:.4f}", RE::BSTimer::QGlobalTimeMultiplier(), RE::BSTimer::QGlobalTimeMultiplierTarget());
		if (timer) {
			text += std::format(" delta={:.5f} realDelta={:.5f} clamp={:.4f} useTarget={} disabled={}", timer->delta, timer->realTimeDelta, timer->clamp,
				timer->useGlobalTimeMultiplierTarget, timer->disableCounter);
		}
		if (vats) {
			text += std::format(" | vats: mode={} magic={:.3f} playerMagic={:.3f} playback={:.3f} command={:.3f}", vats->mode.underlying(), vats->magicTimeSlowdown,
				vats->playerMagicTimeSlowdown, vats->VATSPlaybackTimer, vats->VATSCommandTimer);
		}
		text += std::format(" vatsMenu={} slowdownBefore={:.3f} resets={}", OpenMenus::Open(OpenMenus::Menu::kVats), vatsClock.SlowdownBeforeVats(), vatsResets);
		if (ui) {
			text += std::format(" | menuMode={} freezeFramePause={} freezeFrameBG={} uiTimer: delta={:.5f} disabled={}", GameStop::MenuMode().value_or(0), ui->freezeFramePause,
				ui->freezeFrameMenuBG, ui->uiTimer.delta, ui->uiTimer.disableCounter);
		}
		if (main) {
			text += std::format(" freezeTime={} gameActive={}", main->freezeTime, main->gameActive);
		}
		if (const auto calendar = RE::Calendar::GetSingleton(); calendar && calendar->gameHour) {
			text += std::format(" hour={:.5f}", calendar->gameHour->value);
		}
		if (const auto setting = ShowTutorials()) {
			text += std::format(" | showTutorials={}", setting->GetBinary());
		}
		text += std::format(" | session={}", HostRules::InSession());
		return text;
	}
}
