#pragma once

#include "Protocol.h"
#include "game/WorkshopCells.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>

// How the copy of a piece another player moved follows it. The engine keeps a reference in the cell it
// was made in whatever its position says, so a copy can only be moved within its cell; one whose piece
// went to another cell is made again there. A report that moves nothing (the session's building sent
// again after a load) moves nothing: moving a copy marks it changed in the save. Kept free of the game
// so it can be tested.
namespace WorkshopMove
{
	struct Transform
	{
		float position[3]{};
		float rotation[3]{};  // radians
		float scale = 1.0f;
	};

	// Closer than these is the same place.
	inline constexpr float SAME_POSITION = 0.5f;  // game units
	inline constexpr float SAME_ANGLE = 0.01f;    // radians
	inline constexpr float SAME_SCALE = 0.01f;

	inline Transform Of(const Protocol::WorkshopItem& a_item)
	{
		return { { a_item.position[0], a_item.position[1], a_item.position[2] }, { a_item.rotation[0], a_item.rotation[1], a_item.rotation[2] }, a_item.scale };
	}

	// The angle between two, the short way round (one just past a full turn is next to one just short of it).
	inline float AngleBetween(float a_from, float a_to)
	{
		constexpr float TURN = 2.0f * std::numbers::pi_v<float>;
		const float     d = std::fmod(std::abs(a_to - a_from), TURN);
		return d > std::numbers::pi_v<float> ? TURN - d : d;
	}

	inline bool Same(const Transform& a_a, const Transform& a_b)
	{
		const float dx = a_a.position[0] - a_b.position[0];
		const float dy = a_a.position[1] - a_b.position[1];
		const float dz = a_a.position[2] - a_b.position[2];
		if (dx * dx + dy * dy + dz * dz > SAME_POSITION * SAME_POSITION) {
			return false;
		}
		for (int i = 0; i < 3; ++i) {
			if (AngleBetween(a_a.rotation[i], a_b.rotation[i]) > SAME_ANGLE) {
				return false;
			}
		}
		return std::abs(a_a.scale - a_b.scale) <= SAME_SCALE;
	}

	enum class How
	{
		kStay,      // already there
		kInPlace,   // moved within its cell
		kRecreate,  // made again where it went
	};

	// a_cell: the exterior cell the copy stands in (grid x, y); none for an interior, which is all one
	// cell, or for the worldspace's persistent cell, which spans it.
	inline How HowToMove(const Transform& a_now, const Transform& a_to, std::optional<std::pair<std::int32_t, std::int32_t>> a_cell)
	{
		if (Same(a_now, a_to)) {
			return How::kStay;
		}
		if (a_cell && (WorkshopCells::Grid(a_to.position[0]) != a_cell->first || WorkshopCells::Grid(a_to.position[1]) != a_cell->second)) {
			return How::kRecreate;
		}
		return How::kInPlace;
	}
}
