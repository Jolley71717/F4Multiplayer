// Group difficulty (common/GroupDifficulty.h): how much tougher enemies are with more players in the session.

#include "Test.h"

#include "GroupDifficulty.h"

namespace
{
	bool Near(float a_value, float a_expected)
	{
		return a_value > a_expected - 0.001f && a_value < a_expected + 0.001f;
	}
}

TEST("group difficulty: off, or alone, enemies are as the game makes them")
{
	CHECK(Near(GroupDifficulty::Toughness(0, 4), 1.0f));
	CHECK(Near(GroupDifficulty::Toughness(50, 1), 1.0f));
	CHECK(Near(GroupDifficulty::Toughness(50, 0), 1.0f));
	CHECK(Near(GroupDifficulty::DamageMult(0, 4), 1.0f));
	CHECK(Near(GroupDifficulty::DamageMult(50, 1), 1.0f));
}

TEST("group difficulty: each player beyond the first adds the percent, and the player's damage is the inverse")
{
	CHECK(Near(GroupDifficulty::Toughness(50, 2), 1.5f));
	CHECK(Near(GroupDifficulty::Toughness(50, 3), 2.0f));
	CHECK(Near(GroupDifficulty::Toughness(50, 4), 2.5f));
	CHECK(Near(GroupDifficulty::Toughness(100, 2), 2.0f));
	CHECK(Near(GroupDifficulty::Toughness(25, 5), 2.0f));
	CHECK(Near(GroupDifficulty::DamageMult(50, 3), 0.5f));
	CHECK(Near(GroupDifficulty::DamageMult(100, 2), 0.5f));
	CHECK(Near(GroupDifficulty::DamageMult(25, 5), 0.5f));
}

TEST("group difficulty: the toughness never passes its cap, however many players or whatever percent")
{
	CHECK(Near(GroupDifficulty::Toughness(200, 5), 8.0f));  // 1 + 2 * 4 = 9, capped
	CHECK(Near(GroupDifficulty::Toughness(100, 16), 8.0f));
	CHECK(Near(GroupDifficulty::Toughness(255, 2), 3.0f));  // the percent is held to 200
	CHECK(Near(GroupDifficulty::Toughness(10, 1000), 7.3f));  // the count is held to 64
	CHECK(GroupDifficulty::DamageMult(200, 64) >= 1.0f / GroupDifficulty::MAX_TOUGHNESS - 0.001f);
	for (std::size_t players = 0; players <= 70; ++players) {
		const auto mult = GroupDifficulty::DamageMult(200, players);
		CHECK(mult > 0.0f && mult <= 1.0f);
	}
}
