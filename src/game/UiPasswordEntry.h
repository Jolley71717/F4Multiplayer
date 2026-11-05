#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// What a password typed in the multiplayer menu may hold. Kept free of the game so it can be tested.
namespace UiPasswordEntry
{
	// The keyboard's printable ASCII, so friends on any layout can type it and it reads back from the ini as written. Not a space (the
	// ini trims them at the ends, and they can't be seen there), nor < or > (the menu's text drops those, so it wouldn't show as typed).
	[[nodiscard]] constexpr bool Allowed(std::uint32_t a_character)
	{
		return a_character > 0x20 && a_character < 0x7F && a_character != '<' && a_character != '>';
	}

	// True if a_character was added: one that's allowed, and room for it.
	inline bool Add(std::string& a_typed, std::uint32_t a_character, std::size_t a_max)
	{
		if (!Allowed(a_character) || a_typed.size() >= a_max) {
			return false;
		}
		a_typed.push_back(static_cast<char>(a_character));
		return true;
	}
}
