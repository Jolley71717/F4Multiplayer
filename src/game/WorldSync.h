#pragma once

#include "Protocol.h"

// Shared world state: things that happen in one player's world are replayed in everyone's.
// Actor deaths and health, container contents, picked-up items, doors and locks, and (through
// QuestSync) quest stages. Also forwards NPC hits on remote players' stand-ins. Main thread only.
//
// Remote changes are applied only while the player is in the world (not during loading screens
// or at the main menu) and to loaded references; anything else waits until it can be applied.
namespace WorldSync
{
	// Registers game event sinks. Call once game data is loaded.
	void Install();

	// The server welcomed us into session a_sessionId.
	void OnWelcome(std::uint64_t a_sessionId, std::uint32_t a_localPlayerId);

	// What our world already contains, for Hello and after loading a save.
	[[nodiscard]] Protocol::WorldRequest ResyncPoint();

	void ApplyRemoteDeath(std::uint32_t a_refId);
	void ApplyWorldState(const Protocol::WorldState& a_state);
	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health);
	void ApplyContainerChange(const Protocol::IndexedContainerChange& a_change);
	void ApplyRemotePickup(std::uint32_t a_refId);
	void ApplyRemoteRefState(const Protocol::RefState& a_state);

	// Applies pending remote changes as references load, and collects local events. Call every frame.
	void Frame();

	// Local events to send to the server since the last call.
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forget what we know about the world (on disconnect and after loading a save). The record of
	// which container changes the world contains is kept: that belongs to the world, not the connection.
	void Reset();

	// Save-game record (F4SE co-save): which session container changes this save contains.
	void SaveResyncPoint(const F4SE::SerializationInterface* a_intfc);
	void LoadResyncPoint(const F4SE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void RevertResyncPoint();

	// True when remote changes can be applied (in the world, no loading screen).
	[[nodiscard]] bool InWorld();

	[[nodiscard]] std::string Describe();
}
