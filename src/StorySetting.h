#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

// The host's sStory: the main story and the factions' quests "shared" (the default, also when the key is missing) or
// each player's "own". Read in any case: 0.9.0 compared the value with "shared" exactly, so a host who wrote
// "sStory = Shared" hosted with everyone on their own story, and a joiner's progress in Out of Time (Codsworth's
// search) never reached the host's game. A value that is neither leaves the default and is logged, rather than
// quietly making it the own story. Pure, so it is unit tested.
namespace StorySetting
{
	// true: shared, false: own, nullopt: neither (the setting is left as it was).
	inline std::optional<bool> Parse(std::string_view a_value)
	{
		const auto first = a_value.find_first_not_of(" \t\r\n\"'");
		if (first == std::string_view::npos) {
			return std::nullopt;
		}
		a_value = a_value.substr(first, a_value.find_last_not_of(" \t\r\n\"'") - first + 1);
		const auto is = [&](std::string_view a_word) {
			return std::ranges::equal(a_value, a_word, [](unsigned char a_left, unsigned char a_right) { return std::tolower(a_left) == a_right; });
		};
		if (is("shared")) {
			return true;
		}
		if (is("own")) {
			return false;
		}
		return std::nullopt;
	}

	// The value the in-game menu writes, and the log's word for it.
	constexpr std::string_view Name(bool a_shared)
	{
		return a_shared ? "shared" : "own";
	}
}
