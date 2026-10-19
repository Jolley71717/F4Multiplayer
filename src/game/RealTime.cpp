#include "game/RealTime.h"

#include "Background.h"
#include "Config.h"
#include "game/HostRules.h"
#include "game/OpenMenus.h"
#include "game/RealTimeMenus.h"

namespace RealTime
{
	namespace
	{
		constexpr auto COUNT = RealTimeMenus::MENUS.size();
		using Create_t = RE::UIMenuEntry::Create_t;

		// The game's own creation of each menu in RealTimeMenus::MENUS, called by ours.
		std::array<Create_t*, COUNT> originals{};

		// Which groups are in real time now (a bit per RealTimeMenus::Group): set on the main thread each frame, read
		// wherever the UI makes a menu.
		std::atomic<std::uint32_t> realTimeGroups{ 0 };

		// What each menu was made with, for the log and the dev channel.
		std::array<std::atomic<std::uint32_t>, COUNT> flagsSeen{};
		std::array<std::atomic<std::uint32_t>, COUNT> madeRealTime{};
		std::array<std::atomic<bool>, COUNT>          logged{};

		bool installed = false;

		constexpr std::uint32_t Bit(RealTimeMenus::Group a_group)
		{
			return 1u << static_cast<std::uint32_t>(a_group);
		}

		// The menu's flags are changed as it is made, and only then: the UI counts a menu that pauses the game as it
		// opens and again as it closes, so they must be the same at both. A session that starts or ends while a menu is
		// open leaves that menu as it was made.
		template <std::size_t I>
		RE::IMenu* Create(const RE::UIMessage& a_message)
		{
			const auto menu = originals[I] ? originals[I](a_message) : nullptr;
			if (!menu) {
				return menu;
			}
			const auto& entry = RealTimeMenus::MENUS[I];
			const auto  flags = menu->menuFlags.underlying();
			const bool  realTime = (realTimeGroups.load() & Bit(entry.group)) != 0 && RealTimeMenus::RealTimeFlags(flags) != flags;
			if (realTime) {
				menu->menuFlags.reset(RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kFreezeFramePause);
				++madeRealTime[I];
			}
			flagsSeen[I] = flags;
			if (!logged[I].exchange(true)) {
				REX::INFO("RealTime: {} made with flags {:08X}{}", entry.menu, flags, realTime ? ", runs in real time" : "");
			}
			return menu;
		}

		template <std::size_t... I>
		constexpr std::array<Create_t*, COUNT> Creators(std::index_sequence<I...>)
		{
			return { &Create<I>... };
		}

		constexpr auto CREATORS = Creators(std::make_index_sequence<COUNT>{});

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

		std::uint64_t vatsResets = 0;

		// bRealTimeVats: the slow motion of VATS set back to normal while it is up.
		void Vats()
		{
			const auto vats = RE::VATS::GetSingleton();
			const auto timer = RE::BSTimer::GetSingleton();
			if (!vats || !timer) {
				return;
			}
			const bool  vatsUp = OpenMenus::Open(OpenMenus::Menu::kVats) || vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback);
			const float set = RealTimeMenus::VatsTimeMultiplier(HostRules::InSession(), HostRules::Get().realTimeVats, vatsUp, RE::BSTimer::QGlobalTimeMultiplier(),
				vats->magicTimeSlowdown);
			if (set > 0.0f) {
				timer->SetGlobalTimeMultiplier(set, true);
				if (vatsResets++ == 0) {
					REX::INFO("RealTime: VATS runs in real time (bRealTimeVats)");
				}
			}
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::WARN("RealTime: no UI; menus stop the game in a session too");
			return;
		}
		// Only the creation function of each entry is changed: no menu is looked at, nor any reference to one taken.
		std::string missing;
		{
			RE::BSAutoWriteLock lock{ RE::UI::GetMenuMapRWLock() };
			auto&               menus = ui->menuMap;
			for (std::size_t i = 0; i < COUNT; ++i) {
				const auto it = menus.find(RE::BSFixedString{ RealTimeMenus::MENUS[i].menu });
				if (it == menus.end() || !it->second.create) {
					missing += std::format("{}{}", missing.empty() ? "" : ", ", RealTimeMenus::MENUS[i].menu);
					continue;
				}
				originals[i] = it->second.create;
				it->second.create = CREATORS[i];
			}
		}
		installed = true;
		REX::INFO("RealTime: watching {} menus{}", COUNT, missing.empty() ? std::string{} : std::format(" (not registered: {})", missing));
	}

	void Frame(bool a_session)
	{
		const auto& rules = HostRules::Get();
		const bool  inSession = HostRules::InSession();
		std::uint32_t groups = 0;
		for (const auto group : { RealTimeMenus::Group::kPipboy, RealTimeMenus::Group::kMenus }) {
			if (RealTimeMenus::RealTime(group, inSession, rules)) {
				groups |= Bit(group);
			}
		}
		realTimeGroups = groups;
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
			text += std::format(" | vats: mode={} magic={:.3f} playerMagic={:.3f} playback={:.3f} command={:.3f} camera={}", vats->mode.underlying(), vats->magicTimeSlowdown,
				vats->playerMagicTimeSlowdown, vats->VATSPlaybackTimer, vats->VATSCommandTimer, static_cast<const void*>(vats->cameraContext));
		}
		text += std::format(" vatsMenu={} resets={}", OpenMenus::Open(OpenMenus::Menu::kVats), vatsResets);
		if (ui) {
			text += std::format(" | menuMode={} freezeFramePause={} freezeFrameBG={}", ui->menuMode, ui->freezeFramePause, ui->freezeFrameMenuBG);
		}
		if (main) {
			text += std::format(" freezeTime={} gameActive={}", main->freezeTime, main->gameActive);
		}
		if (const auto setting = ShowTutorials()) {
			text += std::format(" | showTutorials={}", setting->GetBinary());
		}
		text += std::format(" | session={} groups={:X}; menus:", HostRules::InSession(), realTimeGroups.load());
		for (std::size_t i = 0; i < COUNT; ++i) {
			if (!originals[i]) {
				text += std::format(" {}=missing", RealTimeMenus::MENUS[i].menu);
			} else if (logged[i]) {
				text += std::format(" {}={:08X}/{}", RealTimeMenus::MENUS[i].menu, flagsSeen[i].load(), madeRealTime[i].load());
			}
		}
		return text;
	}
}
