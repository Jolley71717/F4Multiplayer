#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

// The name other players see for us: sPlayerName in the ini, the player character's name in the game, or the Steam
// name. The choice is kept free of the game so it can be tested; Session reads the three values and sends the result
// in the Hello.
namespace NameChoice
{
	// sPlayerName's value for "the name of the character I play".
	inline constexpr std::string_view CHARACTER = "character";

	// Shown when the ini gives no name, there is no usable character name and Steam has none.
	inline constexpr std::string_view FALLBACK = "Vault Dweller";

	namespace detail
	{
		inline std::string_view Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			return a_str.substr(first, a_str.find_last_not_of(" \t\r\n") - first + 1);
		}

		inline bool EqualsNoCase(std::string_view a_a, std::string_view a_b)
		{
			return std::ranges::equal(a_a, a_b, [](char a_x, char a_y) {
				return std::tolower(static_cast<unsigned char>(a_x)) == std::tolower(static_cast<unsigned char>(a_y));
			});
		}

		// A name the server keeps something of: it drops everything but printable ASCII (Server.cpp SanitizeName), and
		// one left with nothing becomes "Player 3". So a character name of letters from another alphabet is no better
		// than none when the Steam name is there to use, and a blank one is none.
		inline bool Usable(std::string_view a_name)
		{
			return std::ranges::any_of(a_name, [](char a_c) { return a_c > 0x20 && a_c < 0x7F; });
		}
	}

	// Whether the ini asks for the character's name.
	[[nodiscard]] inline bool WantsCharacter(std::string_view a_ini)
	{
		return detail::EqualsNoCase(detail::Trim(a_ini), CHARACTER);
	}

	// The name to send. a_ini is sPlayerName; a_character is the player character's display name ("" when there is no
	// player yet or it has no name); a_steam is the Steam persona name ("" without Steam).
	//   "character"        the character's name, else the Steam name, else "Vault Dweller"
	//   a name             that name, as written
	//   empty (old inis)   the Steam name, else "Vault Dweller"
	[[nodiscard]] inline std::string Choose(std::string_view a_ini, std::string_view a_character, std::string_view a_steam)
	{
		const auto ini = detail::Trim(a_ini);
		if (!ini.empty() && !WantsCharacter(ini)) {
			return std::string{ ini };
		}
		const auto character = detail::Trim(a_character);
		if (WantsCharacter(ini) && detail::Usable(character)) {
			return std::string{ character };
		}
		const auto steam = detail::Trim(a_steam);
		return std::string{ steam.empty() ? FALLBACK : steam };
	}
}
