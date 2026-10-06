#pragma once

#include "Protocol.h"

// Other players in the session, shown in our world as puppet actors.
// All functions must be called from the game's main thread.
namespace RemotePlayers
{
	// a_appearance is the NPC base form the player asked to be shown as (0 = our default).
	void Add(std::uint32_t a_id, std::string a_name, std::uint32_t a_appearance = 0);
	void Remove(std::uint32_t a_id);
	void RemoveAll();

	// Feeds a state received from the server.
	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state);

	// What the player is wearing and holding (base forms); copied onto their puppet.
	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items);

	// Deletes every puppet actor but keeps the players; they respawn on the next Update.
	// Used before saving and loading so puppets never end up in a save file.
	void DespawnAll();

	// Spawns, moves and despawns puppets. Call once per frame.
	void Update();

	[[nodiscard]] std::size_t Count();
	[[nodiscard]] std::string Describe();
}
