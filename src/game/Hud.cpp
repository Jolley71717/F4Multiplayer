#include "game/Hud.h"

#include "HangWatch.h"
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

		// The sound a notice plays: none, as the game's own notices (Papyrus Debug.Notification, the XP and quest
		// lines) ask for it: a null name. The HUD hands the name to UIUtils::PlayMenuSound (ID 2249707, 1.11.240: +1086570)
		// when the notice shows, which looks it up among the sounds' editor IDs by a hash of the name (that lookup: ID
		// 2214760, +828B60). A null name is never
		// looked up, but "" is: its hash is 0, the key of any sound whose editor ID was set empty (a mod's record, or an
		// editor-ID loader), and that sound played on the player, unstopped. A looping one (a generator's hum, rushing
		// water) stacked with each notice until a load. The HUD's own update (HUDMessages, its slot 4 at +9D2310) shows the
		// next message through +9D2730, which passes the message's sound to PlayMenuSound as it is, null included; the
		// message's movie clip (HUDMenu.swf's HUDMessageItem) plays nothing. So a notice is silent, however many come at
		// once: ClickWatch doesn't count them (a friend downed and helped up a few times in a row read as a repeating
		// sound). Tests (NoticeSoundTests) keep every notice on this null name.
		constexpr const char* NOTICE_SOUND = nullptr;

		void Show(const std::string& a_clean)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), NOTICE_SOUND, false, false);
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
