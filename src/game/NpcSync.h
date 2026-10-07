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

	// Whether our game runs this NPC.
	[[nodiscard]] bool RunsLocally(std::uint32_t a_refId);

	// The NPC the player is talking to (0 = none).
	[[nodiscard]] std::uint32_t TalkingTo();

	// The NPC fired in its owner's world: play the firing animation on our copy.
	void ApplyShot(std::uint32_t a_refId);

	// What an NPC wears and holds in its owner's world: dress our copy the same.
	void ApplyEquipment(const Protocol::NpcEquipment& a_equipment);

	// The local player hit or talked to this NPC: take it over so it reacts here.
	void OnLocalInteraction(std::uint32_t a_refId);

	// The local player activated this reference (talking to an NPC starts this way).
	void OnActivated(std::uint32_t a_refId);

	// Claims, releases, sends owned NPCs' states and moves mirrored ones. Call every frame.
	void Frame();

	std::vector<Outgoing> TakeOutgoing();

	// Gives every mirrored NPC back to its own AI (before saving, and when leaving the session).
	void ReleaseMirrors();

	// Forget everything (on disconnect).
	void Reset();

	std::string Describe();
}
