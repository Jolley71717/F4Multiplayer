#pragma once

#include "Protocol.h"

// Other players in the session, shown in our world as puppet actors.
// All functions must be called from the game's main thread.
namespace RemotePlayers
{
	void Add(std::uint32_t a_id, std::string a_name);
	void Remove(std::uint32_t a_id);
	void RemoveAll();

	// Feeds a state received from the server.
	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state);

	// Deletes every puppet actor but keeps the players; they respawn on the next Update.
	// Used before saving and loading so puppets never end up in a save file.
	void DespawnAll();

	// Spawns, moves and despawns puppets. Call once per frame.
	void Update();

	[[nodiscard]] std::size_t Count();
	[[nodiscard]] std::string Describe();
}
