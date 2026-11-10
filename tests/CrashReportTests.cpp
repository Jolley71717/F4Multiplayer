#include "Test.h"

#include "CrashReport.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

using namespace std::string_view_literals;

namespace
{
	CrashReport::Facts AccessViolation()
	{
		CrashReport::Facts facts;
		facts.code = 0xC0000005;
		facts.address = 0x7FF6801A86C7;
		facts.access = CrashReport::Access::kRead;
		facts.accessAddress = 0xC8;
		facts.threadId = 1234;
		facts.mainThread = true;
		facts.phase = HangWatch::Phase::kSave;
		facts.session = CrashReport::Session::kHosting;
		facts.players = 2;
		facts.uptimeSeconds = 1286;
		return facts;
	}

	std::string Report(const CrashReport::Facts& a_facts, std::span<const CrashReport::Where> a_callers = {})
	{
		CrashReport::ReportText text;
		CrashReport::WriteReport(text, "0.16.1", "2026-11-04 07:04:37", a_facts, { "Fallout4.exe", 0x3486C7, 0x7FF6801A86C7 }, a_callers);
		return std::string{ text.View() };
	}
}

TEST("crash report: the fixed text takes words, hex and padded numbers, and never runs past its end")
{
	CrashReport::Text<16> text;
	text.Append("ab");
	text.AppendHex(0xC8);
	text.AppendDec(7, 3);
	CHECK(text.View() == "ab0xC8007");
	CHECK(!text.Truncated());
	text.AppendDec(0);
	CHECK(text.View() == "ab0xC80070");
	text.Append("0123456789");  // past 15 characters: cut, and marked
	CHECK(text.Truncated());
	CHECK(text.View().size() <= 15);
	CHECK(text.View().ends_with("...\n"));
	text.Append("more");  // dropped
	CHECK(text.View().size() <= 15);
	text.Clear();
	CHECK(text.View().empty());
	CHECK(!text.Truncated());
	text.AppendHex(0);
	CHECK(text.View() == "0x0");
	CrashReport::Text<4> tiny;
	tiny.Append("abcdef");
	CHECK(tiny.View().size() <= 3);
}

TEST("crash report: the exception codes a crash log names")
{
	CHECK(CrashReport::ExceptionName(0xC0000005) == "EXCEPTION_ACCESS_VIOLATION");
	CHECK(CrashReport::ExceptionName(0xC00000FD) == "EXCEPTION_STACK_OVERFLOW");
	CHECK(CrashReport::ExceptionName(0xC000001D) == "EXCEPTION_ILLEGAL_INSTRUCTION");
	CHECK(CrashReport::ExceptionName(0xE06D7363).starts_with("a C++ exception"));
	CHECK(CrashReport::ExceptionName(0x12345678) == "an exception");
}

TEST("crash report: the report says what, where, which thread, the phase, the session, the callers, and no paths")
{
	const std::array<CrashReport::Where, 2> callers{ { { "Fallout4.exe", 0x34D7BC, 0x7FF6801AD7BC }, { "KERNEL32.DLL", 0x2DDA7, 0x7FFAD521DDA7 } } };
	const auto                              text = Report(AccessViolation(), callers);
	CHECK(text.starts_with("F4Multiplayer 0.16.1 crash report, 2026-11-04 07:04:37\n"));
	CHECK(text.find("EXCEPTION_ACCESS_VIOLATION (0xC0000005) at Fallout4.exe+0x3486C7, reading 0xC8 (a null pointer, or a field of one), on thread 1234 (the main thread).") != std::string::npos);
	CHECK(text.find("The game was while saving; the session: hosting, 2 players; the plugin had been loaded for 1286 s.") != std::string::npos);
	CHECK(text.find("  Fallout4.exe+0x34D7BC\n  KERNEL32.DLL+0x2DDA7\n") != std::string::npos);
	CHECK(text.find("No other exception filter was there; the exception goes on to Windows.") != std::string::npos);
	CHECK(text.find("No crash logger (Buffout 4, Addictol) is loaded") != std::string::npos);
	CHECK(text.find('\\') == std::string::npos);
	CHECK(text.find('/') == std::string::npos);

	auto facts = AccessViolation();
	facts.access = CrashReport::Access::kWrite;
	facts.accessAddress = 0x2243892B878;
	facts.mainThread = false;
	facts.session = CrashReport::Session::kJoined;
	facts.players = 1;
	facts.previous = { "Fallout4.exe", 0x22BFFB4, 0x7FF68211FFB4 };
	facts.logger = "AddictolCrashLogger.dll";
	facts.phase = HangWatch::Phase::kPlay;
	const auto other = Report(facts);
	CHECK(other.find(", writing 0x2243892B878, on thread 1234 (not the main thread).") != std::string::npos);
	CHECK(other.find("null pointer") == std::string::npos);
	CHECK(other.find("in play or a menu; the session: joined a friend's game, 1 player;") != std::string::npos);
	CHECK(other.find("No return address was found") != std::string::npos);
	CHECK(other.find("goes on to the exception filter that was there before the mod's, at Fallout4.exe+0x22BFFB4.") != std::string::npos);
	CHECK(other.find("game's own") == std::string::npos);  // an address in the game is named, not explained
	CHECK(other.find("A crash logger is loaded (AddictolCrashLogger.dll)") != std::string::npos);
	CHECK(other.find("No crash logger") == std::string::npos);

	facts.previous = { "Buffout4.dll", 0x1234, 0x7FFA00001234 };
	CHECK(Report(facts).find("at Buffout4.dll+0x1234.\n") != std::string::npos);

	facts.session = CrashReport::Session::kNone;
	facts.access = CrashReport::Access::kNone;
	facts.code = 0xC00000FD;
	facts.stackCopied = false;
	const auto overflow = Report(facts);
	CHECK(overflow.find("EXCEPTION_STACK_OVERFLOW (0xC00000FD) at Fallout4.exe+0x3486C7, on thread") != std::string::npos);
	CHECK(overflow.find("the session: no session;") != std::string::npos);
	CHECK(overflow.find(" player") == std::string::npos);  // "F4Multiplayer" holds the word
	CHECK(overflow.find("The thread's stack couldn't be read") != std::string::npos);
}

