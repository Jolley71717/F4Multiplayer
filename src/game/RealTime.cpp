#include "game/RealTime.h"

#include "Background.h"
#include "Config.h"
#include "game/HostRules.h"
#include "game/OpenMenus.h"
#include "game/RealTimeVats.h"

#include <atomic>
#include <chrono>

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

		RealTimeVats::Clock    vatsClock;
		RealTimeVats::Playback playback;
		std::atomic<std::int64_t> lagNanos{ 0 };  // playback's lag, read by the engine's updates of the stand-ins
		std::uint64_t          vatsResets = 0;
		std::uint64_t          vatsSlowedFrames = 0;
		bool                   vatsWasUp = false;
		std::chrono::steady_clock::time_point lastFrame{};
		float                  friendsRate = 1.0f;

		// iVatsMode: in real time, the slow motion of VATS set back to normal while it is up; in the mode with slowed
		// friends, the stand-ins' playback slowed by the same rate (RealTimeVats::Playback).
		void Vats()
		{
			const auto now = std::chrono::steady_clock::now();
			const float seconds = lastFrame == std::chrono::steady_clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - lastFrame).count();
			lastFrame = now;

			const auto vats = RE::VATS::GetSingleton();
			const auto timer = RE::BSTimer::GetSingleton();
			if (!vats || !timer) {
				return;
			}
			const bool          inSession = HostRules::InSession();
			const std::uint8_t  mode = HostRules::Get().vatsMode;
			const float         multiplier = RE::BSTimer::QGlobalTimeMultiplier();
			const bool          vatsUp = OpenMenus::Open(OpenMenus::Menu::kVats) || vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback);
			const float         set = vatsClock.Frame(inSession, mode, vatsUp, multiplier, vats->magicTimeSlowdown);
			if (vatsUp && !vatsWasUp && vatsClock.SlowdownBeforeVats() > 0.0f && inSession && mode != Protocol::SessionRules::VATS_SLOW_MOTION) {
				REX::INFO("RealTime: VATS up on a slowdown of {:.2f} (Jet or the like): left slowed", vatsClock.SlowdownBeforeVats());
			}
			vatsWasUp = vatsUp;
			if (set > 0.0f) {
				timer->SetGlobalTimeMultiplier(set, true);
				if (vatsResets++ == 0) {
					REX::INFO("RealTime: VATS runs in real time (iVatsMode 0)");
				}
			}

			// Friends' stand-ins play at the game's slowdown while VATS is up, and catch up afterwards. Out of a session, or
			// in another mode, there is no lag and it closes at once.
			friendsRate = vatsClock.FriendsRate(inSession, mode, vatsUp, multiplier);
			if (!inSession || !RealTimeVats::SlowsFriends(mode)) {
				playback.Reset();
			} else {
				playback.Advance(seconds, friendsRate);
			}
			if (friendsRate < 1.0f && vatsSlowedFrames++ == 0) {
				REX::INFO("RealTime: VATS up: friends' stand-ins play slowed with the game (iVatsMode 2), rate {:.3f}", friendsRate);
			} else if (friendsRate >= 1.0f) {
				vatsSlowedFrames = 0;
			}
			lagNanos.store(static_cast<std::int64_t>(playback.Lag() * 1.0e9f), std::memory_order_relaxed);
		}
	}

	void Frame(bool a_session)
	{
		HideTutorials(a_session);
		Vats();
	}

	std::chrono::steady_clock::duration StandInLag()
	{
		return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds{ lagNanos.load(std::memory_order_relaxed) });
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
		text += std::format(" vatsMode={} friendsRate={:.3f} standInLag={:.3f}s", RealTimeVats::ModeName(HostRules::Get().vatsMode), friendsRate, playback.Lag());
		if (ui) {
			text += std::format(" | menuMode={} freezeFramePause={} freezeFrameBG={}", ui->menuMode, ui->freezeFramePause, ui->freezeFrameMenuBG);
		}
		if (main) {
			text += std::format(" freezeTime={} gameActive={}", main->freezeTime, main->gameActive);
		}
		if (const auto setting = ShowTutorials()) {
			text += std::format(" | showTutorials={}", setting->GetBinary());
		}
		text += std::format(" | session={}", HostRules::InSession());
		return text;
	}
}
