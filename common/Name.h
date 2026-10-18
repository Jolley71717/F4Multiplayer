#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// A player's name as the session shows it. Names come from the game (the character's name, typed in the opening in
// any script the player's keyboard has), from Steam (a persona in any script) or from the ini, and go to the HUD, the
// stand-in's name tag and the player list as UTF-8, which is what the game's Scaleform text takes. The server keeps
// every letter and digit of every script (Cyrillic, CJK, an emoji) and drops what can't be shown or could do harm
// there: bytes that aren't UTF-8, control characters, line and paragraph breaks, the invisible characters that
// reorder or hide text (bidi controls, zero-width joiners, the byte order mark), and markup (< and >: the HUD's text
// fields read HTML). A name is at most MAX_CHARACTERS characters, counted in characters (code points), not bytes.
// Free of the game and of the server, so the tests run all of it; the client cleans the names it hears the same way.
namespace Name
{
	// Characters a name may have. The wire holds a name as bytes (Protocol::MAX_NAME_LENGTH): four for the widest
	// UTF-8 character, so every name of this many characters fits.
	inline constexpr std::size_t MAX_CHARACTERS = 32;
	static_assert(Protocol::MAX_NAME_LENGTH >= MAX_CHARACTERS * 4, "every name of MAX_CHARACTERS must fit the wire");

	// One character of UTF-8 text: its code point and how many bytes it took.
	struct Character
	{
		char32_t    point = 0;
		std::size_t length = 0;
	};

	// The character that starts at a_at in a_text, or none when the bytes there aren't well-formed UTF-8: a
	// continuation byte on its own, a sequence cut short, an overlong encoding (a 2-byte 'A'), a surrogate (D800-DFFF)
	// or a code point past U+10FFFF.
	[[nodiscard]] inline std::optional<Character> Decode(std::string_view a_text, std::size_t a_at)
	{
		if (a_at >= a_text.size()) {
			return std::nullopt;
		}
		const auto byte = [&](std::size_t a_i) { return static_cast<unsigned char>(a_text[a_i]); };
		const auto first = byte(a_at);
		if (first < 0x80) {
			return Character{ first, 1 };
		}
		std::size_t length = 0;
		char32_t    point = 0;
		char32_t    least = 0;  // the smallest code point this length may encode (anything smaller is overlong)
		if ((first & 0xE0) == 0xC0) {
			length = 2, point = first & 0x1F, least = 0x80;
		} else if ((first & 0xF0) == 0xE0) {
			length = 3, point = first & 0x0F, least = 0x800;
		} else if ((first & 0xF8) == 0xF0) {
			length = 4, point = first & 0x07, least = 0x10000;
		} else {
			return std::nullopt;  // a continuation byte, or a lead byte of a length UTF-8 no longer has
		}
		if (a_at + length > a_text.size()) {
			return std::nullopt;
		}
		for (std::size_t i = 1; i < length; ++i) {
			const auto next = byte(a_at + i);
			if ((next & 0xC0) != 0x80) {
				return std::nullopt;
			}
			point = (point << 6) | (next & 0x3F);
		}
		if (point < least || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
			return std::nullopt;
		}
		return Character{ point, length };
	}

	// Whether every byte of a_text belongs to a well-formed UTF-8 character.
	[[nodiscard]] inline bool ValidUtf8(std::string_view a_text)
	{
		for (std::size_t at = 0; at < a_text.size();) {
			const auto character = Decode(a_text, at);
			if (!character) {
				return false;
			}
			at += character->length;
		}
		return true;
	}

	// How many characters (code points) a_text holds. A byte that isn't UTF-8 counts as one.
	[[nodiscard]] inline std::size_t Characters(std::string_view a_text)
	{
		std::size_t count = 0;
		for (std::size_t at = 0; at < a_text.size(); ++count) {
			const auto character = Decode(a_text, at);
			at += character ? character->length : 1;
		}
		return count;
	}

	// Whether a character may stand in a shown name.
	[[nodiscard]] constexpr bool Allowed(char32_t a_point)
	{
		if (a_point < 0x20 || a_point == 0x7F || (a_point >= 0x80 && a_point < 0xA0)) {
			return false;  // C0 and C1 control characters, DEL
		}
		if (a_point == U'<' || a_point == U'>') {
			return false;  // markup: the HUD's text fields read HTML
		}
		if (a_point == 0x2028 || a_point == 0x2029) {
			return false;  // line and paragraph separators
		}
		if (a_point >= 0x200B && a_point <= 0x200F) {
			return false;  // zero-width space, joiners and the left-to-right/right-to-left marks
		}
		if (a_point >= 0x202A && a_point <= 0x202E) {
			return false;  // bidi embedding and override controls: they can show a name's letters in another order
		}
		if (a_point >= 0x2066 && a_point <= 0x2069) {
			return false;  // bidi isolates
		}
		if (a_point == 0xFEFF || a_point == 0xFFFE || a_point == 0xFFFF) {
			return false;  // the byte order mark and the two noncharacters
		}
		return true;
	}

	// a_name as it may be shown: its allowed characters (Allowed), at most a_max of them, with the spaces at either
	// end dropped. What isn't UTF-8 is dropped byte by byte. "" when nothing is left.
	[[nodiscard]] inline std::string Sanitize(std::string_view a_name, std::size_t a_max = MAX_CHARACTERS)
	{
		std::string out;
		out.reserve(a_name.size());
		std::size_t kept = 0;
		for (std::size_t at = 0; at < a_name.size() && kept < a_max;) {
			const auto character = Decode(a_name, at);
			if (!character) {
				++at;
				continue;
			}
			if (Allowed(character->point)) {
				out.append(a_name.substr(at, character->length));
				++kept;
			}
			at += character->length;
		}
		const auto first = out.find_first_not_of(' ');
		if (first == std::string::npos) {
			return {};
		}
		return out.substr(first, out.find_last_not_of(' ') - first + 1);
	}

	// The name a player is shown under: Sanitize, or "Player N" for a name nothing is left of.
	[[nodiscard]] inline std::string Shown(std::string_view a_name, std::uint32_t a_id)
	{
		auto name = Sanitize(a_name);
		if (name.empty()) {
			name = std::format("Player {}", a_id);
		}
		return name;
	}
}
