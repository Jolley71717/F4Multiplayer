#include "CrashWatch.h"

#include "Config.h"
#include "HangWatch.h"
#include "Log.h"

#include <Windows.h>
#include <Psapi.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace CrashWatch
{
	namespace
	{
		constexpr std::size_t STACK_COPY = 64 * 1024;  // of the crashing thread's stack, from its stack pointer up
		constexpr std::size_t MAX_CALLERS = 24;
		constexpr std::size_t MODULE_NAME = 64;
		constexpr std::size_t PATH = 1024;

		// Set at Install (and OnDataReady), only read at the crash.
		LPTOP_LEVEL_EXCEPTION_FILTER installTime = nullptr;  // the filter found at our install (the game's own, or a plugin's)
		LPTOP_LEVEL_EXCEPTION_FILTER displaced = nullptr;    // a later plugin's filter OnDataReady displaced (CrashReport::HandOn)
		bool                         installed = false;
		DWORD                        mainThreadId = 0;
		ULONGLONG                    loadedAt = 0;
		std::array<wchar_t, PATH>    folder{};   // the log's folder with its trailing separator; empty: no report file
		std::array<wchar_t, PATH>    logPath{};  // the log, which gets the report appended; empty: none
		std::array<char, 32>         version{};
		std::array<char, 64>         previousName{};  // the previous filter's module, for the report (Name at install)
		CrashReport::Where           previousWhere{};
		std::array<char, 64>         loggerName{};    // a crash logger's DLL found loaded (OnDataReady); empty: none
		std::atomic<std::uint8_t>    session{ 0 };
		std::atomic<std::uint32_t>   players{ 0 };
		std::atomic<bool>            reporting{ false };   // one report per game: a second crash goes straight on
		std::atomic<bool>            reportDone{ false };  // ... after waiting for the first to be written (REPORT_WAIT)
		thread_local int             inFilter = 0;         // how deep this thread is inside our filter already (CrashReport::HandOn)
		constexpr DWORD              REPORT_WAIT = 5000;   // ms a second crashing thread waits for the first's report
		constexpr ULONG              STACK_GUARANTEE = 64 * 1024;  // of the main thread's stack kept for the report after a stack overflow

		// What the crash fills in: static, so neither the crashing thread's stack (a stack overflow has next to none left)
		// nor the heap is used.
		std::array<std::uint8_t, STACK_COPY>                     stackCopy{};
		CrashReport::ReportText                                  text;
		std::array<CrashReport::Where, MAX_CALLERS>              callers{};
		std::array<std::array<char, MODULE_NAME>, MAX_CALLERS + 1> names{};  // what the Wheres' module views point into
		std::array<wchar_t, PATH>                                filePath{};
		std::array<wchar_t, 4096>                                mappedPath{};  // a module's mapped path (Name); one report at a time

		// The loaded image a_address is in (its base), and whether that is executable code. System calls only.
		struct Image
		{
			std::uintptr_t base = 0;
			bool           code = false;
		};

		Image ImageOf(std::uintptr_t a_address)
		{
			if (a_address < 0x10000 || a_address >= 0x800000000000) {
				return {};  // never user-mode code
			}
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(reinterpret_cast<void*>(a_address), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE) {
				return {};
			}
			constexpr DWORD EXECUTABLE = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return { reinterpret_cast<std::uintptr_t>(info.AllocationBase), (info.Protect & EXECUTABLE) != 0 };
		}

		// a_address as module+offset, the module's name written into a_name (the image's mapped file name, without
		// its path: neither dbghelp nor the loader's module list, both behind locks the crashing thread may hold).
		CrashReport::Where Name(std::uintptr_t a_address, std::span<char> a_name)
		{
			CrashReport::Where where{ .address = a_address };
			const auto         image = ImageOf(a_address);
			if (image.base == 0) {
				return where;
			}
			// A path longer than the buffer comes back cut (its end, the file's name, lost): then the module stays
			// unnamed rather than named after a folder.
			const auto length = GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(image.base), mappedPath.data(), static_cast<DWORD>(mappedPath.size()));
			if (length == 0 || length >= mappedPath.size() - 1) {
				return where;
			}
			const auto n = CrashReport::ModuleNameInto({ mappedPath.data(), length }, a_name);
			if (n == 0) {
				return where;
			}
			where.module = { a_name.data(), n };
			where.offset = a_address - image.base;
			return where;
		}

		// Likely return addresses on the copied stack, nearest first: values that point just past a call instruction
		// in a loaded image's code (the hang watch's scan). How many were found, in `callers`.
		std::size_t ScanCallers(std::span<const std::uint8_t> a_stack)
		{
			std::size_t found = 0;
			for (std::size_t at = 0; at + sizeof(std::uintptr_t) <= a_stack.size() && found < MAX_CALLERS; at += sizeof(std::uintptr_t)) {
				std::uintptr_t value = 0;
				std::memcpy(&value, a_stack.data() + at, sizeof(value));
				if (!ImageOf(value).code) {
					continue;
				}
				std::array<std::uint8_t, HangWatch::CALL_BYTES> before{};
				SIZE_T                                           read = 0;
				if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(value - before.size()), before.data(), before.size(), &read) && read == before.size() &&
					HangWatch::EndsWithCall(before)) {
					callers[found] = Name(value, names[found + 1]);
					++found;
				}
			}
			return found;
		}

		// The whole of a_text to the file at a_path. The Windows error, 0 when written. CreateFileW itself takes from the
		// process heap (for the name it hands the kernel): with the heap the broken thing it faults or waits, and the
		// fault ends under ReportGuarded's handler; this is why the files come last, after the text is built.
		DWORD WriteWhole(const wchar_t* a_path, DWORD a_access, DWORD a_creation, std::string_view a_text)
		{
			const auto file = CreateFileW(a_path, a_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, a_creation, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return GetLastError();
			}
			DWORD written = 0;
			DWORD error = 0;
			if (!WriteFile(file, a_text.data(), static_cast<DWORD>(a_text.size()), &written, nullptr)) {
				error = GetLastError();
			} else if (written != a_text.size()) {
				error = ERROR_WRITE_FAULT;
			}
			CloseHandle(file);
			return error;
		}

		void Report(const EXCEPTION_POINTERS* a_info)
		{
			const auto& record = *a_info->ExceptionRecord;
			const auto& context = *a_info->ContextRecord;

			CrashReport::Facts facts;
			facts.code = record.ExceptionCode;
			facts.address = reinterpret_cast<std::uint64_t>(record.ExceptionAddress);
			if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2) {
				const auto kind = record.ExceptionInformation[0];
				facts.access = kind == 0 ? CrashReport::Access::kRead : kind == 1 ? CrashReport::Access::kWrite : kind == 8 ? CrashReport::Access::kExecute : CrashReport::Access::kNone;
				facts.accessAddress = record.ExceptionInformation[1];
			}
			facts.threadId = GetCurrentThreadId();
			facts.mainThread = facts.threadId == mainThreadId;
			facts.phase = HangWatch::PhaseNow();
			facts.session = static_cast<CrashReport::Session>(session.load(std::memory_order_relaxed));
			facts.players = players.load(std::memory_order_relaxed);
			facts.uptimeSeconds = (GetTickCount64() - loadedAt) / 1000;
			facts.previous = { previousWhere.module, previousWhere.offset, previousWhere.address };  // the one HandOn picks first
			facts.logger = std::string_view{ loggerName.data() };

			// The committed stack above the stack pointer is one region up to the stack's base; read, not dereferenced.
			// A stack overflow's is left alone: the thread has next to no stack to run this on, and the copy is 64 KB of
			// the same stack it ran out of.
			std::size_t copied = 0;
			if (record.ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
				MEMORY_BASIC_INFORMATION region{};
				if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT) {
					const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
					const auto size = (std::min)(static_cast<std::size_t>(end - context.Rsp), stackCopy.size());
					SIZE_T     read = 0;
					if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp), stackCopy.data(), size, &read)) {
						copied = read;
					}
				}
			}
			facts.stackCopied = copied > 0;
			const auto found = ScanCallers({ stackCopy.data(), copied });
			const auto where = Name(facts.address, names[0]);

			SYSTEMTIME time{};
			GetLocalTime(&time);
			CrashReport::Text<24> when;
			when.AppendDec(time.wYear, 4);
			when.Append("-");
			when.AppendDec(time.wMonth, 2);
			when.Append("-");
			when.AppendDec(time.wDay, 2);
			when.Append(" ");
			when.AppendDec(time.wHour, 2);
			when.Append(":");
			when.AppendDec(time.wMinute, 2);
			when.Append(":");
			when.AppendDec(time.wSecond, 2);

			text.Clear();
			CrashReport::WriteReport(text, std::string_view{ version.data() }, when.View(), facts, where, std::span<const CrashReport::Where>{ callers.data(), found });

			if (folder[0] != L'\0') {
				CrashReport::Text<64> name;
				CrashReport::AppendFileName(name, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
				std::size_t n = 0;
				for (; n < filePath.size() - 1 && folder[n] != L'\0'; ++n) {
					filePath[n] = folder[n];
				}
				for (const char c : name.View()) {
					if (n + 1 >= filePath.size()) {
						break;
					}
					filePath[n++] = static_cast<wchar_t>(c);
				}
				filePath[n] = L'\0';
				WriteWhole(filePath.data(), GENERIC_WRITE, CREATE_ALWAYS, text.View());
			}
			if (logPath[0] != L'\0') {
				// Appended through a handle of its own (the log's own writer may be the thread that crashed, with its lock
				// held). The log's sink shares the file, and flushes every line, so the lines before the crash are there.
				WriteWhole(logPath.data(), FILE_APPEND_DATA, OPEN_ALWAYS, text.View());
			}
		}

		// No C++ object with a destructor in here: the structured handler below needs none to unwind.
		void ReportGuarded(const EXCEPTION_POINTERS* a_info)
		{
			__try {
				Report(a_info);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				// The report itself faulted (a stack too far gone, memory unmapped meanwhile): the game closes as it would have.
			}
		}

		LONG WINAPI Filter(EXCEPTION_POINTERS* a_info)
		{
			// Entered again on this thread: a filter we handed on to chained back to ours. Nothing more from us (CrashReport::HandOn).
			const int depth = inFilter++;
			if (depth == 0 && a_info) {
				if (!reporting.exchange(true)) {
					ReportGuarded(a_info);
					reportDone = true;
				} else {
					// Another thread is writing the report (or wrote it): this one waits for it, briefly, so the file and the
					// log are whole before anything ends the process.
					for (DWORD waited = 0; !reportDone && waited < REPORT_WAIT; waited += 50) {
						Sleep(50);
					}
				}
			}
			LONG verdict = EXCEPTION_CONTINUE_SEARCH;
			switch (CrashReport::HandOn(depth, displaced != nullptr, installTime != nullptr)) {
			case CrashReport::Next::kDisplaced:
				verdict = displaced(a_info);
				break;
			case CrashReport::Next::kInstallTime:
				verdict = installTime(a_info);
				break;
			default:
				break;
			}
			--inFilter;  // a filter that let the thread go on (EXCEPTION_CONTINUE_EXECUTION) leaves it as it found it
			return verdict;
		}

		// The filter the exception goes to first (HandOn: the displaced one, else the install-time one) named, into
		// previousName, for the report and the log.
		void NotePrevious()
		{
			const auto first = displaced ? displaced : installTime;
			previousWhere = first ? Name(reinterpret_cast<std::uintptr_t>(first), previousName) : CrashReport::Where{};
		}

		std::string PreviousText()
		{
			if (!displaced && !installTime) {
				return "to Windows (no other exception filter was there)";
			}
			const auto where = previousWhere.module.empty() ? std::format("0x{:X}", previousWhere.address) : std::format("{}+0x{:X}", previousWhere.module, previousWhere.offset);
			return std::format("handed to the exception filter that was there before, at {}", where);
		}

		void CopyPath(const std::wstring& a_from, std::array<wchar_t, PATH>& a_to)
		{
			const auto n = (std::min)(a_from.size(), a_to.size() - 1);
			std::memcpy(a_to.data(), a_from.data(), n * sizeof(wchar_t));
			a_to[n] = L'\0';
		}

		void PruneFiles(const std::filesystem::path& a_folder)
		{
			std::vector<std::string> found;
			std::error_code          ec;
			for (std::filesystem::directory_iterator it{ a_folder, ec }, end; !ec && it != end; it.increment(ec)) {
				// Narrowed by hand, never through the locale (a name that doesn't convert would throw at plugin load): a
				// character outside ASCII becomes '?', which no name of ours holds, so FilesToRemove leaves that file alone.
				const auto name = it->path().filename().wstring();
				if (name.starts_with(L"F4Multiplayer-crash-")) {
					std::string narrow;
					for (const auto c : name) {
						narrow.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
					}
					found.push_back(std::move(narrow));
				}
			}
			for (const auto& name : CrashReport::FilesToRemove(std::move(found), CrashReport::KEEP_FILES)) {
				std::filesystem::remove(a_folder / name, ec);
			}
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		if (!Config::Get().crashReport) {
			REX::INFO("Crash report: off (bCrashReport); a crash to desktop leaves only the log");
			return;
		}
		mainThreadId = GetCurrentThreadId();  // the plugin loads on the game's main thread
		loadedAt = GetTickCount64();
		const auto versionText = std::format("{}", F4SE::GetPluginVersion());
		const auto n = (std::min)(versionText.size(), version.size() - 1);
		std::memcpy(version.data(), versionText.data(), n);
		version[n] = '\0';

		// A path too long for its buffer is left out rather than cut short.
		const auto& log = Log::Path();
		if (!log.empty()) {
			const auto parent = log.parent_path();
			if (const auto dir = parent.wstring() + L"\\"; dir.size() < PATH) {
				CopyPath(dir, folder);
			}
			if (const auto file = log.wstring(); file.size() < PATH) {
				CopyPath(file, logPath);
			}
			PruneFiles(parent);
		}

		// Room on the main thread's stack for the report after a stack overflow (Windows keeps this much below the guard
		// page for the filter). Other threads keep Windows' default; their report runs under ReportGuarded either way.
		ULONG guarantee = STACK_GUARANTEE;
		if (!SetThreadStackGuarantee(&guarantee)) {
			REX::WARN("Crash report: no stack guarantee for the main thread (error 0x{:x}); a stack overflow there may close the game with no report", GetLastError());
		}
		installTime = SetUnhandledExceptionFilter(&Filter);
		installed = true;
		NotePrevious();
		REX::INFO("Crash report: a crash to desktop is written to F4Multiplayer-crash-<date>_<time>.txt next to this log (the newest {} are kept) and to this log{}, then {}",
			CrashReport::KEEP_FILES, folder[0] == L'\0' ? " (no folder: to the log alone)" : "", PreviousText());
	}

	void OnDataReady()
	{
		if (!installed) {
			return;
		}
		// Which crash logger is in (any of them may load before or after us). The module list is read here, at a quiet
		// moment, never at the crash: GetModuleHandle takes the loader lock.
		for (const wchar_t* dll : { L"Buffout4.dll", L"AddictolCrashLogger.dll", L"Trainwreck.dll", L"CrashLogger.dll" }) {
			if (GetModuleHandleW(dll)) {
				CrashReport::ModuleNameInto(dll, loggerName);
				break;
			}
		}
		// A plugin that loaded after ours may have put its filter in front of ours: ours goes back in front, and that one
		// gets the exception after the report. It most likely chains back to ours (what it found at its install), which
		// Filter sees as a re-entry and ends the chain at the install-time filter (CrashReport::HandOn).
		const auto current = SetUnhandledExceptionFilter(&Filter);
		if (current != &Filter && current != nullptr) {
			displaced = current;
			NotePrevious();
			REX::INFO("Crash report: {} put an exception filter in after the mod's; the mod's is in front again, then {}",
				previousWhere.module == "Fallout4.exe" ? std::string{ "the game" } : previousWhere.module.empty() ? std::string{ "something" } : std::format("a plugin ({})", previousWhere.module),
				PreviousText());
		}
		// A crash logger's vectored handler may run before any filter and end the process itself (Addictol does, on the
		// rig: its crash-*.log, our WARN, nothing after), so with one loaded the mod's report may not be written.
		if (loggerName[0]) {
			REX::INFO("Crash report: the crash logger {} is loaded and writes its own crash log; the mod's report may then not be written. With a crash, send that logger's crash log too, with F4Multiplayer.log",
				loggerName.data());
		} else {
			REX::INFO("Crash report: no crash logger (Buffout 4, Addictol) is loaded: the mod's report is all a crash leaves");
		}
	}

	void SetSession(CrashReport::Session a_session, std::uint32_t a_players)
	{
		session.store(static_cast<std::uint8_t>(a_session), std::memory_order_relaxed);
		players.store(a_players, std::memory_order_relaxed);
	}

	namespace
	{
		// Each crash is made with volatile reads and writes, so the compiler keeps them (and the recursion is through a
		// volatile pointer, so it isn't seen as the endless recursion it is).
		std::size_t Overflow(std::size_t a_depth)
		{
			static std::size_t (*volatile next)(std::size_t) = &Overflow;
			volatile char pad[2048];
			pad[0] = static_cast<char>(a_depth);
			return next(a_depth + 1) + static_cast<std::size_t>(pad[0]);
		}

		DWORD WINAPI CrashThread(LPVOID a_kind)
		{
			CrashNow(static_cast<Kind>(reinterpret_cast<std::uintptr_t>(a_kind)));
		}
	}

	void CrashNow(Kind a_kind)
	{
		REX::WARN("Crash report: crashing the game on purpose ({}), as the dev channel asked", static_cast<int>(a_kind));
		Log::Flush();
		volatile std::uint32_t* const nowhere = reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(0x10));
		switch (a_kind) {
		case Kind::kWrite:
			*nowhere = 1;
			break;
		case Kind::kCpp:
			throw std::runtime_error("crashtest: a C++ exception nothing catches");
		case Kind::kStack:
			Overflow(0);
			break;
		default:
			(void)*nowhere;
			break;
		}
		// Only if the crash somehow didn't happen (the write to 0x10 would need that page mapped).
		std::abort();
	}

	void CrashOnNewThread(Kind a_kind)
	{
		if (const auto thread = CreateThread(nullptr, 0, &CrashThread, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(a_kind)), 0, nullptr)) {
			CloseHandle(thread);
		}
	}
}
