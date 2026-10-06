#pragma once

#include "Protocol.h"

// Shared NPC AI. Each NPC is run by one player's game (its owner, assigned by the server); the
// other players' games turn its AI off and copy its movement. Main thread only.
namespace NpcSync
{
	struct Outgoing
	{
		std::vector<std::uint8_t> data;
		bool                      reliable;
	};

	// Our player ID once welcomed, 0 otherwise.
	void SetLocalPlayer(std::uint32_t a_id);

	void ApplyOwners(const std::vector<Protocol::ActorOwner>& a_owners);
	void ApplyStates(const std::vector<Protocol::ActorState>& a_states);

	// The local player hit this NPC: take it over so it fights back here.
	void OnLocalHit(std::uint32_t a_refId);

	// Claims, releases, sends owned NPCs' states and moves mirrored ones. Call every frame.
	void Frame();

	std::vector<Outgoing> TakeOutgoing();

	// Gives every mirrored NPC back to its own AI (before saving, and when leaving the session).
	void ReleaseMirrors();

	// Forget everything (on disconnect).
	void Reset();

	std::string Describe();
}
