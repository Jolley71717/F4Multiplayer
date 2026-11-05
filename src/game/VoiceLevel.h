#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>

// How loud the player's microphone is, for the push-to-talk HUD: from the voice pieces as Steam gives them, what the
// icon's microphone and brackets look like. No game types, so it can be tested.
namespace VoiceLevel
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// To be set from real values: the log has each press's level.
	constexpr float SPEECH_DB = -45.0f;
	constexpr float LOUD_DB = -25.0f;
	constexpr float SILENT_DB = -120.0f;
	constexpr auto  FADE = 150ms;            // a bracket lights at once and fades over this long, so it doesn't flicker
	constexpr float FADED = 0.15f;

	// Decibels below full scale, by root mean square.
	inline float Dbfs(std::span<const std::int16_t> a_samples)
	{
		if (a_samples.empty()) {
			return SILENT_DB;
		}
		double sum = 0.0;
		for (const auto sample : a_samples) {
			sum += static_cast<double>(sample) * sample;
		}
		const double rms = std::sqrt(sum / static_cast<double>(a_samples.size())) / 32768.0;
		return rms > 0.0 ? (std::max)(SILENT_DB, static_cast<float>(20.0 * std::log10(rms))) : SILENT_DB;
	}

	struct Look
	{
		float inner = FADED;
		float outer = FADED;
		bool  operator==(const Look&) const = default;
	};

	class Meter
	{
	public:
		void Start(Clock::time_point a_now)
		{
			*this = Meter{};
			lastAudio = a_now;
		}

		void Piece(float a_db, Clock::time_point a_now)
		{
			lastAudio = a_now;
			lastDb = a_db;
			++pieces;
			sum += a_db;
			loudest = (std::max)(loudest, a_db);
			if (a_db >= SPEECH_DB) {
				lastSpeech = a_now;
			}
			if (a_db >= LOUD_DB) {
				lastLoud = a_now;
			}
		}

		[[nodiscard]] Look At(Clock::time_point a_now) const
		{
			return { .inner = Faded(lastSpeech, a_now), .outer = Faded(lastLoud, a_now) };
		}

		// The latest piece's level; silent once they have stopped coming for a moment.
		[[nodiscard]] float LevelAt(Clock::time_point a_now) const
		{
			return lastAudio && a_now - *lastAudio < FADE * 2 ? lastDb : SILENT_DB;
		}

		// "pieces=N mean=-30 loudest=-12 dB"
		[[nodiscard]] std::string Describe() const
		{
			return std::format("pieces={} mean={:.0f} loudest={:.0f} dB", pieces, pieces ? sum / static_cast<float>(pieces) : SILENT_DB, loudest);
		}

	private:
		static float Faded(const std::optional<Clock::time_point>& a_when, Clock::time_point a_now)
		{
			if (!a_when) {
				return FADED;
			}
			const float left = 1.0f - std::chrono::duration<float>(a_now - *a_when) / std::chrono::duration<float>(FADE);
			return FADED + (1.0f - FADED) * std::clamp(left, 0.0f, 1.0f);
		}

		std::optional<Clock::time_point> lastAudio;
		std::optional<Clock::time_point> lastSpeech;
		std::optional<Clock::time_point> lastLoud;
		std::uint32_t                    pieces = 0;
		float                            sum = 0.0f;
		float                            lastDb = SILENT_DB;
		float                            loudest = SILENT_DB;
	};
}
