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

	// What a hit on a friend's stand-in is multiplied back by before it is sent (GroupDifficulty::FriendlyFireMult
	// for the rule and count in force): 1 when nothing is changed.
	[[nodiscard]] float FriendlyFireMult();

	// For net status: "group=x0.50 players=3 writes=2".
	[[nodiscard]] std::string Describe();
}
