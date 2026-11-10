#pragma once

#include "HangDetector.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The crash report's judgement and text (CrashWatch.cpp has the exception filter and the files). Most players have no
// crash logger (Buffout 4, Addictol), so a crash to desktop left nothing but a log that stops: four of the first six
// crash reports came with no crash log. The plugin's own unhandled-exception filter now writes a short report before
// the game closes: what the exception was and where (module+offset, never a path), which thread, what the game and
// the session were doing, and the return addresses found on the thread's stack; the same goes to the log. It runs
// on the crashing thread, whose heap and locks may be the broken thing, so nothing here allocates: the text is built
// in a fixed buffer (Text), and the Windows side reads modules with system calls only, the way the hang watch does.
// A crash logger installed before the plugin still gets the exception after (the filter hands it on). Pure, so it
// is unit tested.
namespace CrashReport
{
	// A fixed-capacity text: appends past the end are dropped (the report ends with "..." then), never written
	// outside the buffer, and nothing is allocated.
	template <std::size_t N>
	class Text
	{
	public:
		void Append(std::string_view a_text)
		{
			for (const char c : a_text) {
				if (size + 1 >= N) {
					Truncate();
					return;
				}
				buffer[size++] = c;
			}
			buffer[size] = '\0';
		}

		void AppendHex(std::uint64_t a_value)
		{
			char digits[16];
			int  n = 0;
			do {
				digits[n++] = "0123456789ABCDEF"[a_value & 0xF];
				a_value >>= 4;
			} while (a_value != 0);
			Append("0x");
			while (n > 0) {
				Append(std::string_view{ &digits[--n], 1 });
			}
		}

		void AppendDec(std::uint64_t a_value, int a_width = 0)
		{
			char digits[20];
			int  n = 0;
			do {
				digits[n++] = static_cast<char>('0' + a_value % 10);
				a_value /= 10;
			} while (a_value != 0);
			for (int pad = a_width - n; pad > 0; --pad) {
				Append("0");
			}
			while (n > 0) {
				Append(std::string_view{ &digits[--n], 1 });
			}
		}

		[[nodiscard]] std::string_view View() const { return { buffer.data(), size }; }
		[[nodiscard]] bool             Truncated() const { return truncated; }
		void                           Clear()
		{
			size = 0;
			truncated = false;
			buffer[0] = '\0';
		}

	private:
		void Truncate()
		{
			if (truncated) {
				return;
			}
			truncated = true;
			constexpr std::string_view MARK = "...\n";
			if (N > MARK.size() + 1) {
				size = (std::min)(size, N - MARK.size() - 1);
				for (const char c : MARK) {
					buffer[size++] = c;
				}
				buffer[size] = '\0';
			}
		}

		std::array<char, N> buffer{};
		std::size_t         size = 0;
		bool                truncated = false;
	};

	constexpr std::size_t TEXT_SIZE = 8192;
	using ReportText = Text<TEXT_SIZE>;

	// What is known of the exception, the thread and the game at the crash (the Windows side fills it in).
	enum class Access : std::uint8_t
	{
		kNone,     // not an access violation, or its parameters say nothing
		kRead,
		kWrite,
		kExecute,  // data execution prevention
	};

	enum class Session : std::uint8_t
	{
		kNone,     // no session
		kHosting,
		kJoined,
	};

	struct Facts
	{
		std::uint32_t      code = 0;             // the exception code (EXCEPTION_ACCESS_VIOLATION = 0xC0000005)
		std::uint64_t      address = 0;          // where (the faulting instruction)
		Access             access = Access::kNone;
		std::uint64_t      accessAddress = 0;    // what an access violation read or wrote
		std::uint32_t      threadId = 0;
		bool               mainThread = false;
		HangWatch::Phase   phase = HangWatch::Phase::kPlay;
		Session            session = Session::kNone;
		std::uint32_t      players = 0;          // in the session, counting us
		std::uint64_t      uptimeSeconds = 0;    // since the plugin loaded
		bool               stackCopied = true;   // the thread's stack could be read for the return addresses
		// The exception filter that was there before ours (the game's own, or a plugin's), which gets the exception after
		// this report; address 0: none, so Windows gets it.
		struct Previous
		{
			std::string_view module;
			std::uint64_t    offset = 0;
			std::uint64_t    address = 0;
		} previous;
		std::string_view logger;  // a crash logger's DLL found loaded (Buffout 4, Addictol, Trainwreck); "" when none
	};

