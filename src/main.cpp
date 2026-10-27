#include "Config.h"
#include "DevChannel.h"
#include "DevCommands.h"
#include "game/AutoSave.h"
#include "game/Equipment.h"
#include "game/Downed.h"
#include "game/Drops.h"
#include "game/LooksMenu.h"
#include "game/RadiantSync.h"
#include "game/ExitMusic.h"
#include "game/Face.h"
#include "game/FastLoad.h"
#include "game/FrameWatch.h"
#include "game/GhostLedger.h"
#include "game/GpuWatch.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/InputGate.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"
#include "game/FrameStats.h"
#include "game/Perf.h"
#include "game/Menu.h"
#include "game/MenuWatch.h"
#include "game/OpenMenus.h"
#include "game/RealTime.h"
#include "game/RemotePlayers.h"
#include "HangWatch.h"
#include "Background.h"
#include "Log.h"
#include "MainThread.h"
#include "Platform.h"
#include "NestedPump.h"
#include "game/SaveGuard.h"
#include "game/SaveLoadHold.h"
#include "game/Storage.h"
#include "game/HavokGuard.h"
#include "game/InertShots.h"
#include "game/TextureWait.h"
#include "game/WorkshopSync.h"
#include "net/Session.h"
#include "Text.h"

// user32 (windows.h clashes with CommonLibF4's names).
extern "C" {
__declspec(dllimport) std::intptr_t __stdcall GetWindowLongPtrW(void* a_window, int a_index);
__declspec(dllimport) std::intptr_t __stdcall SetWindowLongPtrW(void* a_window, int a_index, std::intptr_t a_value);
__declspec(dllimport) std::intptr_t __stdcall CallWindowProcW(std::intptr_t a_proc, void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam);
__declspec(dllimport) int __stdcall PostMessageW(void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam);
__declspec(dllimport) unsigned int __stdcall RegisterWindowMessageW(const wchar_t* a_name);
__declspec(dllimport) short __stdcall GetAsyncKeyState(int a_key);
}

// kernel32, for the stack check (see NestedPump).
extern "C" {
__declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long a_skip, unsigned long a_count, void** a_frames, unsigned long* a_hash);
__declspec(dllimport) void* __stdcall RtlLookupFunctionEntry(std::uint64_t a_address, std::uint64_t* a_imageBase, void* a_history);
}

namespace
{
	bool gameDataReady = false;

	// Set at the game's shutdown stage (ExitMusic::AtShutdown), on the main thread, as Main::Shutdown begins: from there
	// on the engine tears the world down, and no game work of ours runs (see Quitting).
	std::atomic<bool> shutdownStage{ false };

	// Whether the game is quitting: the shutdown stage has come, or the engine has set Main's quit flag (the menu,
	// the console, a script, or the window being destroyed), on which it leaves its main loop for Main::Shutdown.
	// Main itself is deleted late in that shutdown and its pointer cleared, after the stage: it is not read then.
	bool Quitting()
	{
		if (shutdownStage.load(std::memory_order_relaxed)) {
			return true;
		}
		const auto main = RE::Main::GetSingleton();
		return !main || std::atomic_ref{ main->quitGame }.load(std::memory_order_relaxed);
	}

	// The thread the plugin was loaded on: the game's main thread (WinMain).
	std::thread::id mainThread;
	std::atomic<bool>          mainThreadChecked{ false };  // MainThread::Capture
	std::atomic<std::uint32_t> offThreadCalls{ 0 };

	// Which threads run the frame's game work (see FrameStats::TakeThreads): plain counters behind a lock.
	struct FrameThreads
	{
		std::uint32_t             mainCalls = 0, otherCalls = 0;
		std::uint32_t             mainRepeats = 0, otherRepeats = 0;   // no new drawn frame since the call before
		std::uint32_t             mainDrawing = 0, otherDrawing = 0;   // the renderer was inside a frame
		std::uint32_t             lastDrawn = 0;
		std::set<std::thread::id> others;
		std::uint32_t             deferred = 0;  // frame messages put off inside the engine's pump (see NestedPump)
		NestedPump::Check         pumpCheck = NestedPump::Check::kUnknown;
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

	void WakeMainThread();   // with the window, below
	bool WindowFailed();     // likewise: the window could not be hooked, and nothing serves work handed over
	void StartFrameWatch();  // likewise: the watch on the frame chain (FrameWatch.h)

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
		if (WindowFailed()) {
			// No frame runs to take it (multiplayer is off this run): this thread does it at once, as it would after
			// MAIN_THREAD_WAIT, rather than hold every load up for it.
			hold.RunNow(a_work);
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
		if (OnMainThread() || WindowFailed()) {
			hold.RunNow(a_work);  // with no window hooked, nothing else would: the hold would never end
			return;
		}
		hold.Post(std::move(a_work));
		WakeMainThread();
	}

