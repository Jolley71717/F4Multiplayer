#pragma once

// The repository's own source, for the tests that guard it (what the game's log may say, which game calls the
// plugin may make): those rules are on code that only runs in the game, so they are checked where it is written.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SourceTree
{
	// The repository the tests were built in (they run from its build folder), or else the one they run inside.
	inline std::optional<std::filesystem::path> Root()
	{
		std::vector<std::filesystem::path> starts;
		if (char* exe = nullptr; _get_pgmptr(&exe) == 0 && exe && *exe) {
			starts.emplace_back(std::filesystem::path{ exe }.parent_path());
		}
		starts.emplace_back(std::filesystem::current_path());
		for (auto at : starts) {
			for (;; at = at.parent_path()) {
				std::error_code error;
				if (std::filesystem::exists(at / "src" / "game" / "RemotePlayers.cpp", error) && std::filesystem::exists(at / "common" / "Server.cpp", error)) {
					return at;
				}
				if (at == at.parent_path()) {
					break;
				}
			}
		}
		return std::nullopt;
	}

	inline std::string Read(const std::filesystem::path& a_file)
	{
		std::ifstream      file{ a_file, std::ios::binary };
		std::ostringstream text;
		text << file.rdbuf();
		return text.str();
	}
}