	constexpr std::string_view ExceptionName(std::uint32_t a_code)
	{
		switch (a_code) {
		case 0xC0000005:
			return "EXCEPTION_ACCESS_VIOLATION";
		case 0xC000001D:
			return "EXCEPTION_ILLEGAL_INSTRUCTION";
		case 0xC0000094:
			return "EXCEPTION_INT_DIVIDE_BY_ZERO";
		case 0xC0000095:
			return "EXCEPTION_INT_OVERFLOW";
		case 0xC00000FD:
			return "EXCEPTION_STACK_OVERFLOW";
		case 0xC0000096:
			return "EXCEPTION_PRIV_INSTRUCTION";
		case 0xC000008E:
			return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
		case 0xC0000090:
			return "EXCEPTION_FLT_INVALID_OPERATION";
		case 0xC000008C:
			return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
		case 0xC0000006:
			return "EXCEPTION_IN_PAGE_ERROR";
		case 0xC0000409:
			return "STATUS_STACK_BUFFER_OVERRUN (a fast-fail: the runtime found its stack or heap corrupted)";
		case 0xC0000374:
			return "STATUS_HEAP_CORRUPTION";
		case 0x80000003:
			return "EXCEPTION_BREAKPOINT";
		case 0xE06D7363:
			return "a C++ exception nothing caught";
		default:
			return "an exception";
		}
	}

	constexpr std::string_view Describe(Session a_session)
	{
		switch (a_session) {
		case Session::kHosting:
			return "hosting";
		case Session::kJoined:
			return "joined a friend's game";
		default:
			return "no session";
		}
	}

	// A code address as "Fallout4.exe+0x3486C7" (the Windows side names the module; empty: outside any loaded image).
	struct Where
	{
		std::string_view module;  // "" when unknown
		std::uint64_t    offset = 0;
		std::uint64_t    address = 0;
	};

	template <std::size_t N>
	void AppendWhere(Text<N>& a_text, const Where& a_where)
	{
		if (a_where.module.empty()) {
			a_text.AppendHex(a_where.address);
			a_text.Append(" (in no loaded module)");
			return;
		}
		a_text.Append(a_where.module);
		a_text.Append("+");
		a_text.AppendHex(a_where.offset);
	}

	// The report. a_time is "2026-11-04 07:04:37" (local, as the log's), a_version the plugin's; a_callers the return
	// addresses found on the thread's stack, nearest first, already named.
	template <std::size_t N>
	void WriteReport(Text<N>& a_text, std::string_view a_version, std::string_view a_time, const Facts& a_facts, const Where& a_where, std::span<const Where> a_callers)
	{
		a_text.Append("F4Multiplayer ");
		a_text.Append(a_version);
		a_text.Append(" crash report, ");
		a_text.Append(a_time);
		a_text.Append("\n");
		a_text.Append("The game crashed: ");
		a_text.Append(ExceptionName(a_facts.code));
		a_text.Append(" (");
		a_text.AppendHex(a_facts.code);
		a_text.Append(") at ");
		AppendWhere(a_text, a_where);
		if (a_facts.access != Access::kNone) {
			a_text.Append(a_facts.access == Access::kRead ? ", reading " : a_facts.access == Access::kWrite ? ", writing " : ", executing ");
			a_text.AppendHex(a_facts.accessAddress);
			if (a_facts.accessAddress < 0x10000) {
				a_text.Append(" (a null pointer, or a field of one)");
			}
		}
		a_text.Append(", on thread ");
		a_text.AppendDec(a_facts.threadId);
		a_text.Append(a_facts.mainThread ? " (the main thread)" : " (not the main thread)");
		a_text.Append(".\n");
		a_text.Append("The game was ");
		a_text.Append(HangWatch::Describe(a_facts.phase));
		a_text.Append("; the session: ");
		a_text.Append(Describe(a_facts.session));
		if (a_facts.session != Session::kNone) {
			a_text.Append(", ");
			a_text.AppendDec(a_facts.players);
			a_text.Append(a_facts.players == 1 ? " player" : " players");
		}
		a_text.Append("; the plugin had been loaded for ");
		a_text.AppendDec(a_facts.uptimeSeconds);
		a_text.Append(" s.\n");
		if (!a_facts.stackCopied) {
			a_text.Append("The thread's stack couldn't be read, so no return addresses.\n");
		} else if (a_callers.empty()) {
			a_text.Append("No return address was found on the thread's stack.\n");
		} else {
			a_text.Append("Return addresses on its stack, nearest first (a scan, so some may be left over from earlier calls):\n");
			for (const auto& caller : a_callers) {
				a_text.Append("  ");
				AppendWhere(a_text, caller);
				a_text.Append("\n");
			}
		}
		a_text.Append("Modules are named, never their paths: this report is safe to post with a bug report.\n");
		if (a_facts.previous.address != 0) {
			a_text.Append("The exception goes on to the exception filter that was there before the mod's, at ");
			AppendWhere(a_text, { a_facts.previous.module, a_facts.previous.offset, a_facts.previous.address });
			a_text.Append(".\n");
		} else {
			a_text.Append("No other exception filter was there; the exception goes on to Windows.\n");
		}
		if (!a_facts.logger.empty()) {
			// Seen only when the logger let the exception reach the filters (Addictol ends the process from its vectored
			// handler first, so with it no report of ours is written at all).
			a_text.Append("A crash logger is loaded (");
			a_text.Append(a_facts.logger);
			a_text.Append("): its own crash log in this folder, when it wrote one, says more.\n");
		} else {
			a_text.Append("No crash logger (Buffout 4, Addictol) is loaded; one would say more.\n");
		}
	}

