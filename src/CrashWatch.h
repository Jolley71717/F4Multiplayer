#pragma once

#include "CrashReport.h"

#include <cstdint>

// The crash report's exception filter (CrashReport.h says what it writes and why). Windows calls the unhandled-exception
// filter on the thread that crashed, with its heap and locks in whatever state the crash left them: so everything the
// filter touches was set aside at Install (buffers, the folder, the version), and it uses system calls only. Then the
// exception goes on to the filter installed before (a crash logger's, when one loaded before the plugin), or to
// Windows. Nothing is swallowed, nothing retried; the game closes as it would have.
// An abort() is no exception: it ends the process with a fast fail that no filter and no crash logger sees (the rig,
// 2026-11-12: a game gone with nothing but Windows' Application log entry, ucrtbase.dll 0xc0000409). So a SIGABRT
// handler writes the same report first, saying what called abort: std::terminate on the main thread (with the
// exception's message), a pure virtual call, or abort itself (std::terminate on another thread, a runtime check).
namespace CrashWatch
{
	// At plugin load, after the log is open and the ini read (bCrashReport): puts the filter in front of the one there
	// and removes old report files (the newest CrashReport::KEEP_FILES stay). Logs what it did.
	void Install();

	// Once the game's data is loaded, so every plugin is in: notes which crash logger's DLL is loaded (for the report), and
	// if a plugin loaded after ours took the filter, puts ours back in front of it, so that one gets the exception after
	// the report. Logged.
	void OnDataReady();

	// What the session is, for the report: from Session::Frame. Any thread.
	void SetSession(CrashReport::Session a_session, std::uint32_t a_players);

	// The dev channel's `crashtest` (docs/DEV-CHANNEL.md): crashes the game on purpose, so the rig can test the report.
	// Never reached by players (the dev channel is off for them).
	enum class Kind
	{
		kRead,   // an access violation reading a null pointer's field
		kWrite,  // ... writing one
		kCpp,    // a C++ exception nothing catches
		kStack,  // a stack overflow
		kAbort,      // abort() (a fast fail: only our SIGABRT handler sees it)
		kTerminate,  // std::terminate while a C++ exception is handled (its message goes into the report)
	};
	// On this thread, never returns.
	[[noreturn]] void CrashNow(Kind a_kind);
	// On a thread made for it (a plain Windows thread: no handler of the runtime's or ours above the crash).
	void CrashOnNewThread(Kind a_kind);
}
