// The one hold on every write to the game's UI (game/UiHold.h): the HUD is left alone while a loading screen is
// up, through a save or a game load, and for the seconds after either, while the engine tears its menus down
// and builds them again. A host crashed in Scaleform's text layout 3 s after a save was loaded (the old HUD movie
// released while the new one came up), and again as a conversation opened; the writes that landed then are the
// ones held here.

#include "Test.h"

#include "SourceTree.h"
#include "game/SaveLoadHold.h"
#include "game/UiHold.h"

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using SaveLoadHold::Hold;
using UiHold::Clock;
using UiHold::Guard;
using UiHold::Queue;
using enum UiHold::Hold;

namespace
{
	constexpr auto LIMIT = 60s;
	const auto     START = Clock::time_point{} + 1h;
}

TEST("ui hold: nothing holds the UI before any loading screen, save or load")
{
	Hold  hold{ LIMIT };
	Guard guard{ hold };
	CHECK(!guard.LoadingScreenUp());
	CHECK(guard.Why(START) == kNone);
	CHECK(guard.Settled(START));
}

TEST("ui hold: a loading screen (a door, fast travel) holds the UI while it is up and for a moment after")
{
	Hold  hold{ LIMIT };
	Guard guard{ hold };
	guard.LoadingScreen(true, START);
	CHECK(guard.LoadingScreenUp());
	CHECK(guard.Why(START + 10s) == kLoadingScreen);
	guard.LoadingScreen(false, START + 10s);
	CHECK(!guard.LoadingScreenUp());
	CHECK(guard.Why(START + 10s) == kAfterLoadingScreen);
	CHECK(guard.Why(START + 10s + UiHold::SETTLE - 1ms) == kAfterLoadingScreen);
	CHECK(guard.Settled(START + 10s + UiHold::SETTLE));
}

TEST("ui hold: a save holds the UI from its start to its end, and no longer")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto save = hold.Begin(START);
	CHECK(guard.Why(START + 1s) == kSaveOrLoad);
	hold.End(save);
	CHECK(guard.Settled(START + 1s));
}

// The crash this is for: a game load's own work ran and its loading screen closed, and 3 s later the engine
// released the old HUD movie while building the new one. SETTLE after the screen was not enough; the hold after a
// load is longer, and counts from the screen closing.
TEST("ui hold: a game load holds the UI through the load, its loading screen, and for seconds after the screen has gone")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto load = hold.Begin(START);  // F4SE's message before the load, before the loading screen's event
	CHECK(guard.Why(START) == kSaveOrLoad);
	guard.LoadingScreen(true, START + 200ms);
	CHECK(guard.Why(START + 1s) == kLoadingScreen);
	guard.GameLoaded(START + 2s);  // the load's own work, with the screen still up
	hold.End(load);
	CHECK(guard.Why(START + 2s) == kLoadingScreen);
	guard.LoadingScreen(false, START + 3s);
	CHECK(guard.Why(START + 3s) == kAfterLoad);
	CHECK(guard.Why(START + 3s + UiHold::SETTLE) == kAfterLoad);  // a loading screen's own moment is not enough
	CHECK(guard.Why(START + 3s + UiHold::SETTLE_AFTER_LOAD - 1ms) == kAfterLoad);
	CHECK(guard.Settled(START + 3s + UiHold::SETTLE_AFTER_LOAD));
}

TEST("ui hold: a long loading screen does not use up the hold after the load")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto load = hold.Begin(START);
	guard.LoadingScreen(true, START + 200ms);
	guard.GameLoaded(START + 2s);
	hold.End(load);
	guard.LoadingScreen(false, START + 30s);  // long past SETTLE_AFTER_LOAD from the load's end
	CHECK(guard.Why(START + 30s) == kAfterLoad);
	CHECK(guard.Why(START + 30s + UiHold::SETTLE_AFTER_LOAD - 1ms) == kAfterLoad);
	CHECK(guard.Settled(START + 30s + UiHold::SETTLE_AFTER_LOAD));
}

