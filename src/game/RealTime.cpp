#include "game/RealTime.h"

#include "Background.h"
#include "Config.h"
#include "game/GameStop.h"
#include "game/HostRules.h"
#include "game/OpenMenus.h"
#include "game/MoveSync.h"
#include "game/RealTimeVats.h"
#include "game/VatsTime.h"

#include <chrono>
#include <format>
#include <utility>

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
		std::uint64_t       groupSets = 0;
		bool                vatsWasUp = false;

		// VATS and time in the session (game/VatsTime.h).
		VatsTime::Reporter                     reporter;
		VatsTime::Friends                      friends;
		std::vector<std::vector<std::uint8_t>> outgoing;
		VatsTime::Own                          own;            // as reported last frame
		bool                                   pretendVats = false;  // vatstime pretend on
		float                                  groupOn = 0.0f;  // the group's slow motion in effect last frame
		bool                                   slowedAlone = false;  // our game slowed on its own (Jet, slow VATS), logged once each time

		// bRealTimeVats: the slow motion of VATS set back to normal while it is up; bGroupVatsSlowdown: every game at the
		// host's speed while anyone is in VATS. Then our own state goes out when it changed.
		void Vats()
		{
			const auto vats = RE::VATS::GetSingleton();
			const auto timer = RE::BSTimer::GetSingleton();
			if (!vats || !timer) {
				return;
			}
			const bool  session = HostRules::InSession();
			const bool  realVats = OpenMenus::Open(OpenMenus::Menu::kVats) || vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback);
			const bool  vatsUp = realVats || (pretendVats && session);
			const float multiplier = RE::BSTimer::QGlobalTimeMultiplier();
			const float group = session ? VatsTime::Group(friends.GroupSpeed(), vatsUp, friends.InVats()) : 0.0f;
			// The game's own VATS only: a pretended one has no slowdown of the game's to set back.
			const float set = vatsClock.Frame(session, HostRules::Get().realTimeVats, realVats, multiplier, vats->magicTimeSlowdown, group);
			if (realVats && !vatsWasUp && vatsClock.SlowdownBeforeVats() > 0.0f && session && HostRules::Get().realTimeVats && group <= 0.0f) {
				REX::INFO("RealTime: VATS up on a slowdown of {:.2f} (Jet or the like): left slowed", vatsClock.SlowdownBeforeVats());
			}
			vatsWasUp = realVats;
			if (group != groupOn) {
				if (group > 0.0f) {
					REX::INFO("RealTime: group VATS slow motion on at x{:.2f} ({}; {} friend(s) in VATS)", group, vatsUp ? "we are in VATS" : "a friend is in VATS", friends.InVats());
				} else {
					REX::INFO("RealTime: group VATS slow motion off (game at x{:.2f}{})", set > 0.0f ? set : multiplier, set > 0.0f ? ", set back" : "");
				}
				groupOn = group;
			}
			if (set > 0.0f) {
				timer->SetGlobalTimeMultiplier(set, true);
				if (group > 0.0f) {
					++groupSets;
				} else if (realVats && vatsResets++ == 0) {
					REX::INFO("RealTime: VATS runs in real time (bRealTimeVats)");
				}
			}

			// Our game slowed on its own, outside the group's slow motion: friends' stand-ins and their NPCs go on at full
			// speed in it, on the wall clock (VatsTime.h). Logged once each time, for the rig.
			const float runs = set > 0.0f ? set : multiplier;
			const bool  alone = session && group <= 0.0f && runs < VatsTime::FULL;
			if (alone != slowedAlone) {
				slowedAlone = alone;
				if (alone) {
					REX::INFO("RealTime: our game runs slowed on its own at x{:.2f} ({}): friends and their NPCs keep full speed here", runs,
						realVats ? "VATS without real time" : vats->magicTimeSlowdown > 0.0f ? "Jet or the like" : "the console's or a mod's time multiplier");
				} else {
					REX::INFO("RealTime: our game is back to x{:.2f}", runs);
				}
			}

			own = { vatsUp, runs };
			if (const auto row = reporter.Frame(std::chrono::steady_clock::now(), session, own)) {
				outgoing.push_back(Protocol::Encode(*row));
				if (reporter.Reports() <= 3 || MoveSync::Loggable(static_cast<std::uint32_t>(reporter.Reports()))) {
					REX::INFO("RealTime: reported vats={} mult={:.2f} ({} so far)", own.vats, row->multiplier, reporter.Reports());
				}
			}
		}
	}

	void Frame(bool a_session)
	{
		HideTutorials(a_session);
		Vats();
	}

	void OnTimeScales(const Protocol::TimeScales& a_table, std::uint32_t a_self)
	{
		const bool hadGroup = friends.GroupSpeed() > 0.0f;
		for (const auto& change : friends.Set(a_table, a_self)) {
			if (change.left) {
				if (change.wasVats) {
					REX::INFO("RealTime: player {} left while in VATS", change.playerId);
				}
			} else if (change.vats != change.wasVats) {
				REX::INFO("RealTime: player {} {} VATS (their game at x{:.2f})", change.playerId, change.vats ? "is in" : "is out of", change.multiplier);
			}
		}
		if (const bool hasGroup = friends.GroupSpeed() > 0.0f; hasGroup != hadGroup) {
			REX::INFO("RealTime: the host's group VATS slow motion is {}{}", hasGroup ? "on at x" : "off", hasGroup ? std::format("{:.2f}", friends.GroupSpeed()) : "");
		}
	}

	void OnSessionEnded()
	{
		friends.Clear();
		pretendVats = false;
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	std::string Status()
	{
		return std::format("vatsTime: own={}/x{:.2f} friendsInVats={} group={} groupSpeed={:.2f} reports={} groupSets={}", own.vats ? "vats" : "-", own.multiplier,
			friends.InVats(), groupOn > 0.0f ? "on" : "off", friends.GroupSpeed(), reporter.Reports(), groupSets);
	}

	std::string VatsTimeCommand(std::string_view a_args)
	{
		if (a_args == "pretend on" || a_args == "pretend off") {
			pretendVats = a_args == "pretend on";
		} else if (!a_args.empty()) {
			return "usage: vatstime [pretend on|off]";
		}
		std::string text = Status() + std::format(" pretend={} | friends:", pretendVats);
		for (const auto& row : friends.Rows()) {
			text += std::format(" player {}={}/x{:.2f}", row.playerId, (row.flags & Protocol::kTimeVats) ? "vats" : "-", row.multiplier);
		}
		if (friends.Rows().empty()) {
			text += " none";
		}
		return text;
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
		text += std::format(" | session={} | {}", HostRules::InSession(), Status());
		return text;
	}
}
