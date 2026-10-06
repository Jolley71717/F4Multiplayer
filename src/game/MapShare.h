#pragma once

#include "Protocol.h"

// Shared map discoveries: a location one player discovers shows up on everyone's map, ready for
// fast travel. Map markers are persistent references, so their form IDs match across games.
// Main thread only.
namespace MapShare
{
	// Looks for newly discovered markers every 2 s and applies waiting ones. Call every frame.
	void Frame();

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Markers another player discovered (playerId 0: the session's catch-up list).
	void Apply(const Protocol::MarkersFound& a_found);

	// Forget what the map looked like; the next Frame takes a fresh baseline (after loading a save).
	void Rebaseline();

	// When this player last discovered a location (discoveries give XP).
	[[nodiscard]] std::chrono::steady_clock::time_point LastDiscovery();

	void Reset();

	[[nodiscard]] std::string Describe();
}
