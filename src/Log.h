#pragma once

// The plugin's log (Documents/My Games/Fallout4/F4SE/F4Multiplayer.log). CommonLib's F4SE::Init opens
// it; this keeps the last launches' logs and makes sure no line holds a whole SteamID64 or IP address
// (see Redact.h: players attach this file to public bug reports).
namespace Log
{
	// Before F4SE::Init: the previous logs move up to F4Multiplayer.1.log to .3.log. Returns why the
	// previous log could not be kept (logged once the log is open), or no error.
	std::error_code RotateBeforeInit(std::string_view a_saveFolder);

	// Right after F4SE::Init, before any thread of ours starts: every line passes through Redact::Scrub
	// on its way to the file. A net under the call sites, which log players and accounts by alias already.
	void InstallScrubber();

	// "info" (the default: what players attach) or "debug" (also the friend scans and HUD messages,
	// still by alias). sLogLevel in the ini, or the dev channel's "log level". False for anything else.
	bool                           SetLevel(std::string_view a_level);
	[[nodiscard]] std::string_view Level();

	void Flush();

	// Where the log is (empty before RotateBeforeInit, or without a Documents folder).
	[[nodiscard]] const std::filesystem::path& Path();
}
