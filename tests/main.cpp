#include "Test.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif

namespace
{
	// A run that ends early never ends silently: a crash, abort() or std::terminate prints "CRASHED in <test>" on
	// stderr and exits with this code; an exit() from inside a test prints "EXITED in <test>" (its own code stands).
	constexpr int kCrashExit = 99;

	// The test running now, or null between tests and after the summary.
	std::atomic<const char*> g_running{ nullptr };

	const char* RunningName()
	{
		const char* name = g_running.load();
		return name ? name : "(no test: between tests)";
	}

	[[noreturn]] void Crashed(const char* a_what)
	{
		std::fprintf(stderr, "\nCRASHED in %s: %s\n", RunningName(), a_what);
		std::fflush(stderr);
		std::_Exit(kCrashExit);
	}

	void OnSignal(int a_signal)
	{
		switch (a_signal) {
		case SIGABRT:
			Crashed("SIGABRT (abort())");
		case SIGSEGV:
			Crashed("SIGSEGV (a bad memory access)");
		case SIGFPE:
			Crashed("SIGFPE (an arithmetic fault)");
		case SIGILL:
			Crashed("SIGILL (an illegal instruction)");
		default:
			Crashed("a signal");
		}
	}

	void OnTerminate()
	{
		// An exception that left a thread, a noexcept function or a destructor, or a join-less std::thread.
		if (const auto current = std::current_exception()) {
			try {
				std::rethrow_exception(current);
			} catch (const std::exception& e) {
				char what[512];
				std::snprintf(what, sizeof what, "std::terminate: an uncaught exception: %s", e.what());
				Crashed(what);
			} catch (...) {
				Crashed("std::terminate: an uncaught exception of an unknown type");
			}
		}
		Crashed("std::terminate (no exception in flight)");
	}

	void OnExit()
	{
		// exit() (or a return from main) before the summary: name the test that called it.
		if (g_running.load()) {
			std::fprintf(stderr, "\nEXITED in %s: exit() was called before the summary\n", RunningName());
			std::fflush(stderr);
		}
	}

#ifdef _WIN32
	LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* a_info)
	{
		char what[128];
		const auto* record = a_info ? a_info->ExceptionRecord : nullptr;
		std::snprintf(what, sizeof what, "exception 0x%08lX at %p", record ? record->ExceptionCode : 0ul,
			record ? record->ExceptionAddress : nullptr);
		Crashed(what);
	}

	void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t)
	{
		// The release CRT would fast-fail here, past every handler above.
		Crashed("the C runtime's invalid parameter handler (a bad argument to a CRT function)");
	}

	void OnPureCall()
	{
		Crashed("a pure virtual function call");
	}
#endif

	void InstallCrashHandlers()
	{
		// SIGABRT is process-wide everywhere. A test's own thread that ends in std::terminate reaches it too on
		// Windows, where set_terminate holds for the calling thread only and the default handler calls abort().
		std::signal(SIGABRT, OnSignal);
		std::set_terminate(OnTerminate);
		std::atexit(OnExit);
#ifndef _WIN32
		std::signal(SIGSEGV, OnSignal);
		std::signal(SIGFPE, OnSignal);
		std::signal(SIGILL, OnSignal);
#else
		// Faults go to the unhandled exception filter (it names the exception code and address); the CRT's
		// SIGSEGV/SIGFPE/SIGILL would hide them and hold for one thread only.
		// No abort() message box or Windows Error Reporting dialog: a run left waiting on one looks like a hang.
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
		SetUnhandledExceptionFilter(OnUnhandled);
		_set_invalid_parameter_handler(OnInvalidParameter);
		_set_purecall_handler(OnPureCall);
#endif
	}
}

int main(int a_argc, char** a_argv)
{
	// Unbuffered: through a pipe or a redirect stdout is fully buffered, so a run that ended early lost the last
	// few dozen test lines and the last line shown was not the last test that finished.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	InstallCrashHandlers();

	const char* filter = a_argc > 1 ? a_argv[1] : nullptr;
	int         run = 0;
	int         failed = 0;
	int         known = 0;  // known bugs still failing
	for (const auto& test : Test::Cases()) {
		if (filter && !std::strstr(test.name, filter)) {
			continue;
		}
		++run;
		const int before = Test::Failures();
		const int knownBefore = Test::KnownFailures();
		Test::RunningKnownBug() = test.knownBug != nullptr;
		g_running = test.name;
		try {
			test.body();
		} catch (const Test::Abort&) {
		} catch (const std::exception& e) {
			Test::Fail(test.name, 0, std::string("exception: ") + e.what());
		} catch (...) {
			Test::Fail(test.name, 0, "an exception of a type that is not std::exception");
		}
		g_running = nullptr;
		Test::RunningKnownBug() = false;
		const bool ok = Test::Failures() == before;
		if (test.knownBug && ok) {
			// The symptom still failing is what is expected; its passing means the bug is gone and the mark
			// should go too. A failure of anything else is a plain failure (below).
			if (Test::KnownFailures() == knownBefore) {
				++Test::Failures();
				++failed;
				std::printf("FIXED? %s: passes now; make it a plain TEST (known bug: %s)\n", test.name, test.knownBug);
			} else {
				std::printf("known  %s (bug: %s)\n", test.name, test.knownBug);
				++known;
			}
			continue;
		}
		failed += !ok;
		std::printf("%s %s\n", ok ? "ok    " : "FAILED", test.name);
	}
	std::printf("\n%d tests, %d failed", run, failed);
	if (known > 0) {
		std::printf(", %d known bugs still failing", known);
	}
	std::printf("\n");
	return failed;
}
