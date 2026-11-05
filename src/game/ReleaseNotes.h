#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Which of the release notes the About page's changelog shows (dist/RELEASE-<version>.md). Kept free of the game so it can be tested.
namespace ReleaseNotes
{
	using Version = std::array<int, 3>;

	struct Note
	{
		Version version{};
		int     candidate = 0;  // the N of -rcN; 0 for the release itself
	};

	// "RELEASE-0.11.2.md" or "RELEASE-0.11.2-rc1.md"; anything else, nothing.
	[[nodiscard]] inline std::optional<Note> Parse(std::string_view a_file)
	{
		constexpr std::string_view PREFIX = "RELEASE-", SUFFIX = ".md", RC = "-rc";
		if (!a_file.starts_with(PREFIX) || !a_file.ends_with(SUFFIX)) {
			return std::nullopt;
		}
		a_file.remove_prefix(PREFIX.size());
		a_file.remove_suffix(SUFFIX.size());
		Note note;
		const auto number = [&](int& a_out) {
			const auto [end, error] = std::from_chars(a_file.data(), a_file.data() + a_file.size(), a_out);
			if (error != std::errc{} || end == a_file.data()) {
				return false;
			}
			a_file.remove_prefix(static_cast<std::size_t>(end - a_file.data()));
			return true;
		};
		for (std::size_t part = 0; part < note.version.size(); ++part) {
			if (part > 0) {
				if (!a_file.starts_with('.')) {
					return std::nullopt;
				}
				a_file.remove_prefix(1);
			}
			if (!number(note.version[part])) {
				return std::nullopt;
			}
		}
		if (a_file.starts_with(RC)) {
			a_file.remove_prefix(RC.size());
			if (!number(note.candidate) || note.candidate <= 0) {
				return std::nullopt;
			}
		}
		if (!a_file.empty()) {
			return std::nullopt;
		}
		return note;
	}

	// The files to show, newest version first, at most a_most: for each version up to a_current its release's notes, or its latest
	// candidate's before there is a release (an rc's package holds its own notes, and the release is that same package).
	[[nodiscard]] inline std::vector<std::string> Pick(const std::vector<std::string>& a_files, const Version& a_current, std::size_t a_most)
	{
		struct Chosen
		{
			int         candidate = 0;
			std::string file;
		};
		std::map<Version, Chosen, std::greater<>> versions;
		for (const auto& file : a_files) {
			const auto note = Parse(file);
			if (!note || note->version > a_current) {
				continue;
			}
			const auto [it, added] = versions.try_emplace(note->version, Chosen{ note->candidate, file });
			if (!added && it->second.candidate != 0 && (note->candidate == 0 || note->candidate > it->second.candidate)) {
				it->second = Chosen{ note->candidate, file };
			}
		}
		std::vector<std::string> out;
		for (const auto& [version, chosen] : versions) {
			if (out.size() == a_most) {
				break;
			}
			out.push_back(chosen.file);
		}
		return out;
	}
}