// The load's own work runs on the main thread between frames, which may come only after its loading screen has
// closed: the hold then counts from the work.
TEST("ui hold: a load whose loading screen closed before its own work ran holds for the seconds from that work")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto load = hold.Begin(START);
	guard.LoadingScreen(true, START + 200ms);
	guard.LoadingScreen(false, START + 1s);
	CHECK(guard.Why(START + 1s) == kSaveOrLoad);  // the hold is still on
	guard.GameLoaded(START + 2s);
	hold.End(load);
	CHECK(guard.Why(START + 2s) == kAfterLoad);
	CHECK(guard.Why(START + 2s + UiHold::SETTLE_AFTER_LOAD - 1ms) == kAfterLoad);
	CHECK(guard.Settled(START + 2s + UiHold::SETTLE_AFTER_LOAD));
}

TEST("ui hold: a loading screen after the load (fast travel) is not taken for the load's own")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto load = hold.Begin(START);
	guard.LoadingScreen(true, START + 200ms);
	guard.GameLoaded(START + 2s);
	hold.End(load);
	guard.LoadingScreen(false, START + 3s);
	CHECK(guard.Settled(START + 3s + UiHold::SETTLE_AFTER_LOAD));
	guard.LoadingScreen(true, START + 20s);
	guard.LoadingScreen(false, START + 22s);
	CHECK(guard.Why(START + 22s) == kAfterLoadingScreen);
	CHECK(guard.Settled(START + 22s + UiHold::SETTLE));  // not SETTLE_AFTER_LOAD
}

TEST("ui hold: a load begun again before the first one's end holds the UI until the second one ends")
{
	Hold       hold{ LIMIT };
	Guard      guard{ hold };
	const auto first = hold.Begin(START);
	const auto second = hold.Begin(START + 1s);
	guard.GameLoaded(START + 2s);
	hold.End(first);
	CHECK(guard.Why(START + 2s + UiHold::SETTLE_AFTER_LOAD) == kSaveOrLoad);
	guard.GameLoaded(START + 10s);
	hold.End(second);
	CHECK(guard.Why(START + 10s) == kAfterLoad);
	CHECK(guard.Settled(START + 10s + UiHold::SETTLE_AFTER_LOAD));
}

TEST("ui hold: a save or load whose end never came holds the UI only for the hold's limit")
{
	Hold  hold{ LIMIT };
	Guard guard{ hold };
	hold.Begin(START);
	CHECK(guard.Why(START + LIMIT - 1s) == kSaveOrLoad);
	CHECK(guard.Settled(START + LIMIT));
}

TEST("ui hold: the hold is asked from any thread while the loading screen's events come on another")
{
	Hold              hold{ LIMIT };
	Guard             guard{ hold };
	std::atomic<bool> stop{ false };
	std::atomic<bool> wrong{ false };
	guard.LoadingScreen(true, START);
	std::thread events{ [&] {
		for (int i = 0; i < 20'000; ++i) {
			guard.LoadingScreen(false, START);
			guard.LoadingScreen(true, START);
		}
		guard.LoadingScreen(false, START);
	} };
	std::thread asker{ [&] {
		while (!stop) {
			// Up, or just closed: never settled at the moment it closed.
			const auto why = guard.Why(START);
			if (why != kLoadingScreen && why != kAfterLoadingScreen) {
				wrong = true;
			}
		}
	} };
	events.join();
	stop = true;
	asker.join();
	CHECK(!wrong);
	CHECK(guard.Settled(START + UiHold::SETTLE));
}

