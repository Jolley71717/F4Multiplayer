#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The game's side of group difficulty (common/GroupDifficulty.h, the host's iGroupDifficulty): the engine's multipliers
// of the damage the player deals to NPCs, one per difficulty (fDiffMultHPByPCVE to fDiffMultHPByPCTSV, game settings in
// memory, never in a save), each divided by the toughness while the session has that many players, and put back when
// the count or the rule changes and when the session ends. The originals are read once, the first time a change is made.
namespace GroupHits
{
	// Every frame from Party: the host's rule and how many players are here (our own included). Writes the settings
	// only when the multiplier changes.
	void Update(std::uint8_t a_percent, std::size_t a_players);

	// The session ended: the settings as the game had them.
	void Restore();

	// The engine's multiplier of our player's damage to NPCs in effect now, at our difficulty (divided while the rule
	// is on): what a hit's figure is multiplied by as it comes off (CopyHealth::FigureDealt). 1 when it can't be read.
	[[nodiscard]] float PlayerHitMult();

	// For net status: "group=x0.50 players=3 writes=2".
	[[nodiscard]] std::string Describe();
}
