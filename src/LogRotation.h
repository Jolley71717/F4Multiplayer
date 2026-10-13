#pragma once

#include <filesystem>
#include <format>
#include <system_error>

// Keeping the last few logs. The game's log is emptied every launch, so a player who restarted after
// a problem used to have nothing left to attach. Before the log is opened again, F4Multiplayer.log
// becomes F4Multiplayer.1.log, .1 becomes .2, and so on; the oldest goes. No game needed.
namespace LogRotation
{
	// "F4Multiplayer.log", 2 -> "F4Multiplayer.2.log".
	inline std::filesystem::path Kept(const std::filesystem::path& a_log, int a_index)
	{
		auto name = a_log.stem();
		name += std::format(".{}", a_index);
		name += a_log.extension();
		return a_log.parent_path() / name;
	}

	// Moves a_log to its .1 copy, keeping a_keep copies. A missing or empty log is left alone (a launch
	// that wrote nothing is not worth a place). Never throws: a file another program holds open (a log
	// viewer, an editor, a sync client) only means that copy is not kept, and the game still starts. The
	// first error is returned; an error on the last step means the previous log will be overwritten.
	inline std::error_code Rotate(const std::filesystem::path& a_log, int a_keep)
	{
		std::error_code ec;
		const auto      size = std::filesystem::file_size(a_log, ec);
		if (ec || size == 0 || a_keep < 1) {
			return {};
		}
		std::error_code first;
		for (int i = a_keep - 1; i >= 0; --i) {
			const auto from = i == 0 ? a_log : Kept(a_log, i);
			if (!std::filesystem::exists(from, ec)) {
				continue;
			}
			std::filesystem::rename(from, Kept(a_log, i + 1), ec);  // replaces the older copy
			if (ec && !first) {
				first = ec;
			}
		}
		return first;
	}
}
