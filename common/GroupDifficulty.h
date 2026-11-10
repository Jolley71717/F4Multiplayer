#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Group difficulty (the host's iGroupDifficulty, SessionRules::groupDifficulty): enemies are tougher the more players
// are in the session, so three or four friends don't melt every fight. The rule is one number, the percent more
// toughness for each player beyond the first: with 50, two players meet enemies half as tough again, four players
// enemies two and a half times as tough. Every game applies it to its own player's hits: the engine's difficulty
// multipliers of the damage the player deals to NPCs (fDiffMultHPByPC*) are divided by the toughness while that many
// players are here (src/game/GroupHits.cpp), so a hit's number, computed in the shooter's game before it is sent, is
// the same tougher enemy for everyone. Enemies' damage to players is untouched, and so is friendly fire: a player's
// hit on a stand-in reaches the hit event before those multipliers (the rig, 2026-11-10), and is sent as it is. Kept
// free of the game so it can be tested (tests/GroupDifficultyTests.cpp).
namespace GroupDifficulty
{
	// The most percent per extra player the host may ask for, and the most an enemy can be toughened however many
	// players there are.
	inline constexpr std::uint8_t MAX_PERCENT = 200;
	inline constexpr float        MAX_TOUGHNESS = 8.0f;

	// How many times tougher an enemy is with a_players in the session: 1 alone, or with the rule off.
	[[nodiscard]] inline float Toughness(std::uint8_t a_percent, std::size_t a_players)
	{
		if (a_percent == 0 || a_players <= 1) {
			return 1.0f;
		}
		const auto extra = static_cast<float>((std::min<std::size_t>)(a_players, 64) - 1);
		return (std::min)(1.0f + static_cast<float>((std::min)(a_percent, MAX_PERCENT)) * extra / 100.0f, MAX_TOUGHNESS);
	}

	// What the player's damage to NPCs is multiplied by: the inverse of the toughness, 1 when nothing changes.
	[[nodiscard]] inline float DamageMult(std::uint8_t a_percent, std::size_t a_players)
	{
		return 1.0f / Toughness(a_percent, a_players);
	}
}
