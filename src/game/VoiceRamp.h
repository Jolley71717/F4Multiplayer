#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

// A friend's voice starting from silence (Voice.cpp): the device had run out of what to play, either because they
// began to talk or because their next piece came late (an underrun mid-word). The piece's first sample was played
// straight after silence, a step the speaker turns into a click; with a piece every 20-40 ms and a late one now and
// then, a friend talking (or an open mic) clicked over and over. The first RAMP of such a piece now fades in. Kept free
// of the game so it can be tested.
namespace VoiceRamp
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr std::size_t RAMP_SAMPLES = 96;  // 4 ms at 24 kHz: no click, too short to hear as a fade
	// A piece coming to an empty queue this soon after the last one is an underrun (the device starved mid-speech), not
	// the start of a new sentence.
	inline constexpr auto UNDERRUN_GAP = 1s;

	// Fades the first samples in, from silence to their own level.
	inline void FadeIn(std::span<std::int16_t> a_samples, std::size_t a_ramp = RAMP_SAMPLES)
	{
		const std::size_t n = a_samples.size() < a_ramp ? a_samples.size() : a_ramp;
		for (std::size_t i = 0; i < n; ++i) {
			a_samples[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(a_samples[i]) * static_cast<std::int32_t>(i) / static_cast<std::int32_t>(n));
		}
	}

	// Whether a piece arriving to an empty queue, a_sinceLast after the speaker's previous one, is an underrun.
	constexpr bool Underrun(bool a_queueEmpty, bool a_heardBefore, Clock::duration a_sinceLast)
	{
		return a_queueEmpty && a_heardBefore && a_sinceLast < UNDERRUN_GAP;
	}
}
