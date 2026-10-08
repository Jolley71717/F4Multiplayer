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

	// The player's look; their stand-in is rebuilt with it.
	void SetFace(std::uint32_t a_id, Protocol::Face a_face);

	// Dev: throw away every stand-in's face copy and rebuild (after Face::SetParts).
	void RebuildFaces();

	// Their Pip-Boy light is on (from their status): a light follows their stand-in.
	void SetLight(std::uint32_t a_id, bool a_on);

	// Dev: the light form placed at a stand-in whose player has their Pip-Boy light on.
	void SetLightForm(std::uint32_t a_formId);

	struct Info
	{
		std::uint32_t                        id = 0;
		std::string                          name;
		std::optional<Protocol::PlayerState> state;  // latest received
		RE::Actor*                           actor = nullptr;  // their stand-in, if spawned
	};

	[[nodiscard]] std::vector<Info> List();

	// The player's name, or "" if unknown.
	[[nodiscard]] std::string NameOf(std::uint32_t a_id);

	// Their health in percent, shown after their name when they're hurt or down.
	void SetHealth(std::uint32_t a_id, std::uint8_t a_percent, bool a_downed);

	// The player fired: their stand-in plays the firing animation.
	void PlayShot(std::uint32_t a_id);

	// Replays a reload or aiming on their stand-in, or notes their Pip-Boy light (a ShotAction).
	void PlayAction(std::uint32_t a_id, std::uint8_t a_action);

	// The player a stand-in actor belongs to, or 0.
	std::uint32_t PlayerIdFor(std::uint32_t a_actorFormId);

	// Deletes every puppet actor but keeps the players; they respawn on the next Update.
	// Used before saving and loading so puppets never end up in a save file.
	void DespawnAll();

	// Spawns, moves and despawns puppets. Call once per frame.
	void Update();


	[[nodiscard]] std::string Describe();
}
