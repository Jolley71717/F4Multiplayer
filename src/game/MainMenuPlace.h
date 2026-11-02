#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

// Where the Multiplayer entry goes in the main menu's list (and the pause menu's, the same movie in PauseMode). The
// list's entries are AS3 objects with a text (a translation key such as "$LOAD") and an index the menu acts on; ours
// carries an index no entry of the game's has. Kept free of the game so it can be tested; MainMenuEntry reads the list.
namespace MainMenuPlace
{
	// Far above the game's own indexes (0 to about 15), so the menu's press handler has no case for it.
	inline constexpr int ENTRY_INDEX = 900;

	// The entry is put after Load; a list without Load (a mod that rebuilt the menu) gets none.
	inline constexpr std::string_view AFTER = "$LOAD";

	struct Entry
	{
		std::string_view text;
		int              index = -1;
	};

	constexpr bool SameKey(std::string_view a_lhs, std::string_view a_rhs)
	{
		constexpr auto lower = [](char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; };
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (lower(a_lhs[i]) != lower(a_rhs[i])) {
				return false;
			}
		}
		return true;
	}

	// Where to insert ours, or nothing: it is already there (the menu keeps it until it rebuilds the list), or the
	// list has no Load to put it after.
	[[nodiscard]] constexpr std::optional<std::size_t> InsertAt(std::span<const Entry> a_entries)
	{
		std::optional<std::size_t> at;
		for (std::size_t i = 0; i < a_entries.size(); ++i) {
			if (a_entries[i].index == ENTRY_INDEX) {
				return std::nullopt;
			}
			if (!at && SameKey(a_entries[i].text, AFTER)) {
				at = i + 1;
			}
		}
		return at;
	}
}
