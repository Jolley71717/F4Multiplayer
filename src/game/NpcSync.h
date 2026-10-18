#pragma once

#include "game/RemoteEdits.h"

#include "Protocol.h"

// Shared NPC AI. Each NPC is run by one player's game (its owner, assigned by the server); the
// other players' games turn its AI off and copy its movement. Main thread only.
// Power armor frames standing in the world are shared the same way (FrameShare.h): their owner (whoever last used one,
// else the host) tells where it stands, and the other games move their own reference of it there. One our game made at
// runtime is shared as a spawn instead (FrameShare::SharesMade, Spawns): the other games place a copy of it.
namespace NpcSync
{
	struct Outgoing
	{
		std::vector<std::uint8_t> data;
		bool                      reliable;
	};

	// Our player ID once welcomed, 0 otherwise.
	void SetLocalPlayer(std::uint32_t a_id);
	[[nodiscard]] std::uint32_t LocalPlayer();

	void ApplyOwners(const std::vector<Protocol::ActorOwner>& a_owners);
	void ApplyStates(const std::vector<Protocol::ActorState>& a_states);

	// Whether the actor is our own current companion (it follows us, whoever runs it elsewhere).
	[[nodiscard]] bool IsOurCompanion(RE::Actor* a_actor);

	// Whether our game runs this NPC.
	[[nodiscard]] bool RunsLocally(std::uint32_t a_refId);

	// The NPC the player is talking to (0 = none).
	[[nodiscard]] std::uint32_t TalkingTo();

	// The NPC fired in its owner's world: play the firing animation on our copy, with the sound of the
	// owner's copy's gun (Shot::sound).
	void ApplyShot(std::uint32_t a_refId, std::uint32_t a_sound);

	// A mirrored NPC's owner reported it reloading or aiming: replay it.
	void ApplyAction(std::uint32_t a_refId, std::uint8_t a_action);

	// What an NPC wears and holds in its owner's world: dress our copy the same.
	void ApplyEquipment(const Protocol::NpcEquipment& a_equipment);

	// The local player hit or talked to this NPC: take it over so it reacts here.
	void OnLocalInteraction(std::uint32_t a_refId);

	// Whether the local player hit or talked to this NPC lately (NpcHold::FOUGHT).
	[[nodiscard]] bool FoughtLately(std::uint32_t a_refId);

	// The local player activated this reference (talking to an NPC starts this way).
	void OnActivated(std::uint32_t a_refId);

	// Claims, releases, sends owned NPCs' states and moves mirrored ones. Call every frame. a_hosting: our game's
	// server is the session's (its world is the session's: it offers to run every power armor frame standing in its
	// loaded cells, FrameShare::Offers).
	void Frame(bool a_hosting);

	// Console commands run for NPCs outside NpcSync this frame (WorldSync's kills), for `net status`.
	void CountCommands(std::uint32_t a_commands);

	std::vector<Outgoing> TakeOutgoing();

	// Gives every mirrored NPC back to its own AI (before saving, and when leaving the session).
	void ReleaseMirrors();

	// Lets go of one mirrored NPC (before Spawns deletes our copy of a friend's spawn); its owner's next state mirrors it
	// again if it is still there.
	void DropMirror(std::uint32_t a_refId);

	// Forget everything (on disconnect).
	void Reset();

	// Which player runs the NPC (0: nobody yet).
	std::uint32_t Owner(std::uint32_t a_refId);

	// The id the session (and every call above) knows one of our game's NPCs by: a plugin's reference is the same in
	// every game; one made at runtime goes by its spawn id (Spawns: our own shared one, or our copy of a friend's).
	// 0: never shared (the player, a stand-in, a runtime NPC of our game's own).
	[[nodiscard]] std::uint32_t SessionId(std::uint32_t a_formId);

	// Our game's NPC under an id the session uses: the reference's own form id, or our runtime NPC's for a spawn id
	// (0: none here).
	[[nodiscard]] std::uint32_t LocalRef(std::uint32_t a_sessionId);

	std::string Describe();

	// Whether a spot is in the area our engine draws around our player: our interior, or outside within the cells it
	// attaches around ours. Not during a loading screen. Where a friend's frame is shown (Spawns' copies).
	[[nodiscard]] bool InSight(const RemoteEdits::Spot& a_spot);

	// Whether an actor of ours (not our player) is in this power armor frame.
	[[nodiscard]] bool InFrame(const RE::TESObjectREFR* a_frame);

	// Dev: one mirror's owner, snapshots and distance from its owner's copy (why it is or isn't puppeted); for a power
	// armor frame, who runs it and where ours stands.
	std::string DescribeMirror(std::uint32_t a_refId);
}
