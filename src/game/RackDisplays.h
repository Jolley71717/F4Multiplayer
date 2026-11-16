#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Weapons on weapon racks, shown to friends (the rules: src/game/RackShare.h). Our game tells the others which weapons its
// own racks show (kReportRackWeapons), and shows theirs (kRackWeapons) as copies placed where they are: disabled until
// their activation is blocked, then held still with nothing to bump into, kept out of workshop mode's reach, and never
// saved (they go before every save and load and come again while our player is near). Main thread only.
namespace RackDisplays
{
	// Every frame while welcomed and in the world.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// From the server: every weapon a friend's racks show (their whole list, in place of the last).
	void OnList(const Protocol::RackWeapons& a_list);

	// A friend joined: our list again, for them. One left: their weapons go.
	void OnPlayerJoined();
	void OnPlayerLeft(std::uint32_t a_playerId);

	// Before every save and load: no copy is saved.
	void DiscardCopies();

	// A game was loaded: our racks are read anew (the list we keep of places out of sight was another world's).
	void OnGameLoaded();

	// The session ended: every copy goes, and every list.
	void Reset();

	std::string Describe();
}
