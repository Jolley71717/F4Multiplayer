#include "HangWatch.h"

#include "Config.h"
#include "net/Identity.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <array>
#include <cstring>
#include <deque>
#include <map>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace HangWatch
{
	namespace
	{
		constexpr auto          TICK = std::chrono::seconds(1);
		constexpr auto          OVERSLEPT = std::chrono::seconds(5);       // a look this late: the whole game (or the PC) was paused
		constexpr auto          NOT_RESPONDING = std::chrono::seconds(5);  // Windows' wait before it calls a window not responding
		constexpr int           LONG_FACTOR = 4;                           // saves, loads and loading screens
		constexpr std::size_t   KEEP_FILES = 3;
		constexpr std::uint32_t MAX_SECONDS = 3600;
		constexpr std::size_t   STACK_COPY = 64 * 1024;  // of the main thread's stack, from its stack pointer up
		constexpr std::size_t   MAX_CALLERS = 16;

		// The opt-in minidump (bHangDump): thread stacks and the thread and module lists, enough to see
		// where each thread is stuck, in megabytes rather than the gigabytes of the game's whole memory.
		// MiniDumpWriteDump runs in this process: it suspends every other thread, then needs the loader
		// lock and heap locks, so if the frozen main thread holds one of those the dump waits forever with
		// the game suspended, and a freeze the game would have come out of becomes a permanent one.
		// dbghelp is also single-threaded, and a crash logger may be using it at the same moment. Hence
		// off by default; the text report needs none of that.
		constexpr auto DUMP_TYPE = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

		using WriteDumpFn = decltype(&MiniDumpWriteDump);

		std::atomic<bool>              loadingScreen{ false };
		std::atomic<Phase>             saveOrLoad{ Phase::kPlay };
		std::atomic<Clock::time_point> saveOrLoadSince{};

		struct Watch
		{
			HWND                  window = nullptr;
			DWORD                 mainThreadId = 0;
			HANDLE                mainThread = nullptr;  // null: the report can't say where it was
			WriteDumpFn           writeDump = nullptr;   // set only with bHangDump
			std::filesystem::path folder;                // the plugin's folder next to the log; empty: the hang is only logged
			Clock::duration       longLimit{};
		};

		// The engine sets Main's quit flag when the game is to close (the menu, the console, a script, or
		// the window being destroyed), leaves its main loop on it and shuts down without reading window
		// messages, which can take a while and isn't a hang. The one game value the watch reads. Main is
		// deleted far into the shutdown and its pointer cleared; the watch stops at its first look after
		// the flag, so only a look that straddles the delete could read a freed Main (freed heap memory
		// stays mapped), and only in a shutdown that got that far within a second.
		bool Quitting()
		{
			const auto main = RE::Main::GetSingleton();
			return !main || std::atomic_ref{ main->quitGame }.load(std::memory_order_relaxed);
		}

		// The main thread at one moment: where it was and the top of its stack.
		struct Snapshot
		{
			std::uintptr_t            ip = 0;
			std::vector<std::uint8_t> stack;  // from its stack pointer up
		};

		// Between SuspendThread and ResumeThread there are only system calls, into a buffer made before:
		// the main thread may be stopped holding the heap lock, the loader lock or the log's, and taking
		// any of them here would wait forever on a thread that waits for us.
		bool Capture(HANDLE a_thread, Snapshot& a_out)
		{
			a_out.stack.resize(STACK_COPY);
			if (SuspendThread(a_thread) == static_cast<DWORD>(-1)) {
				return false;
			}
			CONTEXT context{};
			context.ContextFlags = CONTEXT_CONTROL;
			const bool got = GetThreadContext(a_thread, &context) != FALSE;  // also waits until the thread has stopped
			SIZE_T     copied = 0;
			if (got) {
				// The committed part of the stack above the stack pointer is one region up to the stack's base.
				MEMORY_BASIC_INFORMATION region{};
				if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT) {
					const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
					const auto size = (std::min)(static_cast<std::size_t>(end - context.Rsp), a_out.stack.size());
					if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), a_out.stack.data(), size, &copied)) {
						copied = 0;
					}
				}
			}
			ResumeThread(a_thread);
			a_out.ip = got ? context.Rip : 0;
			a_out.stack.resize(copied);
			return got;
		}

		// Names code addresses as module+offset without dbghelp or the loader's module list (both behind
		// locks): the memory manager says which image an address is in, and the image's mapped file gives
		// its name. Regions are remembered, since a stack holds the same few areas over and over.
		class Modules
		{
		public:
			// Whether a_address is in a loaded image's executable code.
			bool IsCode(std::uintptr_t a_address)
			{
				const auto region = Find(a_address);
				return region && region->code;
			}

			// "Fallout4.exe+0x1A2B3C", or the bare address outside any loaded image.
			std::string Describe(std::uintptr_t a_address)
			{
				const auto region = Find(a_address);
				if (!region || region->base == 0) {
					return std::format("0x{:X}", a_address);
				}
				auto name = names.find(region->base);
				if (name == names.end()) {
					wchar_t path[MAX_PATH]{};
					const auto length = GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(region->base), path, MAX_PATH);
					auto       module = ModuleName({ path, length });
					name = names.emplace(region->base, module.empty() ? std::format("0x{:X}", region->base) : std::move(module)).first;
				}
				return std::format("{}+0x{:X}", name->second, a_address - region->base);
			}

		private:
			struct Region
			{
				std::uintptr_t begin = 0, end = 0;
				std::uintptr_t base = 0;  // the image's; 0: not an image
				bool           code = false;
			};

			const Region* Find(std::uintptr_t a_address)
			{
				if (a_address < 0x10000 || a_address >= 0x800000000000) {
					return nullptr;  // never user-mode code
				}
				for (const auto& region : regions) {
					if (a_address >= region.begin && a_address < region.end) {
						return &region;
					}
				}
				MEMORY_BASIC_INFORMATION info{};
				if (VirtualQuery(reinterpret_cast<void*>(a_address), &info, sizeof(info)) != sizeof(info)) {
					return nullptr;
				}
				constexpr DWORD EXECUTABLE = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
				const bool      image = info.State == MEM_COMMIT && info.Type == MEM_IMAGE;
				const auto      begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
				return &regions.emplace_back(Region{ begin, begin + info.RegionSize, image ? reinterpret_cast<std::uintptr_t>(info.AllocationBase) : 0,
					image && (info.Protect & EXECUTABLE) != 0 });
			}

			std::deque<Region>                    regions;  // a deque: Find hands out pointers into it
			std::map<std::uintptr_t, std::string> names;
		};

		// Likely return addresses on the copied stack, nearest first: values that point just past a call
		// instruction in a loaded image's code. A scan rather than an unwind (that needs the loader's
		// function tables, behind its lock), so a few may be left over from calls that already returned.
		std::vector<std::uintptr_t> ReturnAddresses(const Snapshot& a_snapshot, Modules& a_modules, std::size_t a_max)
		{
			std::vector<std::uintptr_t> callers;
			for (std::size_t at = 0; at + sizeof(std::uintptr_t) <= a_snapshot.stack.size() && callers.size() < a_max; at += sizeof(std::uintptr_t)) {
				std::uintptr_t value = 0;
				std::memcpy(&value, a_snapshot.stack.data() + at, sizeof(value));
				if (!a_modules.IsCode(value)) {
					continue;
				}
				// Read rather than dereferenced: the bytes before may be outside the image.
				std::array<std::uint8_t, CALL_BYTES> before{};
				SIZE_T                               read = 0;
				if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(value - before.size()), before.data(), before.size(), &read) && read == before.size() &&
					EndsWithCall(before)) {
					callers.push_back(value);
				}
			}
			return callers;
		}

		std::vector<std::string> Callers(const Snapshot& a_snapshot, Modules& a_modules)
		{
			std::vector<std::string> callers;
			for (const auto address : ReturnAddresses(a_snapshot, a_modules, MAX_CALLERS)) {
				callers.push_back(a_modules.Describe(address));
			}
			return callers;
		}

		std::uint64_t Ticks(const FILETIME& a_time)
		{
			return (std::uint64_t{ a_time.dwHighDateTime } << 32) | a_time.dwLowDateTime;
		}

		// The processor time each of the game's threads has used so far (HangDetector.h, ThreadTime). The list of
		// threads is the kernel's, from a Toolhelp thread snapshot (unlike a module snapshot, it doesn't read the
		// loader's lists), and each thread is asked only for its times.
		std::vector<ThreadTime> ThreadTimes()
		{
			std::vector<ThreadTime> times;
			const auto              snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return times;
			}
			const auto    process = GetCurrentProcessId();
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
				if (entry.th32OwnerProcessID != process) {
					continue;
				}
				if (const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
					FILETIME created{}, exited{}, kernel{}, user{};
					if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
						times.push_back({ entry.th32ThreadID, Ticks(kernel) + Ticks(user) });
					}
					CloseHandle(thread);
				}
			}
			CloseHandle(snapshot);
			return times;
		}

		// Where another busy thread was, looked at the way the main thread is (Capture). Its id is checked to be still
		// one of the game's: a thread that ended since the snapshot can have its id taken by another program's, which
		// mustn't be stopped.
		Report::Busy LookAt(const BusyThread& a_thread, Modules& a_modules)
		{
			Report::Busy busy{ .id = a_thread.id, .percent = a_thread.percent };
			if (const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_thread.id)) {
				Snapshot snapshot;
				if (GetProcessIdOfThread(thread) == GetCurrentProcessId() && Capture(thread, snapshot)) {
					busy.where = a_modules.Describe(snapshot.ip);
					busy.callers = Callers(snapshot, a_modules);
				}
				CloseHandle(thread);
			}
			return busy;
		}

		std::string Joined(const std::vector<std::string>& a_callers)
		{
			std::string joined;
			for (const auto& caller : a_callers) {
				joined += (joined.empty() ? "" : ", ") + caller;
			}
			return joined.empty() ? "none found"s : joined;
		}

		// Written under a temporary name and renamed when done, so a game closed mid-write leaves no
		// broken file behind (the next start-up removes the leftover). a_write returns the Windows error,
		// 0 when written; so does this.
		template <class Write>
		DWORD WriteFileWhole(const std::filesystem::path& a_path, Write a_write)
		{
			auto partial = a_path;
			partial += PARTIAL_SUFFIX;
			const auto file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return GetLastError();
			}
			DWORD error = a_write(file);
			CloseHandle(file);
			if (error == 0 && !MoveFileExW(partial.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
				error = GetLastError();
			}
			if (error != 0) {
				DeleteFileW(partial.c_str());
			}
			return error;
		}

		void PruneFiles(const std::filesystem::path& a_folder)
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
			for (const auto& name : HangFilesToRemove(std::move(names), KEEP_FILES)) {
				std::filesystem::remove(a_folder / name, ec);
			}
		}

		void OnHang(const Watch& a_watch, Clock::duration a_stalled, Phase a_phase, Clock::duration a_limit)
		{
			// First, while the main thread is still where it hung.
			Snapshot   snapshot;
			const bool captured = a_watch.mainThread && Capture(a_watch.mainThread, snapshot);

			SYSTEMTIME time{};
			GetLocalTime(&time);
			Modules modules;
			Report  report;
			report.version = std::format("{}", F4SE::GetPluginVersion());
			report.time = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
			report.phase = a_phase;
			report.stalledSeconds = std::chrono::duration_cast<std::chrono::seconds>(a_stalled).count();
			report.limitSeconds = std::chrono::duration_cast<std::chrono::seconds>(a_limit).count();
			report.threadId = a_watch.mainThreadId;
			if (captured) {
				report.where = modules.Describe(snapshot.ip);
				report.callers = Callers(snapshot, modules);
			}
			// Then every thread of the game, timed for a moment: one that kept a processor busy while the main
			// thread waits is likely what it waits for, and a busy main thread is in a loop of its own.
			const auto before = ThreadTimes();
			const auto from = Clock::now();
			std::this_thread::sleep_for(THREAD_LOOK);
			const auto after = ThreadTimes();
			const auto between = Clock::now() - from;
			report.timed = !before.empty() && !after.empty();
			if (report.timed) {
				report.mainPercent = CpuPercent(before, after, a_watch.mainThreadId, between);
				const std::array<std::uint32_t, 2> skip{ static_cast<std::uint32_t>(a_watch.mainThreadId), static_cast<std::uint32_t>(GetCurrentThreadId()) };
				for (const auto& busy : BusyThreads(before, after, between, skip)) {
					report.busy.push_back(LookAt(busy, modules));
				}
			}

			REX::WARN("Hang: the game's main thread (thread {}) hasn't read its window messages for {} s, {}", report.threadId, report.stalledSeconds, Describe(a_phase));
			if (captured) {
				REX::WARN("Hang: it was at {}; return addresses on its stack: {}", report.where, Joined(report.callers));
			}
			if (report.mainPercent) {
				REX::WARN("Hang: it used {}% of a processor over {} ms", *report.mainPercent, std::chrono::duration_cast<std::chrono::milliseconds>(THREAD_LOOK).count());
			}
			for (const auto& busy : report.busy) {
				REX::WARN("Hang: thread {} kept a processor {}% busy meanwhile, at {}; return addresses on its stack: {}", busy.id, busy.percent,
					busy.where.empty() ? "an unknown place"s : busy.where, Joined(busy.callers));
			}
			if (report.timed && report.busy.empty()) {
				REX::WARN("Hang: no other thread of the game kept a processor busy meanwhile");
			}
			if (a_watch.folder.empty()) {
				REX::WARN("Hang: no report file (no Documents folder)");
				return;
			}

			const auto stem = [&](std::string_view a_suffix) { return HangFileName(time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, a_suffix); };
			const auto text = ReportText(report);
			const auto name = stem(REPORT_SUFFIX);
			const auto error = WriteFileWhole(a_watch.folder / name, [&](HANDLE a_file) -> DWORD {
				DWORD written = 0;
				if (!WriteFile(a_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
					return GetLastError();
				}
				return written == text.size() ? 0 : static_cast<DWORD>(ERROR_WRITE_FAULT);
			});
			if (error == 0) {
				REX::WARN("Hang: wrote {}", name);
			} else {
				REX::WARN("Hang: couldn't write {} (error 0x{:x})", name, error);
			}

			if (a_watch.writeDump) {
				const auto dumpName = stem(DUMP_SUFFIX);
				REX::WARN("Hang: writing {} (bHangDump)", dumpName);
				const auto comment = "F4Multiplayer hang\n" + text;
				const auto dumpError = WriteFileWhole(a_watch.folder / dumpName, [&](HANDLE a_file) -> DWORD {
					MINIDUMP_USER_STREAM             stream{ CommentStreamA, static_cast<ULONG>(comment.size() + 1), const_cast<char*>(comment.c_str()) };
					MINIDUMP_USER_STREAM_INFORMATION streams{ 1, &stream };
					return a_watch.writeDump(GetCurrentProcess(), GetCurrentProcessId(), a_file, DUMP_TYPE, nullptr, &streams, nullptr) ? 0 : GetLastError();
				});
				if (dumpError == 0) {
					REX::WARN("Hang: wrote {}", dumpName);
				} else {
					REX::WARN("Hang: couldn't write {} (error 0x{:x})", dumpName, dumpError);
				}
			}
			PruneFiles(a_watch.folder);
		}

		void Run(Watch a_watch, Detector a_detector)
		{
			for (;;) {
				const auto asleep = Clock::now();
				std::this_thread::sleep_for(TICK);
				const auto woke = Clock::now();
				if (!IsWindow(a_watch.window) || Quitting()) {
					REX::INFO("Hang watch: the game is closing; stopped");
					return;
				}
				// IsHungAppWindow only reads what Windows keeps on the main thread: nothing is sent to it,
				// nothing waits on it, and a minimised or alt-tabbed game that waits for messages counts
				// as answering.
				Sample sample;
				sample.responsive = !IsHungAppWindow(a_watch.window);
				const Phase saveOrLoadNow = saveOrLoad;  // read before its start time, which SetSaveOrLoad sets first
				sample.phase = CurrentPhase(loadingScreen, saveOrLoadNow, woke - saveOrLoadSince.load(), a_watch.longLimit);
				sample.paused = woke - asleep > TICK + OVERSLEPT;
				// An exception must never end this thread (std::terminate would take the game with it).
				try {
					const auto result = a_detector.Tick(Clock::now(), sample);
					if (result.event == Detector::Event::kHang) {
						OnHang(a_watch, result.stalled, result.phase, a_detector.Limit(result.phase));
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
		const auto folder = Identity::DataFolder();
		if (folder) {
			PruneFiles(*folder);
		}
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
		// The window was made by the main thread (WinMain's), which is the one watched.
		watch.mainThreadId = GetWindowThreadProcessId(watch.window, nullptr);
		watch.mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, watch.mainThreadId);
		if (!watch.mainThread) {
			REX::WARN("Hang watch: can't look at the main thread (error 0x{:x}); a hang is logged without where it was", GetLastError());
		}
		// Loaded now, not when the game hangs: a hang can hold the loader lock that loading takes.
		if (Config::Get().hangDump) {
			if (const auto dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
				watch.writeDump = reinterpret_cast<WriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
			}
			if (!watch.writeDump) {
				REX::WARN("Hang watch: bHangDump is on, but dbghelp couldn't be loaded; reports only");
			}
		}
		watch.folder = folder.value_or(std::filesystem::path{});
		const auto limit = std::chrono::seconds(seconds);
		watch.longLimit = limit * LONG_FACTOR;
		const bool dump = watch.writeDump != nullptr;

		try {
			std::thread{ Run, std::move(watch), Detector{ limit, limit * LONG_FACTOR, NOT_RESPONDING } }.detach();
		} catch (const std::exception& e) {
			REX::ERROR("Hang watch: couldn't start ({})", e.what());
			return;
		}
		REX::INFO("Hang watch: a freeze of {} s ({} s while saving, loading or on a loading screen) is logged with a short report{}", seconds, seconds * LONG_FACTOR,
			dump ? " and a minidump (bHangDump)" : "");
	}

	void SetLoading(bool a_loading)
	{
		loadingScreen = a_loading;
	}

	void SetSaveOrLoad(Phase a_what)
	{
		if (a_what != Phase::kPlay) {
			saveOrLoadSince = Clock::now();
		}
		saveOrLoad = a_what;
	}

	std::string DescribeMainThread(void* a_window)
	{
		const auto id = a_window ? GetWindowThreadProcessId(static_cast<HWND>(a_window), nullptr) : 0;
		const auto thread = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id) : nullptr;
		if (!thread) {
			return "somewhere unknown (the main thread couldn't be looked at)";
		}
		Snapshot   snapshot;
		const bool captured = Capture(thread, snapshot);
		CloseHandle(thread);
		if (!captured) {
			return "somewhere unknown (the main thread couldn't be looked at)";
		}
		Modules modules;
		return std::format("at {}; return addresses on its stack: {}", modules.Describe(snapshot.ip), Joined(Callers(snapshot, modules)));
	}

	std::uint32_t WindowThread(void* a_window)
	{
		return a_window ? GetWindowThreadProcessId(static_cast<HWND>(a_window), nullptr) : 0;
	}

	ThreadLook LookAtThread(std::uint32_t a_threadId, std::size_t a_maxCallers)
	{
		ThreadLook look;
		if (a_threadId == 0 || a_threadId == GetCurrentThreadId()) {
			return look;  // stopping itself would never come back
		}
		const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_threadId);
		if (!thread) {
			return look;
		}
		Snapshot snapshot;
		// An id read from the game's memory may name a thread that has ended, its id taken by another program's.
		if (GetProcessIdOfThread(thread) == GetCurrentProcessId() && Capture(thread, snapshot)) {
			Modules modules;
			look = { .looked = true, .ip = snapshot.ip, .callers = ReturnAddresses(snapshot, modules, a_maxCallers) };
		}
		CloseHandle(thread);
		return look;
	}

	bool ReadMemory(std::uintptr_t a_address, void* a_out, std::size_t a_size)
	{
		SIZE_T read = 0;
		return a_address != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a_address), a_out, a_size, &read) && read == a_size;
	}
}
