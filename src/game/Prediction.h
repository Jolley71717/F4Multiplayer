#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

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

	// States reach the game when it reads the network, once a frame. At 16 frames a second two or three
	// arrive in the same frame, and stamped with that frame's time the blend between them moved the
	// stand-in in spurts: a friend in a slow game saw the host run unevenly and bounce. Each state is
	// stamped one usual gap after the last instead, eased toward when it really arrived; a silence, or a
	// state far off the beat, starts the timeline over.
	struct ArrivalClock
	{
		using Clock = std::chrono::steady_clock;
		static constexpr auto  RESYNC = std::chrono::milliseconds(250);
		static constexpr float EASE = 0.125f;    // of the distance to the real arrival, taken per state
		static constexpr float GAP_KEEP = 0.9f;  // smoothing of the usual gap

		float             gap = 0.033f;  // seconds between the sender's states (a guess until measured)
		Clock::time_point stamp{};
		Clock::time_point arrival{};
		bool              started = false;

		// The time to stamp a state that arrived at a_arrival with; a_steps is how many states the sender
		// made since the last one we got (more than one when some were lost), 1 when unknown.
		Clock::time_point Next(Clock::time_point a_arrival, std::uint32_t a_steps = 1)
		{
			const float steps = static_cast<float>((std::max)(a_steps, 1u));
			if (!started) {
				started = true;
				stamp = arrival = a_arrival;
				return stamp;
			}
			const float since = std::chrono::duration<float>(a_arrival - arrival).count() / steps;
			arrival = a_arrival;
			if (since < std::chrono::duration<float>(RESYNC).count()) {
				gap = gap * GAP_KEEP + since * (1.0f - GAP_KEEP);
			}
			const auto predicted = stamp + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(gap * steps));
			const auto error = a_arrival - predicted;
			if (error > RESYNC || error < -RESYNC) {
				stamp = a_arrival;
				return stamp;
			}
			const auto eased = predicted + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(error) * EASE);
			stamp = (std::max)(eased, stamp + std::chrono::milliseconds(1));
			return stamp;
		}
	};
}
