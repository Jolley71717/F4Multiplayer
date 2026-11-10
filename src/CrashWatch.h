#pragma once

#include "CrashReport.h"

#include <cstdint>

// The crash report's exception filter (CrashReport.h says what it writes and why). Windows calls the unhandled-exception
// filter on the thread that crashed, with its heap and locks in whatever state the crash left them: so everything the
// filter touches was set aside at Install (buffers, the folder, the version), and it uses system calls only. Then the
// exception goes on to the filter installed before (a crash logger's, when one loaded before the plugin), or to
// Windows. Nothing is swallowed, nothing retried; the game closes as it would have.
namespace CrashWatch
{
	// At plugin load, after the log is open and the ini read (bCrashReport): puts the filter in front of the one there
	// and removes old report files (the newest CrashReport::KEEP_FILES stay). Logs what it did.
	void Install();

	// What the session is, for the report: from Session::Frame. Any thread.
	void SetSession(CrashReport::Session a_session, std::uint32_t a_players);
}
