#include "Config.h"
#include "DevChannel.h"
#include "DevCommands.h"
#include "game/Compass.h"
#include "game/ExitMusic.h"
#include "game/FastLoad.h"
#include "game/Hud.h"
#include "game/WorldSync.h"
#include "game/FrameStats.h"
#include "game/Menu.h"
#include "HangWatch.h"
#include "MainThread.h"
#include "game/SaveGuard.h"
#include "game/SaveLoadHold.h"
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

	// A save or a game load is under way: from F4SE's message before it to the one after (SaveLoadHold).
	// The engine may pump window messages inside either, a load runs on a thread of its own while the main
	// thread goes on running frames, and a load starts before its loading screen's event comes: game work
	// then would spawn the stand-ins just taken out for the save (into it), or change the world under the
	// load. It waits for the engine to finish; a message that never came stops it only for
	// SAVE_OR_LOAD_LIMIT.
	constexpr auto SAVE_OR_LOAD_LIMIT = std::chrono::seconds(60);
	// How long a load's thread waits for the main thread to do the game work it hands over.
	constexpr auto MAIN_THREAD_WAIT = std::chrono::seconds(2);
	constexpr auto WAKE_EVERY = std::chrono::milliseconds(100);  // a wake can be skipped (inside a frame)

	bool OnMainThread()
	{
		return std::this_thread::get_id() == mainThread;
	}

	void WakeMainThread();  // with the window, below

	// The hang watch gives a save or load the same longer limit as a loading screen.
	void BeginSaveOrLoad(HangWatch::Phase a_what)
	{
		SaveLoadHold::Global().Begin(std::chrono::steady_clock::now(), [&] { HangWatch::SetSaveOrLoad(a_what); });
	}

	void EndSaveOrLoad(std::uint64_t a_hold, std::string_view a_what)
	{
		// Only the hold that ends tells the hang watch play is back: not the end of an older one (a game
		// loaded again before the last load's own work had run), with the newer load still under way. Both
		// phases are set under the hold's lock, so a newer load's, begun in between, is never written over.
		if (const auto held = SaveLoadHold::Global().End(a_hold, [] { HangWatch::SetSaveOrLoad(HangWatch::Phase::kPlay); })) {
			if (*held) {
				REX::INFO("Frame: {} frame(s) held during the {}", *held, a_what);
			}
		}
	}

	// Game work a save's or load's message needs done before the engine goes on, done on the main thread
	// between frames. A save's message comes on the main thread: it runs at once, after the work an earlier
	// message handed over (a load's, should the save come before the frame after it). A load's comes on the
	// load's own thread while the main thread may be in a frame: that thread hands the work over and waits
	// here, so the frame in progress finishes first, no game object is touched from two threads, and the
	// stand-ins are out before the load starts. Should the main thread not come in MAIN_THREAD_WAIT (busy in
	// the engine's own load work), the load's thread does it itself, once no frame is working, as it always
	// did, rather than hold the load up any longer.
	void RunBeforeSaveOrLoad(SaveLoadHold::Hold::Work a_work, std::string_view a_what)
	{
		auto& hold = SaveLoadHold::Global();
		if (OnMainThread()) {
			hold.RunNow(a_work);  // what an earlier message handed over goes first
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		const auto waited = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };
		hold.Post(std::move(a_work));
		do {
			WakeMainThread();
			if (hold.WaitForFrames(WAKE_EVERY)) {
				REX::INFO("Frame: the main thread took the stand-ins out for the {} ({} ms)", a_what, waited());
				return;
			}
		} while (std::chrono::steady_clock::now() - start < MAIN_THREAD_WAIT);
		auto       left = hold.TakeBack();
		const bool idle = hold.WaitForFrames(MAIN_THREAD_WAIT);
		if (left.empty()) {
			REX::WARN("Frame: the main thread took the stand-ins out for the {} late ({} ms{})", a_what, waited(), idle ? "" : ", and is still at it");
			return;
		}
		REX::WARN("Frame: the main thread didn't come in {} ms; the {}'s thread takes the stand-ins out{}", waited(), a_what, idle ? "" : " beside a frame still working");
		for (auto& fn : left) {
			fn();
		}
	}

	// Game work after a load (or a new game), done on the main thread before its next frame's game work;
	// the hold ends after it, so no frame works between the load and the session starting over. A load's
	// thread hands it over and goes on.
	void RunAfterLoad(SaveLoadHold::Hold::Work a_work)
	{
		auto& hold = SaveLoadHold::Global();
		if (OnMainThread()) {
			hold.RunNow(a_work);
			return;
		}
		hold.Post(std::move(a_work));
		WakeMainThread();
	}

	// Co-save records: which of the session's changes each save contains. The callbacks run inside the
	// save or load, on its thread, and touch only the plugin's own records; the hold keeps frames out then.
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
			Compass::Install();
			Hud::Install();  // the loading screen watcher, for the frame and the hang watch too
			ExitMusic::Install();  // quitting crashed in the cells' music check after the region state was gone
			// Quitting ended the server thread without its last save (up to 15 s of kills and loot lost each time).
			ExitMusic::AtShutdown([] {
				try {
					Session::Shutdown();
				} catch (const std::exception& e) {
					REX::ERROR("Session: shutting down: {}", e.what());
				}
			});
			Session::Initialize();
			gameDataReady = true;
			REX::INFO("Game data ready");
			HangWatch::Start();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
			BeginSaveOrLoad(HangWatch::Phase::kLoad);  // the HUD holds from here too: it is torn down before the loading screen's event comes
			RunBeforeSaveOrLoad([] { Session::OnBeforeSaveOrLoad(); }, "load");
			break;
		case F4SE::MessagingInterface::kPreSaveGame:
			BeginSaveOrLoad(HangWatch::Phase::kSave);
			// Remote players' actors must never be written into a save. A save made before the first frame
			// after a load runs that load's own work first (RunAfterLoad), or the stand-ins it took out
			// would be the ones from before the load.
			RunBeforeSaveOrLoad(
				[] {
					Session::OnBeforeSaveOrLoad();
					SaveGuard::BeforeSave();
				},
				"save");
			break;
		case F4SE::MessagingInterface::kPostSaveGame:
			EndSaveOrLoad(SaveLoadHold::Global().Newest(), "save");
			break;
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			RunAfterLoad([loaded = a_msg->type == F4SE::MessagingInterface::kPostLoadGame, hold = SaveLoadHold::Global().Newest()] {
				// An exception must not keep the hold from ending.
				try {
					if (gameDataReady) {
						if (loaded) {
							SaveGuard::AfterLoad();
						}
						Session::OnGameLoaded();
						Hud::OnGameLoaded();
					}
				} catch (const std::exception& e) {
					REX::ERROR("Frame: after the load: {}", e.what());
				}
				EndSaveOrLoad(hold, "load");
			});
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
		// What a save or load handed over (RunBeforeSaveOrLoad, RunAfterLoad) comes first, held or not.
		auto& hold = SaveLoadHold::Global();
		hold.RunPosted();
		// A loading screen is up. The engine may pump window messages from inside its load code, and game
		// work must not run in the middle of that; it waits for the load to finish.
		if (a_onMain && Hud::LoadingScreenUp()) {
			return;
		}
		// Entered under the hold's lock: a load that begins from here on waits for this frame to finish.
		const SaveLoadHold::Frame frame{ hold, std::chrono::steady_clock::now() };
		if (!frame) {
			return;  // a save or load is under way (counted, and logged after it)
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
	constexpr int            GWLP_WNDPROC = -4;
	constexpr auto           REPOST_AFTER = std::chrono::seconds(2);  // a message that got lost (it shouldn't)
	constexpr std::uintptr_t WAKE = 1;                                // the message's wparam: only a save's or load's work
	std::atomic<void*>       gameWindow{ nullptr };                   // also read by a load's thread (WakeMainThread)
	std::intptr_t            originalWndProc = 0;
	unsigned int             frameMessage = 0;
	bool                     windowFailed = false;
	bool                     inFrame = false;  // main thread only: an engine call can pump messages inside ours
	std::atomic<bool>        framePosted{ false };
	std::chrono::steady_clock::time_point postedAt{};

	// Has the main thread do what a save or load handed over (and only that) now: the frame task may not
	// run while the engine loads.
	void WakeMainThread()
	{
		if (const auto window = gameWindow.load()) {
			PostMessageW(window, frameMessage, WAKE, 0);
		}
	}

	std::intptr_t __stdcall WndProc(void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam)
	{
		if (a_msg == frameMessage && frameMessage != 0) {
			const bool wake = a_wparam == WAKE;
			if (!wake) {
				framePosted = false;
			}
			if (!inFrame) {
				inFrame = true;
				// An exception must never escape into the game's message loop.
				try {
					if (wake) {
						SaveLoadHold::Global().RunPosted();
					} else {
						RunFrame(true);
					}
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
		if (gameWindow.load() || windowFailed) {
			return gameWindow.load() != nullptr;
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
		return gameWindow.load() != nullptr;
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
			if (!PostMessageW(gameWindow.load(), frameMessage, 0, 0)) {
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

namespace SaveLoadHold
{
	Hold& Global()
	{
		static Hold hold{ SAVE_OR_LOAD_LIMIT };
		return hold;
	}
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
