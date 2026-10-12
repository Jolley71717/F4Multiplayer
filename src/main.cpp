#include "Config.h"
#include "DevChannel.h"
#include "DevCommands.h"
#include "game/Compass.h"
#include "game/FastLoad.h"
#include "game/Hud.h"
#include "game/WorldSync.h"
#include "game/FrameStats.h"
#include "game/Menu.h"
#include "MainThread.h"
#include "game/SaveGuard.h"
#include "net/Session.h"

// user32 (windows.h clashes with CommonLibF4's names).
extern "C" {
__declspec(dllimport) std::intptr_t __stdcall GetWindowLongPtrW(void* a_window, int a_index);
__declspec(dllimport) std::intptr_t __stdcall SetWindowLongPtrW(void* a_window, int a_index, std::intptr_t a_value);
__declspec(dllimport) std::intptr_t __stdcall CallWindowProcW(std::intptr_t a_proc, void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam);
__declspec(dllimport) int __stdcall PostMessageW(void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam);
__declspec(dllimport) unsigned int __stdcall RegisterWindowMessageW(const wchar_t* a_name);
}

namespace
{
	bool gameDataReady = false;

	// The thread the plugin was loaded on: the game's main thread (WinMain).
	std::thread::id mainThread;

	// Which threads run the frame's game work (see FrameStats::TakeThreads): plain counters behind a lock.
	struct FrameThreads
	{
		std::uint32_t             mainCalls = 0, otherCalls = 0;
		std::uint32_t             mainRepeats = 0, otherRepeats = 0;   // no new drawn frame since the call before
		std::uint32_t             mainDrawing = 0, otherDrawing = 0;   // the renderer was inside a frame
		std::uint32_t             lastDrawn = 0;
		std::set<std::thread::id> others;
	};
	std::mutex   frameThreadsLock;
	FrameThreads frameThreads;

	void CountFrameThread()
	{
		const auto state = RE::BSGraphics::State::GetSingleton();
		const auto drawn = state ? state->frameCount : 0;
		const bool drawing = state && state->insideFrame;
		const bool onMain = std::this_thread::get_id() == mainThread;
		std::scoped_lock l{ frameThreadsLock };
		auto& t = frameThreads;
		const bool repeat = drawn == t.lastDrawn;
		t.lastDrawn = drawn;
		if (onMain) {
			++t.mainCalls;
			t.mainRepeats += repeat;
			t.mainDrawing += drawing;
		} else {
			++t.otherCalls;
			t.otherRepeats += repeat;
			t.otherDrawing += drawing;
			t.others.insert(std::this_thread::get_id());
		}
	}

	// A loading screen is up. The engine may pump window messages from inside its load code, and game
	// work must not run in the middle of that; it waits for the load to finish.
	std::atomic<bool> loading{ false };