TEST("crash report: a displaced filter that chains back to ours ends at the install-time filter, never in a loop")
{
	using CrashReport::HandOn;
	using CrashReport::Next;
	CHECK(HandOn(0, true, true) == Next::kDisplaced);
	CHECK(HandOn(0, false, true) == Next::kInstallTime);
	CHECK(HandOn(0, false, false) == Next::kSearch);
	CHECK(HandOn(1, true, true) == Next::kInstallTime);  // re-entered: never the displaced one again
	CHECK(HandOn(1, true, false) == Next::kSearch);
	CHECK(HandOn(1, false, true) == Next::kInstallTime);
	CHECK(HandOn(2, true, true) == Next::kSearch);  // entered a third time: the install-time filter came back too
	CHECK(HandOn(3, true, true) == Next::kSearch);

	// Fake filters, called as Windows calls them, each chaining to what it found at its install: the install-time one
	// (which, like the system's UnhandledExceptionFilter, calls the current filter: ours), ours, then a plugin's that
	// found ours and was displaced. One crash: ours, the plugin's, ours again, the install-time one, ours a third time,
	// Windows; then every call returns.
	constexpr int SEARCH = 0;
	std::vector<std::string> calls;
	int                      inOurs = 0;
	int                      depth = 0;
	std::function<int()>     ours;
	std::function<int()>     installTime = [&] { calls.push_back("install-time"); return ours(); };
	std::function<int()>     plugin = [&] { calls.push_back("plugin"); return ours(); };
	ours = [&] {
		REQUIRE(++depth < 10);  // the loop the rule prevents
		const int level = inOurs++;
		calls.push_back(level == 0 ? "ours" : level == 1 ? "ours again" : "ours a third time");
		int verdict = SEARCH;
		switch (HandOn(level, true, true)) {
		case Next::kDisplaced:
			verdict = plugin();
			break;
		case Next::kInstallTime:
			verdict = installTime();
			break;
		default:
			break;
		}
		--inOurs;
		--depth;
		return verdict;
	};
	CHECK(ours() == SEARCH);
	const std::vector<std::string> expected{ "ours", "plugin", "ours again", "install-time", "ours a third time" };
	CHECK(calls == expected);
	CHECK(inOurs == 0);
	calls.clear();
	CHECK(ours() == SEARCH);  // a second time (a filter that let the thread go on): the same, not a re-entry
	CHECK(calls.front() == "ours");
	CHECK(calls.size() == expected.size());
}

TEST("crash report: an address in no module is given as it is")
{
	CrashReport::Text<64> text;
	CrashReport::AppendWhere(text, { "", 0, 0x1234ABCD });
	CHECK(text.View() == "0x1234ABCD (in no loaded module)");
}

TEST("crash report: a module is named by its file name only, into a fixed buffer")
{
	std::array<char, 16> name{};
	CHECK(CrashReport::ModuleNameInto(L"\\Device\\HarddiskVolume3\\Users\\someone\\Games\\Fallout 4\\Fallout4.exe", name) == 12);
	CHECK(std::string_view{ name.data() } == "Fallout4.exe");
	CHECK(CrashReport::ModuleNameInto(L"C:/Windows/System32/ntdll.dll", name) == 9);
	CHECK(std::string_view{ name.data() } == "ntdll.dll");
	CHECK(CrashReport::ModuleNameInto(L"\\x\\caf\u00e9.dll", name) == 8);
	CHECK(std::string_view{ name.data() } == "caf?.dll");
	CHECK(CrashReport::ModuleNameInto(L"a-very-long-module-name.dll", name) == 15);  // cut to the buffer, ended
	CHECK(std::string_view{ name.data() } == "a-very-long-mod");
	CHECK(CrashReport::ModuleNameInto(L"", name) == 0);
	CHECK(CrashReport::ModuleNameInto(L"x.dll", std::span<char>{}) == 0);
}

TEST("crash report: file names sort by time, and only the newest three are kept, apart from the hang watch's")
{
	const auto name = [](int a_day, int a_hour) {
		CrashReport::Text<64> text;
		CrashReport::AppendFileName(text, 2026, 11, a_day, a_hour, 4, 37);
		return std::string{ text.View() };
	};
	CHECK(name(4, 7) == "F4Multiplayer-crash-2026-11-04_07-04-37.txt");
	CHECK(name(4, 7) < name(4, 8));
	CHECK(name(9, 23) < name(10, 0));

	const std::vector<std::string> files{
		"F4Multiplayer.log",
		name(4, 7),
		name(2, 1),
		name(3, 5) + ".tmp",
		name(3, 5),
		name(1, 0),
		"F4Multiplayer-hang-2026-11-01_00-00-00.txt",  // the hang watch's: not ours to remove
		"crash-2026-11-04-07-04-37.log",               // a crash logger's
		"F4Multiplayer-crash-notes.txt",               // the player's own
	};
	auto remove = CrashReport::FilesToRemove(files, 3);
	std::ranges::sort(remove);
	std::vector<std::string> expected{ name(1, 0), name(3, 5) + ".tmp" };
	std::ranges::sort(expected);
	CHECK(remove == expected);
	// And the hang watch's own rule still sees only its files.
	CHECK(HangWatch::HangFilesToRemove(files, 1).empty());
}
