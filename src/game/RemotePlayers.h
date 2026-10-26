#pragma once

#include "game/Equipment.h"
#include "game/NetSim.h"
#include "game/RemoteEdits.h"

#include "Protocol.h"

// Other players in the session, shown in our world as puppet actors.
// All functions must be called from the game's main thread.
namespace RemotePlayers
{
	// a_appearance is the NPC base form the player asked to be shown as (0 = our default).
	void Add(std::uint32_t a_id, std::string a_name, std::uint32_t a_appearance = 0);
	// Their name changed (they named a new character after joining): the name tag and the player list show the new one.
	void Rename(std::uint32_t a_id, std::string a_name);
	void Remove(std::uint32_t a_id);
	void RemoveAll();

	// Feeds a state received from the server.
	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state);
	// The server sent their state, but it names a cell or worldspace our game does not have (a DLC or mod we lack), so it
	// could not be read (FormTranslate::Result::placeless). They are somewhere we cannot reach: their last state is no
	// longer where they are, so the list, the compass, the teleport and their stand-in stop using it, until a state we can
	// read comes (PushState). Not for states that stop coming (a lagging or a lost friend: handled by the connection).
	void NoteUnplaced(std::uint32_t a_id);
	// Dev: friends' states from now on come as over a worse connection (NetSim.h); off lets in what waits.
	void SetNetSim(const NetSim::Settings& a_settings);
	[[nodiscard]] std::string DescribeNetSim();

	// What the player is wearing and holding (base forms), and the mods on those that carry any; copied onto their puppet.
	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items, std::vector<Protocol::ItemMods> a_mods);

	// The player's look; their stand-in is rebuilt with it.
	void SetFace(std::uint32_t a_id, Protocol::Face a_face);

	// Dev: throw away every stand-in's face copy and rebuild (after Face::SetParts).
	void RebuildFaces();

	// Their Pip-Boy light is on (from their status): a light follows their stand-in.
	void SetLight(std::uint32_t a_id, bool a_on);
	// The kind of menu they are in (Protocol::MenuKind, from their status): in the Pip-Boy their stand-in raises its own
	// (StandInPipboy), and in any menu its name tag says so and an icon hangs over it (MenuIcons).
	void SetMenu(std::uint32_t a_id, std::uint8_t a_menu);
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
		std::optional<Protocol::PlayerState> state;  // latest received; none while they are somewhere our game lacks (unreachable)
		RE::Actor*                           actor = nullptr;  // their stand-in, if spawned
		std::uint8_t                         menu = 0;         // the menu they are in (Protocol::MenuKind), from their status
		bool                                 unreachable = false;  // their latest state names a place our game does not have (NoteUnplaced)
	};

	[[nodiscard]] std::vector<Info> List();
	// Whether anyone else is in the session (List copies them all; this is for every frame).
	[[nodiscard]] bool Any();

	// Where a player last said they were, if they are in our cell or worldspace; nothing otherwise.
	[[nodiscard]] std::optional<RE::NiPoint3> PositionOf(std::uint32_t a_id);

	// The last state a player sent, wherever they are (nothing if none yet, they are gone, or they are somewhere our game lacks).
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
	// Used before saving and loading, and before a teleport (Party), so puppets never end up in a save file: each is
	// disabled now and marked for deletion a second later (DeferredDelete::MayDelete), its record kept out of every save
	// meanwhile (one whose cell a teleport unloads too: see Update). Never both in one frame, a teleport's neither: a
	// physics job of the frames after still handles a collision with the stand-in.
	void DespawnAll();
	// Deletes another reference the session placed (Spawns' copies of friends' NPCs) the way stand-ins go: disabled
	// once that is safe, then deleted, and never written into a save (DespawnAll deletes what still waits).
	void Discard(RE::ObjectRefHandle a_ref, std::chrono::steady_clock::time_point a_placedAt);
	// Whether the reference is one of ours on its way out: buried (waiting to be disabled or deleted) or marked for deletion and
	// not yet let go of by the engine. Only those can outlive a load (SaveGuard::BeforeLoad).
	bool OnItsWayOut(const RE::TESObjectREFR* a_ref);
	// Places an empty copy of a power armor frame (as the one a stand-in gets into), kept out of saves: Spawns' copy of a
	// frame a friend's game made. In our player's cell (a_cell: an interior, or outside in its worldspace). Deleted with
	// Discard.
	RE::ObjectRefHandle PlaceFrameCopy(std::uint32_t a_base, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_at, float a_heading);
	// The copy we show of the frame a_owner left parked (see ArmorFrames::ChoosePark), handed over when it is of a_base,
	// stands within ArmorFrames::ORIGINAL_RADIUS of a_spot and nobody is in it: Spawns' copy of the same frame, once
	// their game shares it, takes it over rather than standing a second suit in the same spot. None otherwise.
	RE::ObjectRefHandle TakeParkedCopy(std::uint32_t a_owner, std::uint32_t a_base, const RemoteEdits::Spot& a_spot);
	// Our player asks to get into this frame or opens its inventory (Equipment's frame gate), or someone else here (a_by: a
	// companion ordered into it) is found in it: whether they are kept out of it, a copy the session placed of a friend's
	// frame (see FriendFrame). The HUD tells our player whose it is. Someone else is sent out of it again (each frame:
	// Actor.SwitchToPowerArmor with None, FriendFrame::SendsOut).
	[[nodiscard]] bool KeepOutOfFrame(RE::TESObjectREFR* a_frame, Equipment::FrameAsk a_ask, const RE::Actor* a_by);
	// Whether someone here is in this frame who is to be sent out of it, or just got out of it (its 3D being built again,
	// FriendFrame::Settling): the frame is not theirs, and Spawns' and our own frame copies are left as they are meanwhile
	// (not "it is theirs now", nor taken for a copy that never loaded).
	[[nodiscard]] bool BeingSentOut(RE::TESObjectREFR* a_frame);
	// Whether someone our game sends out of this frame copy is in it, getting in, or on their way out: nothing buries it
	// then (FriendFrame::ChooseLeave). A save keeps it as it is, saved with the frame they wear (Spawns' DiscardCopies too).
	[[nodiscard]] bool SomeoneSentOutOf(RE::TESObjectREFR* a_frame);
	// A copy a save or load coming keeps with someone being sent out of it (Spawns' DiscardCopies): noted, and after loading
	// the save made then, waited on again (the companion wears it still). BeforeSave keeps what the save kept.
	void NoteKeptThroughSave(RE::TESObjectREFR* a_frame);
	void BeforeSave();
	// The co-save's record of them: written with each save, read back with that save alone (cleared by any load or new game).
	[[nodiscard]] std::vector<std::uint8_t> SaveRecord();
	void LoadRecord(const std::vector<std::uint8_t>& a_bytes);
	void RevertRecord();
	// As a load comes (after DespawnAll): who was being sent out of a copy, and the copies just got out of, are forgotten
	// (their ids name the loaded game's forms), and a send-out call still queued in the VM is not to take effect after it.
	void BeforeLoad();
	// A save was loaded: forgets handles and runtime forms from before it.
	void OnGameLoaded();

	// Spawns, moves and despawns puppets. Call once per frame.
	void Update();


	[[nodiscard]] std::string Describe();
}