	class LoadingWatcher :
		public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event.menuName == "LoadingMenu"sv) {
				loading = a_event.opening;
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	LoadingWatcher loadingWatcher;

	// Co-save records: which of the session's changes each save contains.
	constexpr std::uint32_t SERIALIZATION_ID = 'F4MP';

	void OnGameSaved(const F4SE::SerializationInterface* a_intfc)
	{
		WorldSync::SaveResyncPoint(a_intfc);
	}

	void OnGameLoaded(const F4SE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == 'CONT') {
				WorldSync::LoadResyncPoint(a_intfc, version, length);
			}
		}
	}

	void OnRevert(const F4SE::SerializationInterface*)
	{
		WorldSync::RevertResyncPoint();
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			FastLoad::Install();  // checks bFastLoading itself, per loading screen
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink<RE::MenuOpenCloseEvent>(&loadingWatcher);
			}
			Compass::Install();
			Hud::Install();
			Session::Initialize();
			gameDataReady = true;
			REX::INFO("Game data ready");
			break;
		case F4SE::MessagingInterface::kPreSaveGame:
			// Remote players' actors must never be written into a save.
			Session::OnBeforeSaveOrLoad();
			SaveGuard::BeforeSave();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
			Session::OnBeforeSaveOrLoad();
			break;
		case F4SE::MessagingInterface::kPostLoadGame:
			if (gameDataReady) {
				SaveGuard::AfterLoad();
			}
			[[fallthrough]];
		case F4SE::MessagingInterface::kNewGame:
			if (gameDataReady) {
				Session::OnGameLoaded();
				Hud::OnGameLoaded();
			}
			break;
		default:
			break;
		}
	}

	// Whether this call starts a new drawn frame. The F4SE permanent task runs twice per drawn frame, on
	// the game's job threads (measured: dev command "frames"), at uneven moments: stand-ins sampled at
	// either call showed a different moment of their path each frame (a visible jitter), and frame times
	// came out halved. Game work runs once per drawn frame, or after FRAME_FALLBACK without one (a
	// loading screen may not count frames, and the connection must still be served).
	constexpr auto                     FRAME_FALLBACK = std::chrono::milliseconds(50);
	std::atomic<std::uint32_t>         lastDrawnFrame{ ~0u };
	std::chrono::steady_clock::time_point lastWork{};

	bool NewFrame(std::chrono::steady_clock::time_point a_now)
	{
		const auto state = RE::BSGraphics::State::GetSingleton();
		const auto drawn = state ? state->frameCount : 0u;
		const bool counted = state && lastDrawnFrame.exchange(drawn) != drawn;
		if (!counted && a_now - lastWork < FRAME_FALLBACK) {
			return false;
		}
		lastWork = a_now;
		return true;
	}

	// Work queued for the main thread (dev channel commands).
	std::mutex                         postedLock;
	std::vector<std::function<void()>> posted;

	// The game's work for one frame.
	void RunFrame(bool a_onMain)
	{
		if (a_onMain && loading) {
			return;
		}
		CountFrameThread();
		std::vector<std::function<void()>> work;
		{
			std::scoped_lock l{ postedLock };
			work.swap(posted);
		}
		for (auto& fn : work) {
			fn();
		}
		const auto now = std::chrono::steady_clock::now();
		FrameStats::Global().Frame(now);
		if (gameDataReady) {
			DevCommands::Frame();
			Session::Frame();
			Menu::Frame();  // after Session: a button's action sees this frame's connection state
		}
	}

	// The game work runs on the game's main thread, between frames, where the engine runs gameplay
	// itself. The F4SE task runs on the game's job threads, beside the main thread: reading the actor
	// lists from there raced the engine unloading actors (a crash leaving Red Rocket). So the task only
	// asks: once per drawn frame it posts a message to the game's window, and the window procedure, which
	// Windows runs on the thread that made the window (WinMain's), does the work.
	constexpr int     GWLP_WNDPROC = -4;
	constexpr auto    REPOST_AFTER = std::chrono::seconds(2);  // a message that got lost (it shouldn't)
	void*             gameWindow = nullptr;
	std::intptr_t     originalWndProc = 0;
	unsigned int      frameMessage = 0;
	bool              windowFailed = false;
	bool              inFrame = false;  // main thread only: an engine call can pump messages inside ours
	std::atomic<bool> framePosted{ false };
	std::chrono::steady_clock::time_point postedAt{};

	std::intptr_t __stdcall WndProc(void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam)
	{
		if (a_msg == frameMessage && frameMessage != 0) {
			framePosted = false;
			if (!inFrame) {
				inFrame = true;
				// An exception must never escape into the game's message loop.
				try {
					RunFrame(true);
				} catch (const std::exception& e) {
					REX::ERROR("Frame: {}", e.what());
				}
				inFrame = false;
			}
			return 0;
		}
		return CallWindowProcW(originalWndProc, a_window, a_msg, a_wparam, a_lparam);
	}

	bool HookWindow()
	{
		if (gameWindow || windowFailed) {
			return gameWindow != nullptr;
		}
		const auto main = RE::Main::GetSingleton();
		const auto window = main ? reinterpret_cast<void*>(main->hwnd) : nullptr;
		if (!window) {
			return false;  // not made yet
		}
		frameMessage = RegisterWindowMessageW(L"F4Multiplayer.Frame");
		// The original first: the main thread may call ours the moment it is in.
		originalWndProc = GetWindowLongPtrW(window, GWLP_WNDPROC);
		if (frameMessage == 0 || originalWndProc == 0) {
			windowFailed = true;
		} else if (const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<std::intptr_t>(&WndProc)); previous == 0) {
			windowFailed = true;
		} else {
			originalWndProc = previous;
			gameWindow = window;
		}
		if (windowFailed) {
			REX::ERROR("Frame: couldn't hook the game window; game work runs on the task threads");
		} else {
			REX::INFO("Frame: game work runs on the main thread");
		}
		return gameWindow != nullptr;
	}

	// The F4SE permanent task (see NewFrame for when it runs).
	void OnFrame()
	{
		const auto now = std::chrono::steady_clock::now();
		if (!NewFrame(now)) {
			return;
		}
		if (!HookWindow()) {
			RunFrame(false);  // no window yet (startup), or it couldn't be hooked
			return;
		}
		if (!framePosted.exchange(true) || now - postedAt > REPOST_AFTER) {
			postedAt = now;
			if (!PostMessageW(gameWindow, frameMessage, 0, 0)) {
				framePosted = false;
			}
		}
	}
}

namespace MainThread
{
	void Post(std::function<void()> a_fn)
	{
		std::scoped_lock l{ postedLock };
		posted.push_back(std::move(a_fn));
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	mainThread = std::this_thread::get_id();

	REX::INFO("F4Multiplayer loaded");
	if constexpr (sizeof(F4MP_BUILD_LABEL) > 1) {
		REX::INFO("F4Multiplayer build {}", F4MP_BUILD_LABEL);  // a release candidate names itself
	}

	Config::Load();

	if (!F4SE::GetMessagingInterface()->RegisterListener(OnF4SEMessage)) {
		REX::ERROR("Failed to register F4SE message listener");
		return false;
	}

	F4SE::GetTaskInterface()->AddTaskPermanent(OnFrame);

	if (const auto serialization = F4SE::GetSerializationInterface()) {
		serialization->SetUniqueID(SERIALIZATION_ID);
		serialization->SetSaveCallback(OnGameSaved);
		serialization->SetLoadCallback(OnGameLoaded);
		serialization->SetRevertCallback(OnRevert);
	} else {
		REX::WARN("No serialization interface: loading a save mid-session may duplicate shared loot");
	}

	DevChannel::Start();

	return true;
}

namespace FrameStats
{
	Tracker& Global()
	{
		static Tracker tracker;
		return tracker;
	}

	std::string TakeThreads()
	{
		std::scoped_lock l{ frameThreadsLock };
		const auto t = std::exchange(frameThreads, FrameThreads{ .lastDrawn = frameThreads.lastDrawn });
		return std::format("main={} (repeats={} drawing={}) other={} (repeats={} drawing={} threads={})", t.mainCalls, t.mainRepeats, t.mainDrawing, t.otherCalls,
			t.otherRepeats, t.otherDrawing, t.others.size());
	}
}
