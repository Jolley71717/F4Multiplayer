#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// What the menu's Voice page steps through and shows. No game types, so it can be tested.
namespace VoiceSteps
{
	constexpr std::array<std::uint32_t, 6> VOLUMES{ 25, 50, 75, 100, 125, 150 };  // percent
	constexpr std::array<float, 4>         RANGES{ 30.0f, 60.0f, 120.0f, 0.0f };   // meters; 0: everywhere

	// The step after the one a_now is at, or the first above it if it's not a step (the ini can say anything).
	template <class T, std::size_t N>
	T Next(const std::array<T, N>& a_steps, T a_now)
	{
		const auto it = std::ranges::find(a_steps, a_now);
		if (it != a_steps.end()) {
			return std::next(it) != a_steps.end() ? *std::next(it) : a_steps.front();
		}
		const auto above = std::ranges::find_if(a_steps, [&](T a_step) { return a_step > a_now; });
		return above != a_steps.end() ? *above : a_steps.front();
	}

	// The device after a_now in a_names, with "" (the system's default) first and last. A name no longer there goes to the default.
	inline std::string NextOutput(std::span<const std::string> a_names, std::string_view a_now)
	{
		if (a_names.empty()) {
			return {};
		}
		if (a_now.empty()) {
			return a_names.front();
		}
		const auto it = std::ranges::find(a_names, a_now);
		return it != a_names.end() && std::next(it) != a_names.end() ? *std::next(it) : std::string{};
	}

	// A samples' gain, without wrapping round when it goes past full scale.
	inline std::int16_t Scaled(std::int16_t a_sample, float a_gain)
	{
		return static_cast<std::int16_t>(std::clamp(static_cast<float>(a_sample) * a_gain, -32768.0f, 32767.0f));
	}

	// A level as a bar of a_width: -60 dB and below empty, 0 dB full.
	inline std::string Bar(float a_db, std::size_t a_width = 24)
	{
		const auto filled = static_cast<std::size_t>(std::lround(std::clamp((a_db + 60.0f) / 60.0f, 0.0f, 1.0f) * static_cast<float>(a_width)));
		return std::string(filled, '|');
	}

	// UTF-8 from the UTF-16 of a device's name.
	inline std::string Utf8(std::u16string_view a_text)
	{
		std::string out;
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			std::uint32_t c = a_text[i];
			if (c >= 0xD800 && c < 0xDC00 && i + 1 < a_text.size() && a_text[i + 1] >= 0xDC00 && a_text[i + 1] < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) + (a_text[++i] - 0xDC00);
			}
			if (c < 0x80) {
				out.push_back(static_cast<char>(c));
			} else if (c < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (c >> 6)));
				out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			} else if (c < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (c >> 12)));
				out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (c >> 18)));
				out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			}
		}
		return out;
	}
}
