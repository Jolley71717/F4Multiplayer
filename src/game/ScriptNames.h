#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

// Papyrus script names as the game's VM knows them. The VM keeps the case a name was written with (a reference's own
// copy of a script can be named in lower case in the game's files), while the game's own lookups ignore case; so does
// this. Kept free of the game so it can be tested.
namespace ScriptNames
{
	constexpr bool Same(std::string_view a_left, std::string_view a_right)
	{
		if (a_left.size() != a_right.size()) {
			return false;
		}
		const auto lower = [](char a_c) { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c; };
		for (std::size_t i = 0; i < a_left.size(); ++i) {
			if (lower(a_left[i]) != lower(a_right[i])) {
				return false;
			}
		}
		return true;
	}

	// The list's own spelling of a_name, if a_name is on the list (a_list: any range of string_views or of structs with
	// a `script` member).
	template <class List>
	constexpr std::optional<std::string_view> Find(const List& a_list, std::string_view a_name)
	{
		for (const auto& entry : a_list) {
			if constexpr (requires { entry.script; }) {
				if (Same(entry.script, a_name)) {
					return entry.script;
				}
			} else {
				if (Same(entry, a_name)) {
					return std::string_view{ entry };
				}
			}
		}
		return std::nullopt;
	}
}
