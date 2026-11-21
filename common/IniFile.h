#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Rewriting one "key = value" line of an ini file (the plugin's settings file) without touching
// anything else: comments, blank lines, the other keys and the line endings all stay as they
// were, so a file the player edited by hand reads the same afterwards; and adding the keys a
// newer version brings (AddMissing) the same way. Those are pure text in, text out, which the tests
// check; LoadOrCreate does the start-up step on the file itself (written when missing, extended when
// it lacks keys), for the game plugin and, on a temporary folder, the tests.
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

	// What LoadOrCreate found and did with the settings file.
	enum class Kept
	{
		Created,      // there was none: written from the defaults
		NotCreated,   // there was none and it could not be written
		Unreadable,   // there is one, but it can't be read: left alone
		Complete,     // it lacks nothing: left alone
		Extended,     // the keys it lacked were appended to it
		NotExtended,  // it lacks keys and could not be written to
	};

	struct Loaded
	{
		std::string              text;  // the settings to read: the file's, or the defaults when it can't be read
		Kept                     what = Kept::Complete;
		std::vector<std::string> added;  // the keys the file lacked
	};

	// Writes a_text as the whole file.
	[[nodiscard]] inline bool WriteText(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::ofstream out{ a_path, std::ios::binary | std::ios::trunc };
		return out && out.write(a_text.data(), static_cast<std::streamsize>(a_text.size())) && out.flush();
	}

	// The settings file at a_path brought up to a_defaults, and its text: with no file there, a_defaults is written
	// as the file; a file that lacks keys of a_defaults gets them appended (AddMissing, with a_note). The player's
	// own bytes are never rewritten: what is added is appended to the file, so a write that fails half way can't
	// cost a setting, and a file that lacks nothing, or one that can't be read, is not opened for writing at all.
	[[nodiscard]] inline Loaded LoadOrCreate(const std::filesystem::path& a_path, std::string_view a_defaults, std::string_view a_note = {})
	{
		std::error_code error;
		const bool      there = std::filesystem::exists(a_path, error);
		if (error) {
			return { std::string{ a_defaults }, Kept::Unreadable, {} };
		}
		if (!there) {
			return { std::string{ a_defaults }, WriteText(a_path, a_defaults) ? Kept::Created : Kept::NotCreated, {} };
		}
		// Something else by that name (a folder) is not a file to read: some systems open one and fail on the read.
		if (!std::filesystem::is_regular_file(a_path, error) || error) {
			return { std::string{ a_defaults }, Kept::Unreadable, {} };
		}
		std::string text;
		try {
			std::ifstream in{ a_path, std::ios::binary };
			if (!in) {
				return { std::string{ a_defaults }, Kept::Unreadable, {} };
			}
			text.assign(std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{});
		} catch (const std::ios_base::failure&) {
			return { std::string{ a_defaults }, Kept::Unreadable, {} };
		}
		auto merged = AddMissing(text, a_defaults, a_note);
		if (merged.added.empty()) {
			return { std::move(merged.text), Kept::Complete, {} };
		}
		// AddMissing only appends, so the file's own bytes stay where they are.
		const std::string_view tail = std::string_view{ merged.text }.substr(text.size());
		bool                   written = false;
		if (std::ofstream out{ a_path, std::ios::binary | std::ios::app }) {
			written = static_cast<bool>(out.write(tail.data(), static_cast<std::streamsize>(tail.size())) && out.flush());
		}
		return { std::move(merged.text), written ? Kept::Extended : Kept::NotExtended, std::move(merged.added) };
	}
}
