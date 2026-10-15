#pragma once

#include "game/StandInHold.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

// Where a puppet is put when the engine updates it. Our frame's work (RemotePlayers, NpcSync) runs when the game's
// window procedure takes our message, which is not the same moment of each frame as the engine's update of the actors:
// some frames before it, some after, and now and then not in a frame at all. A stand-in put where our frame's work
// last sampled its friend was drawn at an uneven moment of their path each frame: still for one frame, two steps the
// next, and a friend saw the host run choppily and bounce. So our work hands over the path ahead (where the friend is
// at the moment it samples, then the states after it, each at the moment it is to be drawn), and the actor's own
// update puts it where that path is at the moment the update runs. Kept free of the game so it can be tested.
namespace MotionPath
{
	using Clock = std::chrono::steady_clock;

	struct Point
	{
		Clock::time_point at;  // when it is to be drawn there, on our clock
		float             x = 0.0f;
		float             y = 0.0f;
		float             z = 0.0f;
		float             heading = 0.0f;  // radians
	};

	// Enough to reach past the newest state at the longest delay (StateBuffer::MAX_DELAY, 250 ms) with states 33 ms apart.
	inline constexpr std::size_t POINTS = 12;

	struct Path
	{
		std::array<Point, POINTS> points{};
		std::uint8_t              count = 0;

		// Adds the next point; points must come in time order (one no later than the last is left out). False when
		// it is full.
		constexpr bool Add(const Point& a_point)
		{
			if (count == POINTS) {
				return false;
			}
			if (count == 0 || a_point.at > points[count - 1].at) {
				points[count++] = a_point;
			}
			return true;
		}

		[[nodiscard]] constexpr bool Empty() const { return count == 0; }

		// Where it is at a_now: between the two points around it, at the first before the path starts (an update
		// that ran a moment before our work sampled it), and at the last once the path has run out (no newer state
		// came: it stands there, as a held stand-in does). None for an empty path.
		[[nodiscard]] std::optional<Point> At(Clock::time_point a_now) const
		{
			if (count == 0) {
				return std::nullopt;
			}
			if (a_now <= points[0].at) {
				return points[0];
			}
			for (std::size_t i = 1; i < count; ++i) {
				const auto& a = points[i - 1];
				const auto& b = points[i];
				if (a_now > b.at) {
					continue;
				}
				const float t = std::chrono::duration<float>(a_now - a.at).count() / std::chrono::duration<float>(b.at - a.at).count();
				Point out = b;
				out.at = a_now;
				out.x = std::lerp(a.x, b.x, t);
				out.y = std::lerp(a.y, b.y, t);
				out.z = std::lerp(a.z, b.z, t);
				out.heading = a.heading + StandInHold::TurnBetween(a.heading, b.heading) * t;
				return out;
			}
			return points[count - 1];
		}
	};

	// The path from a_from (where it is drawn at a_from.at, our work's moment) on through a_snapshots from a_first: each
	// snapshot is drawn a_delay after its stamp (a_stamp, its member). One no later than a_from is left out, and one in
	// another space than a_cell and a_worldspace ends it (a door or a fast travel is not blended across).
	template <class Snapshots, class Stamp>
	Path Ahead(const Point& a_from, std::uint32_t a_cell, std::uint32_t a_worldspace, const Snapshots& a_snapshots, std::size_t a_first, Clock::duration a_delay, Stamp a_stamp)
	{
		Path path;
		path.Add(a_from);
		for (std::size_t i = a_first; i < a_snapshots.size(); ++i) {
			const auto& snapshot = a_snapshots[i];
			const auto& state = snapshot.state;
			if (state.cell != a_cell || state.worldspace != a_worldspace || !path.Add({ snapshot.*a_stamp + a_delay, state.x, state.y, state.z, state.heading })) {
				break;
			}
		}
		return path;
	}
}
