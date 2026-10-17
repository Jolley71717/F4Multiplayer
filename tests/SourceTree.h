#pragma once

// The repository's own source, for the tests that guard it (what the game's log may say, which game calls the
// plugin may make): those rules are on code that only runs in the game, so they are checked where it is written.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

	// a_text without its comments and its string and character literals, line breaks kept: what a source guard
	// looks for is code, not a name in a comment or a log line.
	inline std::string CodeOnly(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '/') {
				while (i < a_text.size() && a_text[i] != '\n') {
					++i;
				}
				out += '\n';
				continue;
			}
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '*') {
				for (i += 2; i + 1 < a_text.size() && !(a_text[i] == '*' && a_text[i + 1] == '/'); ++i) {
					out += a_text[i] == '\n' ? "\n" : "";
				}
				++i;
				continue;
			}
			if (c == '"' || c == '\'') {
				for (++i; i < a_text.size() && a_text[i] != c; ++i) {
					i += a_text[i] == '\\';  // what a backslash escapes is skipped
				}
				out += ' ';
				continue;
			}
			out += c;
		}
		return out;
	}

	// The plugin's own source files (.cpp and .h under src), relative to the root, with the code of each.
	inline std::vector<std::pair<std::string, std::string>> PluginCode(const std::filesystem::path& a_root)
	{
		std::vector<std::pair<std::string, std::string>> files;
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ a_root / "src" }) {
			const auto ext = entry.path().extension();
			if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
				continue;
			}
			files.emplace_back(std::filesystem::relative(entry.path(), a_root).generic_string(), CodeOnly(Read(entry.path())));
		}
		return files;
	}
}
