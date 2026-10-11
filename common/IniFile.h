#pragma once

#include <string>
#include <string_view>

// Rewriting one "key = value" line of an ini file (the plugin's settings file) without touching
// anything else: comments, blank lines, the other keys and the line endings all stay as they
// were, so a file the player edited by hand reads the same afterwards. Pure text in, text out:
// the game plugin reads and writes the file, the tests check the text.
namespace IniFile
{
	namespace detail
	{
		inline std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r");
			return a_str.substr(first, last - first + 1);
		}

		// The key of a "key = value" line; empty for comments, headers and blank lines.
		inline std::string_view KeyOf(std::string_view a_line)
		{
			const auto view = Trim(a_line);
			if (view.empty() || view.front() == '#' || view.front() == ';' || view.front() == '[') {
				return {};
			}
			const auto eq = view.find('=');
			return eq == std::string_view::npos ? std::string_view{} : Trim(view.substr(0, eq));
		}
	}

	// Returns a_text with the first "a_key = ..." line replaced by "a_key = a_value" (keys match
	// exactly, as the plugin reads them). A key the file doesn't have is appended at the end. The
	// file's own line ending (CRLF or LF) is kept.
	[[nodiscard]] inline std::string SetValue(std::string_view a_text, std::string_view a_key, std::string_view a_value)
	{
		const std::string_view eol = a_text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		const std::string      line = std::string{ a_key } + " = " + std::string{ a_value };

		std::string out;
		out.reserve(a_text.size() + line.size() + eol.size());
		bool replaced = false;
		for (std::size_t start = 0; start < a_text.size();) {
			const auto newline = a_text.find('\n', start);
			const auto end = newline == std::string_view::npos ? a_text.size() : newline + 1;
			const auto current = a_text.substr(start, end - start);
			if (!replaced && detail::KeyOf(current) == a_key) {
				out += line;
				if (newline != std::string_view::npos) {
					out += current.ends_with("\r\n") ? "\r\n" : "\n";
				}
				replaced = true;
			} else {
				out += current;
			}
			start = end;
		}
		if (!replaced) {
			if (!out.empty() && !out.ends_with('\n')) {
				out += eol;
			}
			out += line;
			out += eol;
		}
		return out;
	}
}
