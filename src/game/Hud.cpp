#include "game/Hud.h"

#include "HangWatch.h"
#include "game/ClickWatch.h"
#include "game/SaveLoadHold.h"

namespace Hud
{
	namespace
	{
		using Clock = UiHold::Clock;

		// The one guard (UiHold): the loading screen's events and the load's end go in here, and every UI write
		// asks it. The save or load itself, from its start to its end, it reads from the save/load hold (main.cpp),
		// which begins before the loading screen's event comes.
		UiHold::Guard guard{ SaveLoadHold::Global() };
		// Messages wait here for the HUD's own update (OnHudAdvance). Our frame task runs on the game's job threads,
		// twice a frame, while the main thread draws the HUD: a message written from there raced the HUD's own text
		// layout (the crashes in Scaleform's TreeText with no plugin frame).
		UiHold::Queue queue;

		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName == "LoadingMenu"sv) {
					guard.LoadingScreen(a_event.opening, Clock::now());
					HangWatch::SetLoading(a_event.opening);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuWatcher watcher;

		void Show(const std::string& a_clean)
		{
			ClickWatch::Note(ClickRates::Source::kHud);
			RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), "", false, false);
		}
	}

	bool LoadingScreenUp()
	{
		return guard.LoadingScreenUp(Clock::now());
	}

	void OnGameLoaded()
	{
		guard.GameLoaded(Clock::now());
	}

	bool IsSettled()
	{
		return guard.Settled(Clock::now());
	}

	UiHold::Hold Holding()
	{
		return guard.Why(Clock::now());
	}

	void Install()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		}
	}

	void OnHudAdvance()
	{
		if (const auto message = queue.Pop(IsSettled())) {
			Show(*message);
		}
	}

	void Notify(const std::string& a_message)
	{
		// Not logged at any level: many of these name players (and carry their chat), and players attach the
		// log to public bug reports. What the log needs is logged by the callers, by player ID.
		queue.Push(a_message, IsSettled());
	}

	std::string Describe()
	{
		return std::format("hud: {} hold={}", queue.Describe(), UiHold::Name(Holding()));
	}
}