	// Co-save records: which of the session's changes each save contains. The callbacks run inside the
	// save or load, on its thread, and touch only the plugin's own records; the hold keeps frames out then.
	constexpr std::uint32_t SERIALIZATION_ID = 'F4MP';

	constexpr std::uint32_t DROP_RECORD = 'DROP';
	constexpr std::uint32_t DROP_VERSION = 1;
	constexpr std::uint32_t DROP_MAX_LENGTH = 1u << 20;

	constexpr std::uint32_t RADIANT_RECORD = 'RADI';
	constexpr std::uint32_t RADIANT_VERSION = 1;
	constexpr std::uint32_t RADIANT_MAX_LENGTH = 1u << 18;  // RadiantRules::Encode writes at most about 80 KB

	constexpr std::uint32_t KEPT_RECORD = 'KEPT';
	constexpr std::uint32_t KEPT_VERSION = 1;
	constexpr std::uint32_t KEPT_MAX_LENGTH = 1u << 10;  // FriendFrame::Encode writes at most 768 bytes
	constexpr std::uint32_t LOOKS_RECORD = 'LOOK';
	constexpr std::uint32_t LOOKS_VERSION = 1;
	constexpr std::uint32_t LOOKS_MAX_LENGTH = 2 + 4 * 512;  // LooksSave::Encode: a count and at most BodyMorphs::MAX_SAVED IDs
	constexpr std::uint32_t GHOST_RECORD = 'GHST';
	constexpr std::uint32_t GHOST_VERSION = 1;
	constexpr std::uint32_t GHOST_MAX_LENGTH = 4 + 4 * static_cast<std::uint32_t>(GhostLedger::MAX);  // GhostLedger::Encode: a version, a count, the ids
	constexpr std::uint32_t STORAGE_RECORD = 'STOR';
	constexpr std::uint32_t STORAGE_VERSION = 2;  // 1 (unreleased builds): the moves alone
	// Storage::SaveRecord: a session, a count, the moves, where its lacking moves start, a count, their numbers.
	constexpr std::uint32_t STORAGE_MAX_LENGTH = 12 + 12 * static_cast<std::uint32_t>(StorageView::MAX_IN_FLIGHT) + 8 + 4 * static_cast<std::uint32_t>(Protocol::MAX_STORAGE_LACKING);

	void OnGameSaved(const F4SE::SerializationInterface* a_intfc)
	{
		// The frame copies this save keeps with the companions in them (RemotePlayers::BeforeSave): waited on again after loading it.
		if (const auto kept = RemotePlayers::SaveRecord(); !kept.empty() && a_intfc->OpenRecord(KEPT_RECORD, KEPT_VERSION)) {
			a_intfc->WriteRecordData(kept.data(), static_cast<std::uint32_t>(kept.size()));
		}
		WorldSync::SaveResyncPoint(a_intfc);
		// What our world holds of the items given between players (Drops): read back with the save.
		if (const auto drops = Drops::SaveRecord(); !drops.empty() && a_intfc->OpenRecord(DROP_RECORD, DROP_VERSION)) {
			a_intfc->WriteRecordData(drops.data(), static_cast<std::uint32_t>(drops.size()));
		}
		// The radiant runs our world shared in its session and those finished here (RadiantSync): read back with the save.
		if (const auto radiants = RadiantSync::SaveRecord(); !radiants.empty() && a_intfc->OpenRecord(RADIANT_RECORD, RADIANT_VERSION)) {
			a_intfc->WriteRecordData(radiants.data(), static_cast<std::uint32_t>(radiants.size()));
		}
		// Our moves at a friend's shared storage still unanswered, and those numbered our world lacks (Storage): settled after a load.
		if (const auto storage = Storage::SaveRecord(); !storage.empty() && a_intfc->OpenRecord(STORAGE_RECORD, STORAGE_VERSION)) {
			a_intfc->WriteRecordData(storage.data(), static_cast<std::uint32_t>(storage.size()));
		}
		// The form IDs LooksMenu may still keep a friend's looks under in this same save (LooksMenu): cleared after its load.
		if (const auto looks = LooksMenu::SaveRecord(); !looks.empty() && a_intfc->OpenRecord(LOOKS_RECORD, LOOKS_VERSION)) {
			a_intfc->WriteRecordData(looks.data(), static_cast<std::uint32_t>(looks.size()));
		}
		// The ghosts we set against friends' explosions and have yet to see off (WeaponFire, GhostLedger): taken off as
		// each copy loads after a load of this save.
		if (const auto ghosts = WeaponFire::SaveGhostRecord(); !ghosts.empty() && a_intfc->OpenRecord(GHOST_RECORD, GHOST_VERSION)) {
			a_intfc->WriteRecordData(ghosts.data(), static_cast<std::uint32_t>(ghosts.size()));
		}
	}

