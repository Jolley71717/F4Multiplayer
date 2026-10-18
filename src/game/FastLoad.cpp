#include "game/FastLoad.h"

#include "Config.h"
#include "game/FastLoadCap.h"

#include <mutex>

namespace FastLoad
{
	namespace
	{
		using Clock = FastLoadCap::Clock;
		using FastLoadCap::Change;

		// The decision (FastLoadCap) and the vsync it put aside, under one lock: the loading screen's events
		// come on the UI's thread, the ticks on the frame watch's.
		std::mutex       lock;
		FastLoadCap::Cap cap;
		std::uint32_t    savedInterval = 1;

		// Does what the decision says to the renderer. Its presentInterval is the sync interval the engine hands
		// Present each frame (0: no vsync): one aligned 32-bit store, which the render thread reads whenever it
		// presents, and which the loading screen's event has always set from a thread other than the render
		// thread's. a_offFor: how long vsync had been off, for the log. Under the lock.
		void Apply(Change a_change, Clock::duration a_offFor)
		{
			if (a_change == Change::kNone) {
				return;
			}
			const auto renderer = RE::BSGraphics::GetRendererData();
			if (!renderer) {
				return;
			}
			if (a_change == Change::kOff) {
				savedInterval = renderer->presentInterval;
				renderer->presentInterval = 0;
				return;
			}
			renderer->presentInterval = savedInterval;
			if (a_change != Change::kOnClosed) {
				// The screen is still up: a long load finishes at the game's frame rate, a stuck one stops drawing at 1800 fps.
				REX::INFO("FastLoad: vsync back after {} s of the loading screen, which is still up: {}", std::chrono::duration_cast<std::chrono::seconds>(a_offFor).count(), FastLoadCap::Name(a_change));
			}
		}

		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName != "LoadingMenu"sv) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (!RE::BSGraphics::GetRendererData()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto now = Clock::now();
				std::scoped_lock l{ lock };
				// The settings are read per loading screen, not at install: the in-game menu can turn fast loading
				// off and on, and a screen that started with vsync off still gets it back when it closes.
				if (a_event.opening) {
					const auto& config = Config::Get();
					Apply(cap.Opened(config.fastLoading, std::chrono::seconds{ config.fastLoadingSeconds }, now), {});
				} else {
					Apply(cap.Closed(), cap.OffFor(now));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
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

	void Tick(bool a_stalled)
	{
		const auto       now = Clock::now();
		std::scoped_lock l{ lock };
		const auto       offFor = cap.OffFor(now);  // before the tick turns it on
		Apply(cap.Tick(now, a_stalled), offFor);
	}

	std::chrono::steady_clock::duration VsyncOffFor()
	{
		std::scoped_lock l{ lock };
		return cap.OffFor(Clock::now());
	}
}