	// --- handing the exception on ---

	// Two filters may be known: the one found at our install (the game's own, or a plugin's that loaded before us), and
	// one a plugin that loaded after us put in front of ours, which OnDataReady displaced to put ours back in front. A
	// displaced filter typically chains to what it found at its own install, which is ours: so ours is entered again
	// on the same thread. That re-entry (a_depth 1) gets nothing more from us (no report, never the displaced filter
	// again): it goes straight to the install-time filter, or to Windows. An install-time filter that itself calls the
	// system's UnhandledExceptionFilter comes back to ours once more (a_depth 2): that goes to Windows, so the chain
	// ends whatever the filters do. Without that, two filters would call each other until the stack ran out and every
	// log was lost.
	enum class Next : std::uint8_t
	{
		kDisplaced,    // the filter we displaced at OnDataReady: it then chains back to us, and the re-entry ends it
		kInstallTime,  // the filter found at our install
		kSearch,       // Windows (EXCEPTION_CONTINUE_SEARCH): no filter is left
	};

	// a_depth: how many times this thread was inside our filter already when it was entered (0: the first time).
	constexpr Next HandOn(int a_depth, bool a_hasDisplaced, bool a_hasInstallTime)
	{
		if (a_depth == 0 && a_hasDisplaced) {
			return Next::kDisplaced;
		}
		if (a_depth <= 1 && a_hasInstallTime) {
			return Next::kInstallTime;
		}
		return Next::kSearch;
	}

	// --- the files ---

	constexpr std::string_view FILE_PREFIX = "F4Multiplayer-crash-";
	constexpr std::string_view SUFFIX = ".txt";
	constexpr std::size_t      KEEP_FILES = 3;

	// "F4Multiplayer-crash-2026-11-04_07-04-37.txt": local time, as the log's, in an order that sorts by name.
	template <std::size_t N>
	void AppendFileName(Text<N>& a_text, int a_year, int a_month, int a_day, int a_hour, int a_minute, int a_second)
	{
		a_text.Append(FILE_PREFIX);
		a_text.AppendDec(static_cast<std::uint64_t>(a_year), 4);
		a_text.Append("-");
		a_text.AppendDec(static_cast<std::uint64_t>(a_month), 2);
		a_text.Append("-");
		a_text.AppendDec(static_cast<std::uint64_t>(a_day), 2);
		a_text.Append("_");
		a_text.AppendDec(static_cast<std::uint64_t>(a_hour), 2);
		a_text.Append("-");
		a_text.AppendDec(static_cast<std::uint64_t>(a_minute), 2);
		a_text.Append("-");
		a_text.AppendDec(static_cast<std::uint64_t>(a_second), 2);
		a_text.Append(SUFFIX);
	}

	// Which of a_names (the files in the log folder) go so that only the newest KEEP_FILES reports stay, and no
	// half-written one (the hang watch's rule, with this prefix). Run at start-up, not at the crash.
	inline std::vector<std::string> FilesToRemove(std::vector<std::string> a_names, std::size_t a_keep)
	{
		return HangWatch::HangFilesToRemove(std::move(a_names), a_keep, FILE_PREFIX);
	}

	// A module's file name from its mapped path, into a fixed buffer, ASCII only ('?' for the rest), as the hang
	// watch's ModuleName, without allocating. How many characters were written (the buffer is ended with '\0').
	inline std::size_t ModuleNameInto(std::wstring_view a_path, std::span<char> a_out)
	{
		if (a_out.empty()) {
			return 0;
		}
		const auto slash = a_path.find_last_of(L"\\/");
		const auto name = slash == std::wstring_view::npos ? a_path : a_path.substr(slash + 1);
		std::size_t n = 0;
		for (const auto c : name) {
			if (n + 1 >= a_out.size()) {
				break;
			}
			a_out[n++] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
		}
		a_out[n] = '\0';
		return n;
	}
}