	void OnGameLoaded(const F4SE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == 'CONT') {
				WorldSync::LoadResyncPoint(a_intfc, version, length);
			} else if (type == DROP_RECORD && version == DROP_VERSION && length <= DROP_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					Drops::LoadRecord(bytes);
				}
			} else if (type == KEPT_RECORD && version == KEPT_VERSION && length <= KEPT_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					RemotePlayers::LoadRecord(bytes);
				}
			} else if (type == RADIANT_RECORD && version == RADIANT_VERSION && length <= RADIANT_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					RadiantSync::LoadRecord(bytes);
				}
			} else if (type == STORAGE_RECORD && version == STORAGE_VERSION && length <= STORAGE_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					Storage::LoadRecord(bytes);
				}
			} else if (type == LOOKS_RECORD && version == LOOKS_VERSION && length <= LOOKS_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					LooksMenu::LoadRecord(bytes);
				}
			} else if (type == GHOST_RECORD && version == GHOST_VERSION && length <= GHOST_MAX_LENGTH) {
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) == length) {
					WeaponFire::LoadGhostRecord(bytes, [&](std::uint32_t a_id) { return a_intfc->ResolveFormID(a_id); });
				}
			}
		}
	}

	void OnRevert(const F4SE::SerializationInterface*)
	{
		WorldSync::RevertResyncPoint();
		Drops::RevertRecord();
		RadiantSync::RevertRecord();
		RemotePlayers::RevertRecord();
		LooksMenu::RevertRecord();
		Storage::RevertRecord();
		WeaponFire::RevertGhostRecord();
	}

	// bRunInBackground: while a session runs, the game runs on while its window is not focused (its own bAlwaysActive),
	// or a player who alt-tabbed stood still for friends, with the NPCs their game runs. Out of a session the setting is
	// back as the player's ini has it (see Background). Looked at once a frame.
	Background::Switch background;

	RE::Setting* AlwaysActive()
	{
		static RE::Setting* const found = [] {
			const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
			const auto inPrefs = prefs ? prefs->GetSetting("bAlwaysActive:General") : nullptr;
			const auto setting = inPrefs ? inPrefs : RE::GetINISetting("bAlwaysActive:General");
			if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
				REX::WARN("Config: the game's bAlwaysActive setting was not found; the game pauses while it is not focused");
				return static_cast<RE::Setting*>(nullptr);
			}
			REX::INFO("Config: the game's bAlwaysActive is in its {} settings, {} now", inPrefs ? "Prefs ini" : "ini", setting->GetBinary());
			return setting;
		}();
		return found;
	}

	// Hosting, or joined to someone's.
	bool InSession()
	{
		const auto status = Session::GetStatus();
		return status.hosting || status.link == Session::Status::Link::kJoined;
	}

	void RunInBackground()
	{
		const auto setting = AlwaysActive();
		if (!setting) {
			return;
		}
		const bool session = InSession();
		if (const auto value = background.Update(Config::Get().runInBackground, session, setting->GetBinary())) {
			setting->SetBinary(*value);
			REX::INFO("Config: {}", *value ? "in a session, the game runs on while it is not focused (bRunInBackground)" : "out of the session, the game's bAlwaysActive is back off");
		}
	}

	// Running on unfocused, the game must not read the mouse, keys or pad meant for another window (see InputGate).
	// Main thread only: the game polls its devices there.
	InputGate::Gate inputGate;
	constexpr std::uint32_t INPUT_GATE_LOGGED = 3;  // the first few closes and opens go to the log
	std::uint32_t           inputGateLogged = 0;

	bool MouseButtonDown()
	{
		// VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
		for (const int vk : { 0x01, 0x02, 0x04, 0x05, 0x06 }) {
			if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
				return true;
			}
		}
		return false;
	}

	void GateInput()
	{
		const auto input = RE::BSInputDeviceManager::GetSingleton();
		const auto setting = AlwaysActive();
		if (!input || !setting) {
			return;
		}
		const bool enabled = setting->GetBinary();
		// Off, the window matters only while the gate is closed (see InputGate::Gate::Frame).
		const bool front = (enabled || inputGate.Now() != InputGate::Gate::State::kOpen) ? Hotkeys::GameInFront() : true;
		const auto step = inputGate.Frame(enabled, front, front && enabled && MouseButtonDown());
		if (step.polling) {
			input->pollingEnabled = *step.polling;
		}
		if (step.clear) {
			for (const auto device : input->devices) {
				if (device) {
					device->ClearInputState();
				}
			}
		}
		if ((step.closed || step.opened) && inputGateLogged < INPUT_GATE_LOGGED * 2) {
			++inputGateLogged;
			REX::INFO("Input: {}", step.closed ? "the game's window is not in front: no input reaches the game until it is (it runs on)" :
			                                     std::format("the game's window is in front: input goes to the game again ({})", inputGate.Describe()));
		}
	}

	// The text an F4SE message carries: a save's name, or the file path of the save a load reads; empty with none.
	std::string_view MessageText(const F4SE::MessagingInterface::Message* a_msg)
	{
		constexpr std::uint32_t LONGEST = 4096;  // more is no name
		if (!a_msg->data || a_msg->dataLen == 0 || a_msg->dataLen > LONGEST) {
			return {};
		}
		return { static_cast<const char*>(a_msg->data), a_msg->dataLen };
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			FastLoad::Install();  // checks bFastLoading itself, per loading screen
			OpenMenus::Install();  // which menus are open, for everything that asks: the UI itself is never asked
			if (Config::Get().devChannel) {
				MenuWatch::Install();  // for the stress loop's menu checks (the menus dev command)
			}
			Hud::Install();  // the loading screen watcher, for the frame and the hang watch too
			ExitMusic::Install();  // quitting crashed in the cells' music check after the region state was gone
			// Quitting ended the server thread without its last save (up to 15 s of kills and loot lost each time).
			ExitMusic::AtShutdown([] {
				shutdownStage = true;  // first: a frame message the engine dispatches from here on runs nothing
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
			StartFrameWatch();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
			// Which save is loaded (a death loads the newest one): the line a report of "an old save" is read by.
			AutoSave::OnPreLoad(MessageText(a_msg));
			BeginSaveOrLoad(HangWatch::Phase::kLoad);  // the HUD holds from here too: it is torn down before the loading screen's event comes
			RunBeforeSaveOrLoad(
				[] {
					WeaponFire::BeforeLoad();  // first: the shields Session ends below would send setghost 0 into the load
					Session::OnBeforeLoad();
					Session::OnBeforeSaveOrLoad();
					HavokGuard::BeforeSaveOrLoad();  // a reset under way is ended (enabled) before the load
					Face::ForgetCopies();  // the load sets runtime form IDs back: a mark could name another NPC
					InertShots::BeforeLoad();  // after DespawnAll: every inert shot in flight is handed to the load, which frees it
					SaveGuard::BeforeLoad();  // after DespawnAll (OnBeforeSaveOrLoad): what of ours is on its way out as the load comes
					RemotePlayers::BeforeLoad();  // after DespawnAll too: who was being sent out of a friend's frame copy is forgotten
				},
				"load");
			break;
		case F4SE::MessagingInterface::kPreSaveGame:
			AutoSave::OnPreSave(MessageText(a_msg));  // which save, and whether it is one the mod asked for
			BeginSaveOrLoad(HangWatch::Phase::kSave);
			// Remote players' actors must never be written into a save. A save made before the first frame
			// after a load runs that load's own work first (RunAfterLoad), or the stand-ins it took out
			// would be the ones from before the load.
			// An exception must not go on into the engine's save, which would crash the game and lose the save; each
			// step is caught on its own (and each of the session's, Session::OnBeforeSaveOrLoad), so one that fails does not
			// leave the others' objects in the save.
			try {
				RunBeforeSaveOrLoad(
					[] {
						const auto step = [](std::string_view a_what, auto&& a_work) {
							try {
								a_work();
							} catch (const std::exception& e) {
								REX::ERROR("Frame: before the save, {}: {}", a_what, e.what());
							} catch (...) {
								REX::ERROR("Frame: before the save, {}: an unknown exception", a_what);
							}
						};
						step("the session", [] { Session::OnBeforeSaveOrLoad(); });
						step("the ghosts", [] { WeaponFire::HideGhostsFromSave(); });  // after the shields end there: a ghost against a friend's explosion is never saved
						step("Havok", [] { HavokGuard::BeforeSaveOrLoad(); });  // a reference reset a moment ago is saved on, as it was
						step("the inert shots", [] { InertShots::BeforeSave(); });  // no inert shot, whose base exists at runtime only, is written into the save
						step("the strays", [] { SaveGuard::BeforeSave(); });
						step("the frame copies", [] { RemotePlayers::BeforeSave(); });  // after DespawnAll: the copies kept with their wearers are what a load of this save waits on
						step("the workshops", [] { WorkshopSync::BeforeSave(); });  // no power grid saved names a friend's piece
					},
					"save");
			} catch (const std::exception& e) {
				// The work an earlier message handed over, run first (SaveLoadHold::Hold::RunNow), threw.
				REX::ERROR("Frame: before the save: {}", e.what());
			} catch (...) {
				REX::ERROR("Frame: before the save: an unknown exception");
			}
			break;
		case F4SE::MessagingInterface::kPostSaveGame:
			AutoSave::OnPostSave(MessageText(a_msg));
			if (gameDataReady) {
				// A downed player's flags, cleared for the save, go back on before the engine's next update can stand them up.
				// (WeaponFire::AfterSave waits for the next frame: no console command is sent from the save's own message.)
				if (OnMainThread()) {
					Downed::OnAfterSave();
				} else {
					MainThread::Post([] { Downed::OnAfterSave(); });
				}
			}
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
						} else {
							SaveGuard::NewGame();
						}
						Session::OnGameLoaded(!loaded);
						Hud::OnGameLoaded();
						HavokGuard::OnGameLoaded();
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

	// The frame watch's view of the chain (FrameWatch.h): when each link last ran, and how the last frame
	// message ended. Stamped where each runs, read by the watch's thread.
	std::atomic<std::chrono::steady_clock::time_point> taskAt{};        // the F4SE task (OnFrame)
	std::atomic<std::chrono::steady_clock::time_point> dispatchedAt{};  // the window procedure got a frame message
	std::atomic<std::chrono::steady_clock::time_point> workedAt{};      // a frame's game work ran (RunFrame)
	std::atomic<FrameWatch::Outcome>                   lastOutcome{ FrameWatch::Outcome::kNone };

	// The game's work for one frame. Returns how it ended, for the frame watch.
	FrameWatch::Outcome RunFrame(bool a_onMain)
	{
		// What a save or load handed over (RunBeforeSaveOrLoad, RunAfterLoad) comes first, held or not.
		auto& hold = SaveLoadHold::Global();
		hold.RunPosted();
		// A loading screen is up. The engine may pump window messages from inside its load code, and game
		// work must not run in the middle of that; it waits for the load to finish.
		if (a_onMain && Hud::LoadingScreenUp()) {
			return FrameWatch::Outcome::kLoadingScreen;
		}
		// Entered under the hold's lock: a load that begins from here on waits for this frame to finish.
		const SaveLoadHold::Frame frame{ hold, std::chrono::steady_clock::now() };
		if (!frame) {
			return FrameWatch::Outcome::kHeld;  // a save or load is under way (counted, and logged after it)
		}
		workedAt = std::chrono::steady_clock::now();
		CountFrameThread();
		// Our work's cost per frame, part by part (the dev channel's `perf`): on the main thread only, as a frame there is one
		// of the game's.
		std::optional<Perf::Frame> perfFrame;
		if (a_onMain) {
			perfFrame.emplace();
		}
		std::vector<std::function<void()>> work;
		{
			std::scoped_lock l{ postedLock };
			work.swap(posted);
		}
		for (auto& fn : work) {
			PERF_SCOPE("Posted");
			fn();
		}
		const auto now = std::chrono::steady_clock::now();
		FrameStats::Global().Frame(now);
		if (gameDataReady) {
			{
				PERF_SCOPE("DevCommands");
				DevCommands::Frame();
			}
			Session::Frame();  // its parts are timed inside it
			{
				PERF_SCOPE("Background");
				RunInBackground();
				if (a_onMain) {
					GateInput();
				}
			}
			{
				PERF_SCOPE("RealTime");
				RealTime::Frame(InSession());
			}
			{
				PERF_SCOPE("HavokGuard");
				HavokGuard::Frame();
			}
			InertShots::Frame();  // inert shots that are stale or in a cell no longer attached are ended
			PERF_SCOPE("Menu");
			Menu::Frame();  // after Session: a button's action sees this frame's connection state
		}
		return FrameWatch::Outcome::kWorked;
	}

	// The game work runs on the game's main thread, between frames, where the engine runs gameplay
	// itself. The F4SE task runs on the game's job threads, beside the main thread: reading the actor
	// lists from there raced the engine unloading actors (a crash leaving Red Rocket). So the task only
	// asks: once per drawn frame it posts a message to the game's window, and the window procedure, which
	// Windows runs on the thread that made the window (WinMain's), does the work.
	constexpr int            GWLP_WNDPROC = -4;
	constexpr auto           REPOST_AFTER = std::chrono::seconds(2);  // a message that got lost (it shouldn't)
	constexpr std::uintptr_t WAKE = 1;                                // the message's wparam: only a save's or load's work
	std::atomic<void*>         gameWindow{ nullptr };                   // also read by a load's thread (WakeMainThread)
	std::atomic<std::intptr_t> originalWndProc{ 0 };                   // the frame watch may hook a new window (CheckWindow)
	unsigned int               frameMessage = 0;
	std::atomic<bool>          windowFailed{ false };                 // read by net status (MainThread::HookState)
	bool                       inFrame = false;  // main thread only: an engine call can pump messages inside ours
	bool                       quitSeen = false;  // main thread only: a message came after the game began quitting (logged once)
	NestedPump::Check          pumpCheck = NestedPump::Check::kUnknown;  // main thread only
	std::atomic<bool>          framePosted{ false };
	std::atomic<std::chrono::steady_clock::time_point> postedAt{};  // the task's job threads, and the frame watch

	// Has the main thread do what a save or load handed over (and only that) now: the frame task may not
	// run while the engine loads.
	void WakeMainThread()
	{
		if (const auto window = gameWindow.load()) {
			PostMessageW(window, frameMessage, WAKE, 0);
		}
	}

	// The code of the function at a_address, from the game's unwind table (none when it has no entry, or a_address is
	// not where its entry begins).
	NestedPump::Range FunctionAt(std::uintptr_t a_address)
	{
		struct RuntimeFunction
		{
			std::uint32_t begin;
			std::uint32_t end;
			std::uint32_t unwind;
		};
		std::uint64_t base = 0;
		const auto    entry = static_cast<const RuntimeFunction*>(RtlLookupFunctionEntry(a_address, &base, nullptr));
		if (!entry || base + entry->begin != a_address) {
			return {};
		}
		return { base + entry->begin, base + entry->end };
	}

	// Where on the main thread's stack our frame message is being dispatched from (see NestedPump), by Address Library
	// ID (1.11.240): PositionPlayerJob::Run (+C3B030), the engine's pump while it moves the player; the game's message
	// loop, Main::Run_WindowsMessageLoop (+C336B0), and Main::Run (+C314F0), which calls it.
	NestedPump::Seen WhereDispatched()
	{
		static const NestedPump::Range pump = FunctionAt(REL::ID(2228998).address());
		static const std::array<NestedPump::Range, 2> outer{ FunctionAt(REL::ID(2228915).address()), FunctionAt(REL::ID(4484191).address()) };
		void*      frames[48];
		const auto count = RtlCaptureStackBackTrace(1, static_cast<unsigned long>(std::size(frames)), frames, nullptr);
		std::array<std::uintptr_t, std::size(frames)> addresses{};
		for (unsigned short i = 0; i < count; ++i) {
			addresses[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
		}
		const auto seen = NestedPump::Classify(std::span{ addresses.data(), count }, pump, outer);
		if (NestedPump::Decide(pumpCheck, seen)) {
			if (pumpCheck == NestedPump::Check::kOk) {
				REX::INFO("Frame: the stack check finds the game's message loop; frame messages inside the engine's own pump are put off");
			} else {
				REX::WARN("Frame: the stack check finds neither the game's message loop nor the engine's pump ({} frames); no frame is put off", count);
			}
			std::scoped_lock l{ frameThreadsLock };
			frameThreads.pumpCheck = pumpCheck;
		}
		return seen;
	}

	// What the window procedure calls for our message (see NestedPump::Dispatch).
	struct WindowCalls
	{
		void             Taken() { framePosted = false; }  // the next drawn frame posts another
		bool             Quitting() { return ::Quitting(); }
		NestedPump::Seen Where() { return WhereDispatched(); }
		void             Posted() { SaveLoadHold::Global().RunPosted(); }
		void             Frame() { lastOutcome = RunFrame(true); }
		void             Deferred()
		{
			std::scoped_lock l{ frameThreadsLock };
			++frameThreads.deferred;
		}
	};

	std::intptr_t __stdcall WndProc(void* a_window, unsigned int a_msg, std::uintptr_t a_wparam, std::intptr_t a_lparam)
	{
		if (a_msg == frameMessage && frameMessage != 0) {
			const bool wake = a_wparam == WAKE;
			if (!wake) {
				dispatchedAt = std::chrono::steady_clock::now();
			}
			// An exception must never escape into the game's message loop, the stack check's included.
			try {
				WindowCalls calls;
				const auto  action = NestedPump::Dispatch(wake, inFrame, calls);
				if (!wake) {
					// How a frame message ended, for the frame watch (a frame that ran told RunFrame's own outcome).
					switch (action) {
					case NestedPump::Action::kSkip:
						lastOutcome = FrameWatch::Outcome::kSkipped;
						break;
					case NestedPump::Action::kDefer:
						lastOutcome = FrameWatch::Outcome::kDeferred;
						break;
					case NestedPump::Action::kQuit:
						lastOutcome = FrameWatch::Outcome::kQuit;
						break;
					default:
						break;
					}
				}
				if (action == NestedPump::Action::kQuit && !std::exchange(quitSeen, true)) {
					REX::INFO("Frame: the game is quitting; no more game work runs");
				}
			} catch (const std::exception& e) {
				REX::ERROR("Frame: {}", e.what());
			}
			return 0;
		}
		return CallWindowProcW(originalWndProc.load(std::memory_order_relaxed), a_window, a_msg, a_wparam, a_lparam);
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
			REX::ERROR("Frame: couldn't hook the game window; multiplayer is off this run");
		} else {
			REX::INFO("Frame: game work runs on the main thread");
			MainThread::Capture();
		}
		return gameWindow.load() != nullptr;
	}

	bool WindowFailed()
	{
		return windowFailed;
	}

	// The frame watch (FrameWatch.h), on a thread of its own from the game's data being ready until the
	// game quits. Once a second it reads where each link of the chain got to. A stop is reported after
	// WATCH_LIMITS.stall without game work while the game is alive (a loading screen, a save or a load
	// after longStall: they hold frames by design), with its cause and where the main thread is, and
	// again every `again` while it lasts. The F4SE task dead, or the window not getting our message, is
	// re-armed once a tick from then on: the watch posts the frame message itself, to the game's window
	// as it is now.
	constexpr auto               WATCH_TICK = std::chrono::seconds(1);
	constexpr FrameWatch::Limits WATCH_LIMITS{ .lately = 3s, .stall = 15s, .longStall = 120s, .again = 60s };

	// Posts the frame message in the task's place.
	void PostFrameFromWatch()
	{
		if (const auto window = gameWindow.load()) {
			postedAt = std::chrono::steady_clock::now();
			framePosted = true;
			PostMessageW(window, frameMessage, 0, 0);
		}
	}

	// Whether the game's window is still the one hooked, with our procedure on it. Should the engine make
	// a new window, our message would go to a procedure that is not ours; the new one is hooked here, from
	// the watch's thread (a window of our own process can be). What was found, for the log.
	std::string CheckWindow()
	{
		const auto main = RE::Main::GetSingleton();
		const auto window = main ? reinterpret_cast<void*>(main->hwnd) : nullptr;
		const auto hooked = gameWindow.load();
		if (window && window != hooked) {
			const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<std::intptr_t>(&WndProc));
			if (previous == 0) {
				return "the game's window is a new one, which couldn't be hooked";
			}
			originalWndProc = previous;
			gameWindow = window;
			return "the game's window is a new one; it is hooked from here on";
		}
		const auto proc = hooked ? GetWindowLongPtrW(hooked, GWLP_WNDPROC) : 0;
		return proc == reinterpret_cast<std::intptr_t>(&WndProc) ? "the window and its procedure are still ours" : "another procedure has been put on the window since ours";
	}

	std::uint32_t FramesDeferred()
	{
		std::scoped_lock l{ frameThreadsLock };
		return frameThreads.deferred;
	}

	// The stop's cause, for the report. a_window: what CheckWindow found, when the message is not dispatched.
	std::string ExplainStop(FrameWatch::Stop a_stop, std::string_view a_window)
	{
		using FrameWatch::Stop;
		const auto where = [] { return HangWatch::DescribeMainThread(gameWindow.load()); };
		switch (a_stop) {
		case Stop::kNoTask:
			// The task runs from the game's frame loop, so a main thread held anywhere in it (the engine's pump, moving
			// the player, in the hang of 2026-10-17) stops it first: where it is says why.
			return std::format("the F4SE task has not run, so nothing posts the frame message; the watch posts it once a second instead; the main thread is {}", where());
		case Stop::kNotDispatched:
			return std::format("frame messages are posted, but the window procedure never gets them ({}); the watch posts one once a second", a_window);
		case Stop::kInFrame:
			return std::format("the main thread is still inside a frame of ours (an engine call in it pumps messages); it is {}", where());
		case Stop::kDeferred:
			return std::format("every frame message is put off: the main thread is inside the engine's pump, moving the player ({} put off); it is {}", FramesDeferred(), where());
		case Stop::kLoadingScreen:
			return "a loading screen is up, as far as the plugin heard";
		case Stop::kHeld:
			return "a save or load is under way (the hold ends on its own at its limit)";
		default:
			return std::format("the window procedure ran a frame, yet no game work came of it; the main thread is {}", where());
		}
	}

	void RunFrameWatch()
	{
		FrameWatch::Watch watch{ WATCH_LIMITS };
		for (;;) {
			std::this_thread::sleep_for(WATCH_TICK);
			if (Quitting()) {
				return;
			}
			const auto               state = RE::BSGraphics::State::GetSingleton();
			const FrameWatch::Sample sample{
				.now = std::chrono::steady_clock::now(),
				.drawn = state ? state->frameCount : 0u,
				.quitting = false,
				.hooked = gameWindow.load() != nullptr,
				.task = taskAt.load(),
				.dispatched = dispatchedAt.load(),
				.worked = workedAt.load(),
				.last = lastOutcome.load(),
			};
			// An exception must never end this thread (std::terminate would take the game with it).
			try {
				const auto result = watch.Tick(sample);
				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(result.stalled).count();
				std::string window;
				if (result.past && FrameWatch::Rearmable(result.stop)) {
					if (result.stop == FrameWatch::Stop::kNotDispatched) {
						window = CheckWindow();
					}
					PostFrameFromWatch();
				}
				if (result.event == FrameWatch::Watch::Event::kStalled) {
					// With the main thread in the engine's pump, the texture wait's state too (TextureWait.h): what the job it
					// waits for is waiting on.
					const auto pump = TextureWait::DescribePump(gameWindow.load());
					REX::WARN("Frame watch: no game work has run for {} s while the game drew {} frame(s): {}{}{}", seconds, result.drawn, ExplainStop(result.stop, window),
						pump.empty() ? "" : "; ", pump);
				} else if (result.event == FrameWatch::Watch::Event::kResumed) {
					REX::INFO("Frame watch: game work runs again, after {} s", seconds);
				}
				// Fast loading's vsync comes back once a loading screen has been up for its limit, or at once when
				// the game is stuck under one (a stop past its limit while the game draws): from this thread, which
				// runs whatever holds the main thread (FastLoadCap.h).
				FastLoad::Tick(result.past);
				// The graphics driver taking the game's device away: logged once, before the crash with no plugin frame
				// that follows (GpuDevice.h).
				GpuWatch::Tick(Quitting);
			} catch (const std::exception& e) {
				REX::ERROR("Frame watch: {}", e.what());
			}
		}
	}

	void StartFrameWatch()
	{
		try {
			std::thread{ RunFrameWatch }.detach();
		} catch (const std::exception& e) {
			REX::ERROR("Frame watch: couldn't start ({})", e.what());
		}
	}

	// The F4SE permanent task (see NewFrame for when it runs).
	void OnFrame()
	{
		const auto now = std::chrono::steady_clock::now();
		taskAt = now;
		// The game is on its way out: nothing more is posted (the window may be gone), nor run here.
		if (gameDataReady && Quitting()) {
			return;
		}
		if (!NewFrame(now)) {
			return;
		}
		if (!HookWindow()) {
			// The window could not be hooked: nothing runs. Game work on this job thread raced the engine (reading the
			// actor lists crashed leaving Red Rocket), and what a save or load hands over would take the stand-ins out
			// beside a frame. The log said so once; net status says so too.
			if (windowFailed) {
				return;
			}
			RunFrame(false);  // no window yet: startup, which comes before the game's data is ready
			return;
		}
		if (!framePosted.exchange(true) || now - postedAt.load() > REPOST_AFTER) {
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

	void Capture()
	{
		mainThreadChecked = true;  // the thread itself was taken at plugin load
	}

	void Assert(std::string_view a_where)
	{
		if (!mainThreadChecked || OnMainThread()) {
			return;
		}
		if (offThreadCalls++ == 0) {
			REX::ERROR("MainThread: {} ran off the main thread", a_where);
		}
	}

	std::uint32_t OffThread()
	{
		return offThreadCalls;
	}

	const char* HookState()
	{
		return gameWindow.load() ? "hooked" : (windowFailed ? "failed" : "pending");
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	// Before F4SE::Init opens (and empties) the log: the last launches' logs are kept.
	const auto notKept = Log::RotateBeforeInit(a_f4se->GetSaveFolderName());
	// 192 of 320 bytes used: four 14-byte call stubs (TextureWait::Install's one call, Face::Install's six share one,
	// Equipment::InstallFrameUse's two), HavokGuard::Install's two detours (2 x 56), and InertShots::Install's jump (a
	// 14-byte branch stub and a 10-byte continuation).
	F4SE::Init(a_f4se, { .trampoline = true, .trampolineSize = 320 });
	Log::InstallScrubber();
	mainThread = std::this_thread::get_id();
	if (notKept) {
		REX::WARN("Log: could not keep the previous log ({}); it was overwritten", notKept.message());
	}

	REX::INFO("F4Multiplayer loaded");
	if constexpr (sizeof(F4MP_BUILD_LABEL) > 1) {
		REX::INFO("F4Multiplayer build {}", F4MP_BUILD_LABEL);  // a release candidate names itself
	}
	Platform::LogStartup();  // Windows or Wine, and how the game was started (Steam, its overlay)

	Config::Load();
	Text::Load();
	Perf::StartTracy();  // a developer build with Tracy only (Perf.h)

	if (!F4SE::GetMessagingInterface()->RegisterListener(OnF4SEMessage)) {
		REX::ERROR("Failed to register F4SE message listener");
		return false;
	}

	F4SE::GetTaskInterface()->AddTaskPermanent(OnFrame);
	TextureWait::Install();  // before every move, the texture requests the walk owes are handed over (TextureFlush.h)
	HavokGuard::Install();   // Havok's angle loop ends on a NaN instead of spinning for ever (HavokNan.h)
	InertShots::Install();   // a friend's or a copy's missile flies here with no blast of its own (InertShot.h)
	WeaponFire::SetHandThrowables(Config::Get().grenadeInHand);  // a friend's grenade in their stand-in's hand (HandThrowable.h)
	Face::Install();         // friends' stand-ins get their own head, built the way the player's is (FaceHeads.h)
	Equipment::InstallFrameUse();  // what turns our player away from a power armor frame is mended first (FrameUse.h)
	Equipment::SetFrameGate(&RemotePlayers::KeepOutOfFrame);  // a copy of a friend's frame is theirs (FriendFrame.h)
	Equipment::InstallFrameGate();  // and its Transfer

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

namespace Perf
{
	Recorder& Global()
	{
		static Recorder recorder;
		return recorder;
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
		const auto t = std::exchange(frameThreads, FrameThreads{ .lastDrawn = frameThreads.lastDrawn, .pumpCheck = frameThreads.pumpCheck });
		return std::format("main={} (repeats={} drawing={}) other={} (repeats={} drawing={} threads={}) deferred={} pumpCheck={}", t.mainCalls, t.mainRepeats, t.mainDrawing, t.otherCalls,
			t.otherRepeats, t.otherDrawing, t.others.size(), t.deferred, NestedPump::Name(t.pumpCheck));
	}
}
