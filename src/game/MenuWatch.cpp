#include "game/MenuWatch.h"

namespace MenuWatch
{
	namespace
	{
		std::mutex        lock;
		MenuLog::Log      log;
		std::atomic<bool> installed{ false };

		class Watcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const char* name = a_event.menuName.c_str();
				if (name && *name) {
					std::scoped_lock l{ lock };
					log.Event(name, a_event.opening, MenuLog::Clock::now());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Watcher watcher;
	}

	void Install()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui || installed.exchange(true)) {
			return;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
	}

	bool Installed()
	{
		return installed;
	}

	bool IsOpen(std::string_view a_name)
	{
		std::scoped_lock l{ lock };
		return log.IsOpen(a_name);
	}

	MenuLog::Clock::duration OpenFor(std::string_view a_name)
	{
		std::scoped_lock l{ lock };
		return log.OpenFor(a_name, MenuLog::Clock::now());
	}

	std::string Describe()
	{
		std::scoped_lock l{ lock };
		return log.Describe(MenuLog::Clock::now());
	}
}
