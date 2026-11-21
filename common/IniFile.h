#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Rewriting one "key = value" line of an ini file (the plugin's settings file) without touching
// anything else: comments, blank lines, the other keys and the line endings all stay as they
// were, so a file the player edited by hand reads the same afterwards; and adding the keys a
// newer version brings (AddMissing) the same way. Pure text in, text out: the game plugin reads
// and writes the file, the tests check the text.
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

	// The value of the first "a_key = ..." line, or nothing. a_ignoreCase is for the game's own ini files (Fallout4.ini and
	// the others read keys without regard to case); the plugin's own keys match exactly. A leading UTF-8 byte order mark is ignored.
	[[nodiscard]] inline std::optional<std::string> GetValue(std::string_view a_text, std::string_view a_key, bool a_ignoreCase = false)
	{
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);
		}
		const auto same = [&](std::string_view a_a, std::string_view a_b) {
			return a_a.size() == a_b.size() && std::ranges::equal(a_a, a_b, [&](char a_x, char a_y) {
				return a_ignoreCase ? std::tolower(static_cast<unsigned char>(a_x)) == std::tolower(static_cast<unsigned char>(a_y)) : a_x == a_y;
			});
		};
		for (std::size_t start = 0; start < a_text.size();) {
			const auto newline = a_text.find('\n', start);
			const auto end = newline == std::string_view::npos ? a_text.size() : newline + 1;
			const auto line = detail::Trim(a_text.substr(start, (newline == std::string_view::npos ? end : newline) - start));
			start = end;
			if (same(detail::KeyOf(line), a_key)) {
				return std::string{ detail::Trim(line.substr(line.find('=') + 1)) };
			}
		}
		return std::nullopt;
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

	// What AddMissing made of a player's file: the text, and the keys it added (none: the text is a_text as it was).
	struct Merged
	{
		std::string              text;
		std::vector<std::string> added;
	};

	// Returns a_text with every key a_defaults has and a_text lacks appended at its end, at the default's value and
	// with the comment lines a_defaults puts above it (the block above a run of keys serves every key of the run), in
	// a_defaults' order and with a_text's own line ending; nothing else of a_text changes: the keys it has keep their
	// value, their place and their comments, and a key a_defaults no longer has (an older version's setting) stays.
	// a_note, if given, heads what is added (one comment line). A key commented out in a_text is a key a_text lacks.
	// A leading UTF-8 byte order mark is ignored in both. The plugin's settings file after an update: the player's
	// values survive, and the new version's settings show up with their explanations (src/Config.cpp).
	[[nodiscard]] inline Merged AddMissing(std::string_view a_text, std::string_view a_defaults, std::string_view a_note = {})
	{
		static constexpr std::string_view BOM = "\xEF\xBB\xBF";
		const auto                        lines = [](std::string_view a_src, auto&& a_each) {
			if (a_src.starts_with(BOM)) {
				a_src.remove_prefix(BOM.size());
			}
			for (std::size_t start = 0; start < a_src.size();) {
				const auto newline = a_src.find('\n', start);
				const auto end = newline == std::string_view::npos ? a_src.size() : newline;
				auto       line = a_src.substr(start, end - start);
				if (line.ends_with('\r')) {
					line.remove_suffix(1);
				}
				a_each(line);
				start = newline == std::string_view::npos ? a_src.size() : newline + 1;
			}
		};

		std::vector<std::string_view> present;
		lines(a_text, [&](std::string_view a_line) {
			if (const auto key = detail::KeyOf(a_line); !key.empty()) {
				present.push_back(key);
			}
		});

		const std::string_view eol = a_text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		Merged                 merged;
		std::string            appended;
		std::vector<std::string_view> block;  // the comment lines above the run of keys being read
		bool                          blockWritten = false;
		bool                          lastWasKey = false;
		lines(a_defaults, [&](std::string_view a_line) {
			const auto trimmed = detail::Trim(a_line);
			const auto key = detail::KeyOf(a_line);
			if (!key.empty()) {
				const auto is = [&](std::string_view a_name) { return a_name == key; };
				if (!std::ranges::any_of(present, is) && !std::ranges::any_of(merged.added, is)) {
					if (!blockWritten) {
						if (!appended.empty()) {
							appended += eol;  // a blank line between groups, as the defaults have
						}
						for (const auto& comment : block) {
							appended += comment;
							appended += eol;
						}
						blockWritten = true;
					}
					appended += trimmed;
					appended += eol;
					merged.added.emplace_back(key);
				}
				lastWasKey = true;
			} else if (!trimmed.empty() && (trimmed.front() == '#' || trimmed.front() == ';')) {
				if (lastWasKey) {
					block.clear();  // a comment after a key opens the next key's block
					blockWritten = false;
				}
				block.push_back(trimmed);
				lastWasKey = false;
			} else {
				block.clear();
				blockWritten = false;
				lastWasKey = false;
			}
		});

		merged.text = a_text;
		if (appended.empty()) {
			return merged;
		}
		if (!merged.text.empty() && !merged.text.ends_with('\n')) {
			merged.text += eol;
		}
		if (!merged.text.empty()) {
			merged.text += eol;
		}
		if (!a_note.empty()) {
			merged.text += a_note;
			merged.text += eol;
			merged.text += eol;
		}
		merged.text += appended;
		return merged;
	}
}
