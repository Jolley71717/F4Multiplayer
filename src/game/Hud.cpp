#include "game/Hud.h"

namespace Hud
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		// While a loading screen is up the HUD is torn down and rebuilt; a message written then (a
		// friend's spoken line, "X joined") was behind crashes in the HUD's text layout on the render
		// thread. Messages wait until the HUD has been back for a moment.
		constexpr auto        SETTLE = 1500ms;
		// A game load rebuilds every menu some seconds after its loading screen has closed: the old HUD
		// movie is released while the new one comes up (a host crashed in the text layout, with
		// Movie::Release on the stack, 3 s after a save was loaded). Longer hold after one, counted from
		// that loading screen closing: counted from the load's own end, a long loading screen used it up.
		constexpr auto        SETTLE_AFTER_LOAD = 6s;
		// A load's start is announced before its loading screen opens; a load whose end never came
		// holds the HUD only this long.
		constexpr auto        LOAD_LIMIT = 60s;
		constexpr std::size_t MAX_QUEUED = 8;

		// Written by the menu event sink and the load messages, read from the HUD's update and any
		// thread that queues a message.
		std::atomic<bool>              loading{ false };
		std::atomic<bool>              everLoaded{ false };
		std::atomic<Clock::time_point> loadedAt{};
		std::atomic<bool>              gameLoading{ false };        // from the load's start to its end
		std::atomic<Clock::time_point> loadStartedAt{};
		std::atomic<bool>              gameLoaded{ false };
		std::atomic<bool>              awaitingLoadClose{ false };  // the load's loading screen is still up
		std::atomic<Clock::time_point> gameLoadedAt{};
		// Messages wait here for the HUD's own update (OnHudAdvance). Our frame task runs on the game's
		// job threads, twice a frame, while the main thread draws the HUD: a message written from there
		// raced the HUD's own text layout (the crashes in Scaleform's TreeText with no plugin frame).
		std::mutex               queueLock;
		std::deque<std::string>  queued;
		std::uint32_t            held = 0;
		std::uint32_t            dropped = 0;

		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName == "LoadingMenu"sv) {
					loading = a_event.opening;
					if (!a_event.opening) {
						loadedAt = Clock::now();
						everLoaded = true;
						if (awaitingLoadClose.exchange(false)) {
							gameLoadedAt = Clock::now();
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuWatcher watcher;

		bool Settled()
		{
			const auto now = Clock::now();
			if (loading || (gameLoading && now - loadStartedAt.load() < LOAD_LIMIT)) {
				return false;
			}
			if (gameLoaded && now - gameLoadedAt.load() < SETTLE_AFTER_LOAD) {
				return false;
			}
			return !everLoaded || now - loadedAt.load() >= SETTLE;
		}

		void Show(const std::string& a_clean)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), "", false, false);
		}
	}

	void OnLoadStarting()
	{
		loadStartedAt = Clock::now();
		gameLoading = true;
	}

	void OnGameLoaded()
	{
		gameLoadedAt = Clock::now();
		gameLoaded = true;
		awaitingLoadClose = true;
		gameLoading = false;
	}

	bool IsSettled()
	{
		return Settled();
	}

	void Install()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		}
	}

	void OnHudAdvance()
	{
		std::string message;
		{
			std::scoped_lock l{ queueLock };
			if (queued.empty() || !Settled()) {
				return;
			}
			message = std::move(queued.front());
			queued.pop_front();
		}
		Show(message);
	}

	void Notify(const std::string& a_message)
	{
		// Some of this text comes from other players (names, lines, reject reasons): control characters
		// and markup are dropped before the HUD's text field sees them.
		std::string clean;
		clean.reserve(a_message.size());
		for (const unsigned char c : a_message) {
			if (c >= 0x20 && c != 0x7F && c != '<' && c != '>') {
				clean.push_back(static_cast<char>(c));
			}
		}
		REX::INFO("Hud: {}", clean);
		std::scoped_lock l{ queueLock };
		if (!Settled()) {
			++held;
		}
		if (queued.size() < MAX_QUEUED) {
			queued.push_back(std::move(clean));
		} else {
			++dropped;
		}
	}

	std::string Describe()
	{
		std::scoped_lock l{ queueLock };
		return std::format("hud: queued={} held={} dropped={}", queued.size(), held, dropped);
	}
}
