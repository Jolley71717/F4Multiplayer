#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// NPCs a game makes at runtime (a random encounter's raiders, an ambush), shared between games (SpawnBook.h): ours go
// to the server under spawn ids and our game runs them; friends' are placed here as copies, which NpcSync mirrors.
// Power armor frames a game made at runtime are shared the same way (FrameShare::SharesMade): a friend's is an empty
// copy of its frame here, standing where their game has it while that is in our sight, and moved by their states.
// Every id NpcSync and the wire use for one is its spawn id; LocalOf and IdOf turn it into our game's own NPC and back.
// Main thread only.
namespace Spawns
{
	// NpcSync's scan found a runtime NPC loaded (alive or dead): the spawn id our game shares it under (0: none). One our
	// game made that is ours to share (fighting, or hostile to our player) and not shared yet is reported (TakeOutgoing).
	// Our copies of friends' spawns are not ours: 0.
	std::uint32_t Share(RE::Actor* a_actor);

	// NpcSync's walk found a power armor frame our game made (a_share: FrameShare::SharesMade says it is ours to share
	// now): the spawn id it goes by once the server granted it, which keeps it ours (0: not shared, or not granted yet).
	// One not shared yet is reported (TakeOutgoing), while our game shares fewer frames than FrameShare::MAX_MADE. One
	// that is no longer ours to share gets 0, and NpcSync lets it go.
	std::uint32_t ShareFrame(RE::TESObjectREFR* a_frame, bool a_share);

	// Whether our copy of a frame a_owner's game made stands where their status says they left their frame parked
	// (a_parked, within a_radius): it stands for that frame, and RemotePlayers places no parked copy of its own there.
	[[nodiscard]] bool FrameCopyAt(std::uint32_t a_owner, const Protocol::ParkedFrame& a_parked, float a_radius);

	// The friend whose frame (one their game made and shares) our reference a_local is a copy of, 0 if it is no copy of a
	// friend's frame (see FriendFrame).
	[[nodiscard]] std::uint32_t FrameCopyOwnerOf(std::uint32_t a_local);

	// Our NPC under a spawn id: our own, or our copy of a friend's (0: none).
	[[nodiscard]] std::uint32_t LocalOf(std::uint32_t a_id);

	// The spawn id of one of our runtime NPCs: our own shared one, or our copy of a friend's (0: none).
	[[nodiscard]] std::uint32_t IdOf(std::uint32_t a_local);

	// The spawn id our own NPC is shared under (not a copy's), 0 if none.
	[[nodiscard]] std::uint32_t OwnIdOf(std::uint32_t a_local);

	// The spawn our copy is of (not our own NPC's), 0 if it is no copy of ours.
	[[nodiscard]] std::uint32_t CopyIdOf(std::uint32_t a_local);

	// A friend's game shares one (kSpawned).
	void Add(const Protocol::Spawn& a_spawn);

	// Who runs NPCs now (kActorOwners): answers to our reports, and the end of spawns (an owner of 0).
	void OnOwners(const std::vector<Protocol::ActorOwner>& a_owners);

	// Its owner's state (NpcSync): where our copy is placed.
	void OnState(const Protocol::ActorState& a_state);

	// A spawn died in another game (kActorDied): our copy, or our own NPC (a friend's believed kill), is killed here.
	void ApplyDeath(const Protocol::ActorDeath& a_death);

	// One of our runtime NPCs died here: the spawn id to report its death under (0: none, or one we were told of).
	std::uint32_t OnLocalDeath(std::uint32_t a_local);

	// Places and deletes copies. Every frame while in a session.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Before a save or a load: our copies are deleted (never saved), and placed again afterwards while we stand near. A
	// frame copy someone is being sent out of is kept as it is, saved with the frame they wear (RemotePlayers::SomeoneSentOutOf).
	void DiscardCopies();

	// A loan of the frame a_id to our player is over (FrameLoans): our copy goes as any does, and is placed again where its
	// owner's game has the frame. False when we have no copy of it placed (the session ended, say).
	bool DropLentCopy(std::uint32_t a_id);

	// Our copies of friends' frames now (RemotePlayers tells the container menu's hook which our player is kept out of).
	[[nodiscard]] std::vector<RE::ObjectRefHandle> FrameCopies();

	// A save was loaded: our NPCs' ids name the loaded game's now, and our copies went before it.
	void OnGameLoaded();

	// The session ended: copies deleted, both books cleared.
	void Reset();

	std::string Describe();
}
