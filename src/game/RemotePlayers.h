#pragma once

#include "game/RemoteEdits.h"

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

	// What the player is wearing and holding (base forms), and the mods on those that carry any; copied onto their puppet.
	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items, std::vector<Protocol::ItemMods> a_mods);

	// The player's look; their stand-in is rebuilt with it.
	void SetFace(std::uint32_t a_id, Protocol::Face a_face);

	// Dev: throw away every stand-in's face copy and rebuild (after Face::SetParts).
	void RebuildFaces();

	// Their Pip-Boy light is on (from their status): a light follows their stand-in.
	void SetLight(std::uint32_t a_id, bool a_on);
	// Their power armor state, the base form of the frame they are in and, when every game has that frame (a
	// pre-placed one), its reference (else 0); out of it, the frame they got out of last, where it stands in their world
	// (a copy is shown there).
	void SetPowerArmor(std::uint32_t a_id, bool a_on, std::uint32_t a_frameBase, std::uint32_t a_frameRef, const std::optional<Protocol::ParkedFrame>& a_parked = std::nullopt);
	// The furniture they sit, lie or work in (a chair, a bed, a workbench, a terminal; not a power armor frame), or none:
	// their stand-in is put in our copy of it, and stood up as they get up (StandInSeat.h).
	void SetSeat(std::uint32_t a_id, const std::optional<Protocol::Seat>& a_seat);
	// The form ID of a player's stand-in, or 0 when they have none right now.
	std::uint32_t ActorIdOf(std::uint32_t a_id);

	// Dev: the light form placed at a stand-in whose player has their Pip-Boy light on.
	void SetLightForm(std::uint32_t a_formId);

	// The game's uGridsToLoad (StandInRange), read once the game has read its ini: how far outside the engine draws
	// around our player, for what is in our sight (a stand-in, a frame).
	[[nodiscard]] int GridsToLoad();

	struct Info
	{
		std::uint32_t                        id = 0;
		std::string                          name;
		std::optional<Protocol::PlayerState> state;  // latest received
		RE::Actor*                           actor = nullptr;  // their stand-in, if spawned
	};

	[[nodiscard]] std::vector<Info> List();
	// Whether anyone else is in the session (List copies them all; this is for every frame).
	[[nodiscard]] bool Any();

	// Where a player last said they were, if they are in our cell or worldspace; nothing otherwise.
	[[nodiscard]] std::optional<RE::NiPoint3> PositionOf(std::uint32_t a_id);

	// The last state a player sent, wherever they are (nothing if none yet, or they are gone).
	[[nodiscard]] std::optional<Protocol::PlayerState> LastStateOf(std::uint32_t a_id);

	// The player's name, or "" if unknown.
	[[nodiscard]] std::string NameOf(std::uint32_t a_id);

	// Their health in percent, shown after their name when they're hurt or down.
	void SetHealth(std::uint32_t a_id, std::uint8_t a_percent, bool a_downed);

	// The player fired: their stand-in plays the firing animation, with their gun's sound (Shot::sound).
	void PlayShot(std::uint32_t a_id, std::uint32_t a_sound);

	// Replays a reload or aiming on their stand-in, or notes their Pip-Boy light (a ShotAction).
	void PlayAction(std::uint32_t a_id, std::uint8_t a_action);

	// The player a stand-in actor belongs to, or 0.
	std::uint32_t PlayerIdFor(std::uint32_t a_actorFormId);

	// Whether stand-ins may be placed from this base: the default settlers, the ini's iPuppetBaseForm, and
	// the looks friends asked for this run (non-unique NPCs; a friend's look is known only once that friend
	// has joined). So SaveGuard can tell a stand-in an earlier version saved.
	[[nodiscard]] bool IsStandInBase(std::uint32_t a_base);

	// Deletes every puppet actor but keeps the players; they respawn on the next Update.
	// Used before saving and loading so puppets never end up in a save file.
	void DespawnAll();
	// Deletes another reference the session placed (Spawns' copies of friends' NPCs) the way stand-ins go: disabled
	// once that is safe, then deleted, and never written into a save (DespawnAll deletes what still waits).
	void Discard(RE::ObjectRefHandle a_ref, std::chrono::steady_clock::time_point a_placedAt);
	// Places an empty copy of a power armor frame (as the one a stand-in gets into), kept out of saves: Spawns' copy of a
	// frame a friend's game made. In our player's cell (a_cell: an interior, or outside in its worldspace). Deleted with
	// Discard.
	RE::ObjectRefHandle PlaceFrameCopy(std::uint32_t a_base, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading);
	// The copy we show of the frame a_owner left parked (see ArmorFrames::ChoosePark), handed over when it is of a_base,
	// stands within ArmorFrames::ORIGINAL_RADIUS of a_spot and nobody is in it: Spawns' copy of the same frame, once
	// their game shares it, takes it over rather than standing a second suit in the same spot. None otherwise.
	RE::ObjectRefHandle TakeParkedCopy(std::uint32_t a_owner, std::uint32_t a_base, const RemoteEdits::Spot& a_spot);
	// A save was loaded: forgets handles and runtime forms from before it.
	void OnGameLoaded();

	// Spawns, moves and despawns puppets. Call once per frame.
	void Update();


	[[nodiscard]] std::string Describe();
}
