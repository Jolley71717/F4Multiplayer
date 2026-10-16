#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// Where the game keeps the recording of one line of dialogue, so a friend's line can be played as a
// plain sound on our copy of the speaker without going through the dialogue system:
//   Sound\Voice\<plugin>\<voice type's editor ID>\<INFO id, 8 hex digits>_<response number>.fuz
// relative to Data (loose files and the Voices archives are both found there). The INFO id is the
// form id as the plugin itself numbers it: the load-order byte becomes 00 (Fallout4.esm's 0002A2B3
// stays 0002A2B3, DLCCoast.esm's 0401C9F4 is 0001C9F4), and a light plugin's id keeps its last three
// hex digits. No game types: tests/VoicePathTests.cpp covers it.
namespace VoicePath
{
	// The INFO's id as its own plugin numbers it.
	constexpr std::uint32_t LocalId(std::uint32_t a_formId, bool a_lightPlugin)
	{
		return a_formId & (a_lightPlugin ? 0x00000FFFu : 0x00FFFFFFu);
	}

	// A plugin's file name or a voice type's editor ID is one folder of the path: never empty, never
	// a way out of it (no separators, no "..", no drive), and nothing a file name can't hold.
	inline bool SafeName(std::string_view a_name)
	{
		if (a_name.empty() || a_name.size() > 128 || a_name == "." || a_name == ".." || a_name.find("..") != std::string_view::npos) {
			return false;
		}
		return std::ranges::none_of(a_name, [](char a_c) {
			const auto c = static_cast<unsigned char>(a_c);
			return c < 0x20 || c == 0x7F || a_c == '\\' || a_c == '/' || a_c == ':' || a_c == '*' || a_c == '?' || a_c == '"' || a_c == '<' ||
			       a_c == '>' || a_c == '|';
		});
	}

	// The voice file of one response (1 and up) of a line, or "" when a part can't be a path.
	inline std::string Build(std::string_view a_plugin, std::string_view a_voiceType, std::uint32_t a_infoLocalId, std::uint8_t a_response,
		std::string_view a_extension = "fuz")
	{
		if (!SafeName(a_plugin) || !SafeName(a_voiceType) || a_response == 0 || a_infoLocalId == 0 || !SafeName(a_extension)) {
			return {};
		}
		return std::format("Sound\\Voice\\{}\\{}\\{:08X}_{}.{}", a_plugin, a_voiceType, a_infoLocalId, a_response, a_extension);
	}
}
