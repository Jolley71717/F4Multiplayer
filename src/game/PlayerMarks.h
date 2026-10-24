#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Each player's mark: a color and a shape, by player ID, the same on the compass (Compass), on the Pip-Boy's maps
// (FriendMap) and in the player list ("pink diamond"). Free of the game.
namespace PlayerMarks
{
	enum class Shape
	{
		kCircle,
		kSquare,
		kDiamond,
		kTriangle,
	};

	struct Mark
	{
		std::uint32_t    color;  // 0xRRGGBB
		Shape            shape;
		std::string_view name;
	};

	inline constexpr std::array<Mark, 4> MARKS{ {
		{ 0x4FC3F7, Shape::kCircle, "blue circle" },
		{ 0xFFB74D, Shape::kSquare, "orange square" },
		{ 0xF06292, Shape::kDiamond, "pink diamond" },
		{ 0xB39DDB, Shape::kTriangle, "purple triangle" },
	} };

	constexpr const Mark& Of(std::uint32_t a_playerId)
	{
		return MARKS[a_playerId % MARKS.size()];
	}
}