TEST("ui hold: messages queued while the UI is held come out after it, in order, one per HUD update")
{
	Queue queue;
	queue.Push("Sam joined", false);
	queue.Push("Sam: hello", false);
	CHECK(queue.Size() == 2u);
	CHECK(queue.Held() == 2u);
	CHECK(!queue.Pop(false).has_value());  // still held
	CHECK(queue.Size() == 2u);
	auto first = queue.Pop(true);
	REQUIRE(first.has_value());
	CHECK(*first == "Sam joined");
	auto second = queue.Pop(true);
	REQUIRE(second.has_value());
	CHECK(*second == "Sam: hello");
	CHECK(!queue.Pop(true).has_value());
	queue.Push("Sam left", true);
	CHECK(queue.Held() == 2u);  // not held
	CHECK(queue.Pop(true).value_or("") == "Sam left");
}

TEST("ui hold: the queue keeps a few messages and drops the rest, counted")
{
	Queue queue;
	for (std::size_t i = 0; i < Queue::MAX_QUEUED + 3; ++i) {
		queue.Push(std::to_string(i), false);
	}
	CHECK(queue.Size() == Queue::MAX_QUEUED);
	CHECK(queue.Dropped() == 3u);
	CHECK(queue.Held() == Queue::MAX_QUEUED + 3);
	CHECK(queue.Pop(true).value_or("") == "0");  // the oldest first
	CHECK(queue.Describe() == std::format("queued={} held={} dropped=3", Queue::MAX_QUEUED - 1, Queue::MAX_QUEUED + 3));
}

TEST("ui hold: markup and control characters from other players are kept out of a message")
{
	CHECK(Queue::Clean("<b>Sam</b>\x01 joined\r\n") == "bSam/b joined");
	CHECK(Queue::Clean("plain text 123") == "plain text 123");
	CHECK(Queue::Clean("\x7F") == "");
	Queue queue;
	queue.Push("<font color='#ff0000'>Sam</font>: hi", true);
	CHECK(queue.Pop(true).value_or("") == "font color='#ff0000'Sam/font: hi");
}

// Every write to the game's UI goes by the one guard. The game can't run in a test, so the guard is on the
// source: a file that writes to the UI (the HUD's message call, the message box, a menu's movie, a UI task, a
// Scaleform value) asks Hud::IsSettled somewhere in it. Hud.cpp is the guard's own game side.
namespace
{
	bool Mentions(std::string_view a_code, std::string_view a_name)
	{
		return a_code.find(a_name) != std::string_view::npos;
	}

	// What a file does to the UI, by the names of the engine's means.
	std::vector<std::string> UiWrites(std::string_view a_code)
	{
		std::vector<std::string> writes;
		for (const std::string_view name : { "ShowHUDMessage", "MessageMenuManager", "uiMovie", "AddUITask", "GFx::Value", "UIMessageQueue" }) {
			if (Mentions(a_code, name)) {
				writes.emplace_back(name);
			}
		}
		return writes;
	}
}

TEST("ui hold: the guard finds a file's UI writes in its code, not in its comments or strings")
{
	CHECK(UiWrites(SourceTree::CodeOnly(R"x(RE::SendHUDMessage::ShowHUDMessage(a_clean.c_str(), "", false, false);)x")).size() == 1u);
	CHECK(UiWrites(SourceTree::CodeOnly(R"x(const auto movie = menu ? menu->uiMovie.get() : nullptr; Scaleform::GFx::Value v;)x")).size() == 2u);
	CHECK(UiWrites(SourceTree::CodeOnly(R"x(// ShowHUDMessage crashed once
		REX::INFO("no uiMovie here"); /* MessageMenuManager */)x")).empty());
}

TEST("ui hold: every file that writes to the game's UI asks the one guard first")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	int writers = 0;
	for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
		const auto writes = UiWrites(code);
		if (writes.empty() || file == "src/game/Hud.cpp") {
			continue;
		}
		++writers;
		if (!Mentions(code, "Hud::IsSettled")) {
			std::string names;
			for (const auto& name : writes) {
				names += (names.empty() ? "" : ", ") + name;
			}
			Test::Fail(__FILE__, __LINE__, std::format("{} writes to the UI ({}) without asking Hud::IsSettled: hold the write while the UI is not settled", file, names));
		}
	}
	CHECK(writers >= 3);  // the compass, our menu and the developer's gfx commands, at least
}
