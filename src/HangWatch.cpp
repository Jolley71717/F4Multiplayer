#include "HangWatch.h"

#include "Config.h"
#include "HangDetector.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace HangWatch
{
	namespace
	{
		constexpr auto          TICK = std::chrono::seconds(1);
		constexpr auto          OVERSLEPT = std::chrono::seconds(5);       // a look this late: the whole game (or the PC) was paused
		constexpr auto          NOT_RESPONDING = std::chrono::seconds(5);  // Windows' wait before it calls a window not responding
		constexpr int           LOADING_FACTOR = 4;
		constexpr std::size_t   KEEP_DUMPS = 3;
		constexpr std::uint32_t MAX_SECONDS = 3600;

		// Thread stacks and the thread and module lists: enough to see where each thread is stuck, in
		// megabytes rather than the gigabytes of the game's whole memory.
		constexpr auto DUMP_TYPE = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

		using WriteDumpFn = decltype(&MiniDumpWriteDump);

		std::atomic<bool> loading{ false };

		struct Watch
		{
			HWND                  window = nullptr;
			DWORD                 mainThreadId = 0;
			WriteDumpFn           writeDump = nullptr;  // null: the hang is only logged
			std::filesystem::path folder;               // the F4SE log folder; empty: the hang is only logged
		};

		std::filesystem::path LogFolder()
		{
			PWSTR      docs = nullptr;
			const auto result = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs);
			std::filesystem::path path{ SUCCEEDED(result) && docs ? docs : L"" };
			CoTaskMemFree(docs);
			if (path.empty()) {
				return {};
			}
			const auto saveFolder = F4SE::GetSaveFolderName();
			return path / "My Games" / (saveFolder.empty() ? "Fallout4"sv : saveFolder) / "F4SE";
		}

		// Written under a temporary name and renamed when done, so a game closed mid-write leaves no
		// broken dump behind (the next prune removes the leftover). Returns the Windows error, 0 when written.
		DWORD WriteDump(const Watch& a_watch, const std::filesystem::path& a_path, const std::string& a_comment)
		{
			auto partial = a_path;
			partial += L".tmp";
			const auto file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return GetLastError();
			}
			MINIDUMP_USER_STREAM             comment{ CommentStreamA, static_cast<ULONG>(a_comment.size() + 1), const_cast<char*>(a_comment.c_str()) };
			MINIDUMP_USER_STREAM_INFORMATION streams{ 1, &comment };
			const bool written = a_watch.writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, DUMP_TYPE, nullptr, &streams, nullptr) != FALSE;
			DWORD      error = written ? 0 : GetLastError();
			CloseHandle(file);
			if (written && !MoveFileExW(partial.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
				error = GetLastError();
			}
			if (error != 0) {
				DeleteFileW(partial.c_str());
			}
			return error;
		}

		void PruneDumps(const std::filesystem::path& a_folder)
		{
			std::vector<std::string> names;
			std::error_code          ec;
			for (std::filesystem::directory_iterator it{ a_folder, ec }, end; !ec && it != end; it.increment(ec)) {
				// Only our own names are turned into narrow strings: another file's may not convert.
				const auto name = it->path().filename().wstring();
				if (name.starts_with(L"F4Multiplayer-hang-")) {
					names.push_back(it->path().filename().string());
				}
			}
			for (const auto& name : DumpsToRemove(std::move(names), KEEP_DUMPS)) {
				std::filesystem::remove(a_folder / name, ec);
			}
		}

		void OnHang(const Watch& a_watch, Clock::duration a_stalled, bool a_loading)
		{
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(a_stalled).count();
			const auto what = std::format("the game's main thread (thread {}, 0x{:x}) hasn't read its window messages for {} s{}", a_watch.mainThreadId,
				a_watch.mainThreadId, seconds, a_loading ? " on a loading screen" : "");
			if (!a_watch.writeDump || a_watch.folder.empty()) {
				REX::WARN("Hang: {}; no dump (dbghelp or the log folder is missing)", what);
				return;
			}
			SYSTEMTIME time{};
			GetLocalTime(&time);
			const auto name = DumpName(time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
			REX::WARN("Hang: {}; writing {}", what, name);
			if (const auto error = WriteDump(a_watch, a_watch.folder / name, "F4Multiplayer hang: " + what); error == 0) {
				REX::WARN("Hang: wrote {}", name);
			} else {
				REX::WARN("Hang: couldn't write {} (error 0x{:x})", name, error);
			}
			PruneDumps(a_watch.folder);
		}

		void Run(Watch a_watch, Detector a_detector)
		{
			for (;;) {
				const auto asleep = Clock::now();
				std::this_thread::sleep_for(TICK);
				const auto woke = Clock::now();
				if (!IsWindow(a_watch.window)) {
					return;  // the game is closing
				}
				// IsHungAppWindow only reads what Windows keeps on the main thread: nothing is sent to it,
				// nothing waits on it, and a minimised or alt-tabbed game that waits for messages counts
				// as answering.
				Sample sample;
				sample.responsive = !IsHungAppWindow(a_watch.window);
				sample.loading = loading;
				sample.paused = woke - asleep > TICK + OVERSLEPT;
				// An exception must never end this thread (std::terminate would take the game with it).
				try {
					const auto result = a_detector.Tick(Clock::now(), sample);
					if (result.event == Detector::Event::kHang) {
						OnHang(a_watch, result.stalled, sample.loading);
					} else if (result.event == Detector::Event::kRecovered) {
						REX::INFO("Hang: the game answers again, after about {} s", std::chrono::duration_cast<std::chrono::seconds>(result.stalled).count());
					}
				} catch (const std::exception& e) {
					REX::ERROR("Hang watch: {}", e.what());
				}
			}
		}
	}

	void Start()
	{
		const auto seconds = (std::min)(Config::Get().hangSeconds, MAX_SECONDS);
		if (seconds == 0) {
			REX::INFO("Hang watch: off");
			return;
		}
		const auto main = RE::Main::GetSingleton();
		Watch      watch;
		watch.window = main ? reinterpret_cast<HWND>(main->hwnd) : nullptr;
		if (!watch.window) {
			REX::WARN("Hang watch: no game window; off");
			return;
		}
		watch.mainThreadId = GetWindowThreadProcessId(watch.window, nullptr);
		// Loaded now, not when the game hangs: a hang can hold the loader lock that loading takes.
		if (const auto dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
			watch.writeDump = reinterpret_cast<WriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
		}
		watch.folder = LogFolder();

		const auto limit = std::chrono::seconds(seconds);
		try {
			std::thread{ Run, std::move(watch), Detector{ limit, limit * LOADING_FACTOR, NOT_RESPONDING } }.detach();
		} catch (const std::exception& e) {
			REX::ERROR("Hang watch: couldn't start ({})", e.what());
			return;
		}
		REX::INFO("Hang watch: a freeze of {} s ({} s on a loading screen) is logged with a dump of the threads", seconds, seconds * LOADING_FACTOR);
	}

	void SetLoading(bool a_loading)
	{
		loading = a_loading;
	}
}
