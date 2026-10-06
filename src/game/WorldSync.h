#pragma once

#include "Protocol.h"

// Shared world state: things that happen in one player's world are replayed in everyone's.
// Currently: actor deaths and health. Main thread only.
namespace WorldSync
{
	// Registers game event sinks. Call once game data is loaded.
	void Install();

	// Deaths reported by the server (another player killed these).
	void ApplyRemoteDeath(std::uint32_t a_refId);
	void ApplyWorldState(const Protocol::WorldState& a_state);
	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health);

	// Applies pending remote changes to actors as they load. Call every frame.
	void Frame();

	// Local events to send to the server since the last call.
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forget session state (on disconnect).
	void Reset();

	[[nodiscard]] std::string Describe();
}
