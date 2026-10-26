#include "game/MenuClock.h"

#include "game/GameStop.h"

#include <charconv>

namespace MenuClock
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr float NPC_RANGE = 4000.0f;

		// Written on whichever thread opens or closes a menu; read in Frame.
		std::mutex                 eventLock;
		std::string                eventName;
		bool                       eventOpening = false;
		std::atomic<std::uint64_t> events{ 0 };

		class Watcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				{
					std::scoped_lock l{ eventLock };
					const char* name = a_event.menuName.c_str();
					eventName = name ? name : "";
					eventOpening = a_event.opening;
				}
				events.fetch_add(1);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Watcher watcher;
		bool    watching = false;

		bool              armed = false;
		Clock::duration   window = std::chrono::seconds(2);
		std::uint64_t     seenEvents = 0;
		Clock::time_point until{};
		std::uint32_t     frameInWindow = 0;
		std::uint32_t     npcId = 0;
		RE::NiPoint3      npcAt{};

		// The living NPC nearest the player, picked at each menu event, and where it stands.
		void PickNpc()
		{
			npcId = 0;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return;
			}
			float nearest = NPC_RANGE;
			cell->ForEachReferenceInRange(player->data.location, NPC_RANGE, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && !actor->IsPlayerRef() && !actor->IsDead(false)) {
					const float distance = actor->data.location.GetDistance(player->data.location);
					if (distance < nearest) {
						nearest = distance;
						npcId = actor->GetFormID();
						npcAt = actor->data.location;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// How far the picked NPC moved since the last frame.
		std::string NpcMoved()
		{
			const auto actor = npcId ? RE::TESForm::GetFormByID<RE::Actor>(npcId) : nullptr;
			if (!actor) {
				return "npc=none";
			}
			const auto at = actor->data.location;
			const float moved = at.GetDistance(npcAt);
			npcAt = at;
			return std::format("npc={:08X} moved={:.2f}", npcId, moved);
		}
	}

	std::string Sample()
	{
		const auto ui = RE::UI::GetSingleton();
		const auto timer = RE::BSTimer::GetSingleton();
		const auto main = RE::Main::GetSingleton();
		const auto calendar = RE::Calendar::GetSingleton();
		std::string text = std::format("menuMode={}", GameStop::MenuMode().value_or(0));
		if (ui) {
			text += std::format(" freezeFramePause={} freezeFrameBG={} uiTimer: delta={:.5f} realDelta={:.5f} disabled={}", ui->freezeFramePause, ui->freezeFrameMenuBG,
				ui->uiTimer.delta, ui->uiTimer.realTimeDelta, ui->uiTimer.disableCounter);
		}
		if (timer) {
			text += std::format(" | timer: delta={:.5f} realDelta={:.5f} disabled={} mult={:.4f}", timer->delta, timer->realTimeDelta, timer->disableCounter,
				RE::BSTimer::QGlobalTimeMultiplier());
		}
		if (main) {
			text += std::format(" | freezeTime={} gameActive={}", main->freezeTime, main->gameActive);
		}
		if (calendar && calendar->gameHour) {
			text += std::format(" hour={:.5f}", calendar->gameHour->value);
		}
		return text;
	}

	std::string Command(std::string_view a_args)
	{
		if (a_args.starts_with("on")) {
			auto seconds = 2;
			if (const auto rest = a_args.substr(2); !rest.empty()) {
				const auto from = rest.find_first_not_of(' ');
				if (from == std::string_view::npos ||
					std::from_chars(rest.data() + from, rest.data() + rest.size(), seconds).ec != std::errc{} || seconds < 1 || seconds > 60) {
					return "error: usage: menuclock on [seconds 1-60] | off";
				}
			}
			if (!watching) {
				const auto ui = RE::UI::GetSingleton();
				if (!ui) {
					return "error: the UI isn't there yet";
				}
				ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
				watching = true;
			}
			armed = true;
			window = std::chrono::seconds(seconds);
			seenEvents = events.load();
			REX::INFO("MenuClock: armed, logging each frame for {} s after a menu opens or closes", seconds);
			return std::format("armed for {} s after each menu event", seconds);
		}
		if (a_args == "off") {
			armed = false;
			until = {};
			REX::INFO("MenuClock: off");
			return "off";
		}
		if (!a_args.empty()) {
			return "error: usage: menuclock on [seconds 1-60] | off";
		}
		return std::format("armed={} events={} | {}", armed, events.load(), Sample());
	}

	void Frame()
	{
		if (!armed) {
			return;
		}
		const auto now = Clock::now();
		if (const auto seen = events.load(); seen != seenEvents) {
			seenEvents = seen;
			std::string name;
			bool        opening = false;
			{
				std::scoped_lock l{ eventLock };
				name = eventName;
				opening = eventOpening;
			}
			PickNpc();
			until = now + window;
			frameInWindow = 0;
			REX::INFO("MenuClock: {} {} (event {})", name, opening ? "opened" : "closed", seen);
		}
		if (now >= until) {
			return;
		}
		REX::INFO("MenuClock: frame {} {} | {}", frameInWindow++, Sample(), NpcMoved());
	}
}
