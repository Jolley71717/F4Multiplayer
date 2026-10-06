#include "game/FastLoad.h"

namespace FastLoad
{
	namespace
	{
		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName != "LoadingMenu"sv) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto renderer = RE::BSGraphics::GetRendererData();
				if (!renderer) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (a_event.opening) {
					if (!active) {
						savedInterval = renderer->presentInterval;
						active = true;
					}
					renderer->presentInterval = 0;
				} else if (active) {
					renderer->presentInterval = savedInterval;
					active = false;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::uint32_t savedInterval = 1;
			bool          active = false;
		};

		MenuWatcher watcher;
	}

	void Install()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::WARN("FastLoad: UI not available");
			return;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		REX::INFO("FastLoad: installed");
	}
}
