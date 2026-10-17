#pragma once

#include "game/SaveLoadHold.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The one hold on everything the plugin writes to the game's UI: the HUD's messages, the compass markers, our
// menu's pages, map markers, the developer's gfx commands. Every one of them asks Hud::IsSettled first (a test
// on the source sees to it) and waits or queues while the answer is no. The engine tears its menus down and
// builds them again around a loading screen and, later again, after a game load: a write that lands on a movie
// being released crashed the game in Scaleform's text layout and render tree, with no plugin frame on the stack
// (the write was queued into the HUD's own update, or came from our frame task beside the engine's). So the UI
// is left alone
//   - while a loading screen is up, and for SETTLE after it has closed (a door, fast travel);
//   - from a save's or a game load's start to its end (the save/load hold, F4SE's messages);
//   - for SETTLE_AFTER_LOAD after a game load, counted from the load's loading screen closing (the engine
//     rebuilds every menu then, and a long loading screen must not use the time up), or from the load's own end
//     when its screen had already closed by then.
// Free of the game, so it is unit tested. Any thread.
namespace UiHold
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// After a loading screen (a crash in the HUD's text layout came from a message written as the screen closed).
	constexpr auto SETTLE = 1500ms;
	// After a game load: a host crashed in the text layout, Movie::Release on the stack, 3 s after a save was loaded.
	constexpr auto SETTLE_AFTER_LOAD = 6s;
	// The longest a loading screen counts as up. Its close event never heard of would keep the frame's game work,
	// the compass and the HUD's messages off for the rest of the run (the one hold with no end of its own: the
	// save/load hold has a limit). Well past any real loading screen, even on a slow disk with every mod there is.
	constexpr auto LOADING_SCREEN_LIMIT = 5min;

	// Why the UI is held, or that it isn't.
	enum class Hold
	{
		kNone,                // settled: writes may go
		kLoadingScreen,       // a loading screen is up
		kSaveOrLoad,          // a save or a game load is under way
		kAfterLoad,           // a game load ended a moment ago: its menus are being rebuilt
		kAfterLoadingScreen,  // a loading screen closed a moment ago
	};

	constexpr std::string_view Name(Hold a_hold)
	{
		switch (a_hold) {
		case Hold::kLoadingScreen:
			return "loading screen";
		case Hold::kSaveOrLoad:
			return "save or load";
		case Hold::kAfterLoad:
			return "after a load";
		case Hold::kAfterLoadingScreen:
			return "after a loading screen";
		default:
			return "settled";
		}
	}

	class Guard
	{
	public:
		// a_hold: the game's save/load hold (its start and end are F4SE's messages, see SaveLoadHold).
		explicit Guard(const SaveLoadHold::Hold& a_hold, Clock::duration a_settle = SETTLE, Clock::duration a_settleAfterLoad = SETTLE_AFTER_LOAD,
			Clock::duration a_loadingLimit = LOADING_SCREEN_LIMIT) :
			hold(a_hold),
			settle(a_settle),
			settleAfterLoad(a_settleAfterLoad),
			loadingLimit(a_loadingLimit)
		{}

		// The loading screen's open or close event (the UI's thread).
		void LoadingScreen(bool a_up, Clock::time_point a_now)
		{
			std::scoped_lock l{ lock };
			if (a_up) {
				// A load waits only for the loading screen that was up when it ended, never a later one.
				awaitingLoadClose = false;
				loadingSince = a_now;
				loading = true;
				return;
			}
			loadedAt = a_now;
			everLoaded = true;
			if (std::exchange(awaitingLoadClose, false)) {
				gameLoadedAt = a_now;  // the load's hold counts from here
			}
			loading = false;
		}

		// A game load's (or a new game's) own work ran, on the frame's thread, before its hold ends (main.cpp).
		void GameLoaded(Clock::time_point a_now)
		{
			std::scoped_lock l{ lock };
			gameLoadedAt = a_now;
			gameLoaded = true;
			// Its loading screen still up: the hold counts from that closing. Already closed (the load's own work
			// runs on the main thread, maybe after it): from now.
			awaitingLoadClose = loading;
		}

		// Whether a loading screen is up: one heard open, and not yet the limit ago.
		[[nodiscard]] bool LoadingScreenUp(Clock::time_point a_now) const
		{
			std::scoped_lock l{ lock };
			return LoadingNow(a_now);
		}

		[[nodiscard]] Hold Why(Clock::time_point a_now) const
		{
			{
				std::scoped_lock l{ lock };
				if (LoadingNow(a_now)) {
					return Hold::kLoadingScreen;
				}
				// The save/load hold is asked after this lock is let go: it has a lock of its own.
			}
			if (hold.Held(a_now)) {
				return Hold::kSaveOrLoad;
			}
			std::scoped_lock l{ lock };
			if (LoadingNow(a_now)) {
				return Hold::kLoadingScreen;  // opened meanwhile
			}
			if (gameLoaded && a_now - gameLoadedAt < settleAfterLoad) {
				return Hold::kAfterLoad;
			}
			if (everLoaded && a_now - loadedAt < settle) {
				return Hold::kAfterLoadingScreen;
			}
			return Hold::kNone;
		}

		// Whether the UI may be written to now.
		[[nodiscard]] bool Settled(Clock::time_point a_now) const
		{
			return Why(a_now) == Hold::kNone;
		}

	private:
		// Under the lock.
		[[nodiscard]] bool LoadingNow(Clock::time_point a_now) const { return loading && a_now - loadingSince < loadingLimit; }

		const SaveLoadHold::Hold& hold;
		Clock::duration           settle;
		Clock::duration           settleAfterLoad;
		Clock::duration           loadingLimit;
		mutable std::mutex        lock;
		bool                      loading = false;            // a loading screen is up (its event; counts for loadingLimit at most)
		Clock::time_point         loadingSince{};             // when it opened
		bool                      everLoaded = false;
		Clock::time_point         loadedAt{};                 // the last loading screen closed
		bool                      gameLoaded = false;
		bool                      awaitingLoadClose = false;  // the load ended with its loading screen still up
		Clock::time_point         gameLoadedAt{};
	};

	// The HUD's messages, waiting for the HUD's own update while the UI is held. Any thread.
	class Queue
	{
	public:
		// A flood of messages (a stuck friend's spoken lines) is not kept: the newest are dropped, counted.
		static constexpr std::size_t MAX_QUEUED = 8;

		// Some of this text comes from other players (names, lines, reject reasons): control characters and
		// markup are dropped before the HUD's text field sees them.
		static std::string Clean(std::string_view a_text)
		{
			std::string clean;
			clean.reserve(a_text.size());
			for (const unsigned char c : a_text) {
				if (c >= 0x20 && c != 0x7F && c != '<' && c != '>') {
					clean.push_back(static_cast<char>(c));
				}
			}
			return clean;
		}

		// Queues a message (cleaned). a_settled: whether the UI may be written to now; a message that arrives
		// while it may not is counted as held.
		void Push(std::string_view a_text, bool a_settled)
		{
			auto clean = Clean(a_text);
			std::scoped_lock l{ lock };
			held += !a_settled;
			if (queued.size() < MAX_QUEUED) {
				queued.push_back(std::move(clean));
			} else {
				++dropped;
			}
		}

		// The next message to show, if there is one and the UI may be written to (a_settled).
		[[nodiscard]] std::optional<std::string> Pop(bool a_settled)
		{
			std::scoped_lock l{ lock };
			if (queued.empty() || !a_settled) {
				return std::nullopt;
			}
			auto message = std::move(queued.front());
			queued.pop_front();
			return message;
		}

		[[nodiscard]] std::size_t Size() const
		{
			std::scoped_lock l{ lock };
			return queued.size();
		}

		[[nodiscard]] std::uint32_t Held() const
		{
			std::scoped_lock l{ lock };
			return held;
		}

		[[nodiscard]] std::uint32_t Dropped() const
		{
			std::scoped_lock l{ lock };
			return dropped;
		}

		[[nodiscard]] std::string Describe() const
		{
			std::scoped_lock l{ lock };
			return std::format("queued={} held={} dropped={}", queued.size(), held, dropped);
		}

	private:
		mutable std::mutex      lock;
		std::deque<std::string> queued;
		std::uint32_t           held = 0;     // messages that arrived while the UI was held
		std::uint32_t           dropped = 0;  // messages past MAX_QUEUED
	};
}
