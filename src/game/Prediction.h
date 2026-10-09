#pragma once

#include <algorithm>
#include <chrono>

// Pure helpers behind stand-in motion, kept free of the game so they can be tested.
namespace Prediction
{
	// When the next position update is late, the stand-in keeps going along its last known
	// velocity for this long, instead of stopping dead (which looks like stutter); after that it
	// holds still.
	constexpr auto MAX_AHEAD = std::chrono::milliseconds(150);

	// How far ahead of the last known position to project, in seconds, clamped to MAX_AHEAD. 0 when
	// the update is not late.
	constexpr float AheadSeconds(std::chrono::steady_clock::duration a_late)
	{
		const auto clamped = std::clamp(a_late, std::chrono::steady_clock::duration::zero(), std::chrono::steady_clock::duration(MAX_AHEAD));
		return std::chrono::duration<float>(clamped).count();
	}

	// The speed fed to the animation: what the stand-in actually covered, smoothed, so the feet
	// match the ground even when the reported speed lags (a_measured is this frame's
	// displacement rate; a_previous the last smoothed value).
	constexpr float SmoothSpeed(float a_previous, float a_measured)
	{
		constexpr float keep = 0.5f;
		return a_previous * keep + a_measured * (1.0f - keep);
	}

	// Distance over time in windows of a tenth of a second: a per-call rate would be wrong when a
	// frame sets the same target twice, or when two frames are a millisecond apart.
	struct SpeedWindow
	{
		float distance = 0.0f;
		float seconds = 0.0f;
		float speed = 0.0f;  // the smoothed result

		static constexpr float WINDOW = 0.1f;

		// Adds one step; true when the window closed and `speed` was updated.
		constexpr bool Add(float a_distance, float a_seconds)
		{
			distance += a_distance;
			seconds += a_seconds;
			if (seconds < WINDOW) {
				return false;
			}
			speed = SmoothSpeed(speed, distance / seconds);
			distance = seconds = 0.0f;
			return true;
		}

		constexpr void Reset(float a_speed)
		{
			distance = seconds = 0.0f;
			speed = a_speed;
		}
	};
}
