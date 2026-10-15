#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// NPCs a game makes at runtime (a random encounter's raiders, an ambush), shared between games (SpawnBook.h): ours go
// to the server under spawn ids and our game runs them; friends' are placed here as copies, which NpcSync mirrors.
// Every id NpcSync and the wire use for one is its spawn id; LocalOf and IdOf turn it into our game's own NPC and back.
// Main thread only.
namespace Spawns
{
	// NpcSync's scan found a runtime NPC loaded (alive or dead): the spawn id our game shares it under (0: none). One our
	// game made that is ours to share (fighting, or hostile to our player) and not shared yet is reported (TakeOutgoing).
	// Our copies of friends' spawns are not ours: 0.
	std::uint32_t Share(RE::Actor* a_actor);

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

	// Before a save or a load: our copies are deleted (never saved), and placed again afterwards while we stand near.
	void DiscardCopies();

	// A save was loaded: our NPCs' ids name the loaded game's now, and our copies went before it.
	void OnGameLoaded();

	// The session ended: copies deleted, both books cleared.
	void Reset();

	std::string Describe();
}
