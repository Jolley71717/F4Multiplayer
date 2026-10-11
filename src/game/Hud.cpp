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
		constexpr std::size_t MAX_QUEUED = 8;

		std::atomic<bool>        loading{ false };
		std::atomic<bool>        everLoaded{ false };
		Clock::time_point        loadedAt{};
		std::deque<std::string>  queued;  // main thread only
		std::uint32_t            held = 0;

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
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuWatcher watcher;

		bool Settled()
		{
			return !loading && (!everLoaded || Clock::now() - loadedAt >= SETTLE);
		}

		void Show(const std::string& a_clean)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), "", false, false);
		}
	}

	void Install()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		}
	}

	void Frame()
	{
		if (queued.empty() || !Settled()) {
			return;
		}
		Show(queued.front());
		queued.pop_front();
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
		if (!Settled()) {
			if (queued.size() < MAX_QUEUED) {
				queued.push_back(std::move(clean));
			}
			++held;
			return;
		}
		Show(clean);
	}

	std::string Describe()
	{
		return std::format("hud: queued={} held={}", queued.size(), held);
	}
}
