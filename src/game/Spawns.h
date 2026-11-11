#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>
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
	// game made that is ours to share (fighting, or hostile to our player; a random encounter's actor calm or not,
	// Encounters.h) and not shared yet is reported (TakeOutgoing). Our run of an encounter that started where a friend's
	// already ran is stopped here instead, as its first actor is seen, and its actors are removed (Frame). Our copies of
	// friends' spawns are not ours: 0.
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

	// The spawn our copy is of (not our own NPC's), 0 if it is no copy of ours. Our quest's own creature paired with a
	// friend's spawn from the same alias (QuestActors.h) counts as our copy of it: mirrored, its roll and factions theirs
	// (it is never deleted, and saved as our game's: unpaired before every save).
	[[nodiscard]] std::uint32_t CopyIdOf(std::uint32_t a_local);

	// A friend's game shares one (kSpawned).
	void Add(const Protocol::Spawn& a_spawn);

	// Who runs NPCs now (kActorOwners): answers to our reports, and the end of spawns (an owner of 0).
	void OnOwners(const std::vector<Protocol::ActorOwner>& a_owners);

	// Its owner's state (NpcSync): where our copy is placed.
	void OnState(const Protocol::ActorState& a_state);

	// A state of a plugin's NPC a_owner's game runs that our game has no reference of (NpcSync found none under its id):
	// a temporary reference a quest of theirs took out of a cell we have not loaded (SpawnBook::AbsentBase). It is booked
	// as a friend's spawn is, under its own id, and our copy of it placed and mirrored as theirs is; the state moves it.
	// False when nothing names what to place it from.
	bool Absent(const Protocol::ActorState& a_state, std::uint32_t a_owner);

	// Whether a_id, a plugin's reference, is a friend's NPC our game has no reference of, booked by Absent: our copy of it
	// stands in for it (Spawns::LocalOf names it). Never ours to run: what our player hits is a copy, no NPC of ours.
	[[nodiscard]] bool IsAbsent(std::uint32_t a_id);

	// Whether a friend's spawn a_id is known here, alive and still theirs, so a copy of it is placed or may be about to
	// be (Conversations holds its line for the copy a moment). False for one over, or none known.
	[[nodiscard]] bool Expects(std::uint32_t a_id);

	// A friend's frame was given back from a loan (kFrameReturned with a place: FrameLoan::ChoosePlace), or our player
	// gives one back (FrameLoans, as the return goes): it stands where its borrower got out of it from now on, in every
	// game, and our copy is placed there, not where the owner's frame stood as it was lent (SpawnBook::RETURN_HOLD).
	// False when it says no place, or we know no such spawn.
	bool GivenBack(const Protocol::FrameReturn& a_return);

	// A spawn died in another game (kActorDied): our copy, or our own NPC (a friend's believed kill), is killed here.
	void ApplyDeath(const Protocol::ActorDeath& a_death);

	// Whether our runtime NPC a_local was killed here for another game's report (ApplyDeath), its death event not yet come.
	[[nodiscard]] bool KilledHere(std::uint32_t a_local);

	// One of our runtime NPCs died here: the spawn id to report its death under (0: none, or one we were told of).
	std::uint32_t OnLocalDeath(std::uint32_t a_local);

	// Random encounters (Encounters.h), for their runs shared (EncounterRuns.h): whether a quest is an encounter in our
	// game's files; whether our run of one stands for good (Encounters::Book::Stands; nothing when the book knows no run
	// of it: none of its actors seen alive); whether ours is giving way (stopped lately, or within its window still);
	// whether a_player's game still shares a live actor of their run of it.
	[[nodiscard]] bool                IsEncounter(const RE::TESQuest* a_quest);
	[[nodiscard]] std::optional<bool> EncounterStands(std::uint32_t a_quest);
	[[nodiscard]] bool                EncounterGivingWay(std::uint32_t a_quest);
	[[nodiscard]] bool                TheirEncounterLive(std::uint32_t a_player, std::uint32_t a_quest);

	// Our copy of a friend's spawn as a run's alias can take it: the copy placed now (0: none yet), and whether the spawn
	// is over (dead, let go of, or unknown here), so no copy is to be waited for.
	struct CopyHere
	{
		std::uint32_t local = 0;
		bool          over = false;
	};
	[[nodiscard]] CopyHere CopyFor(std::uint32_t a_id);

	// An NPC hit our player (WorldSync's hit watch; any thread): if it is an actor of an encounter run of ours, the run
	// stands from now on (Encounters::Book::OursHurt), seen to on the next Frame.
	void OnPlayerHitBy(std::uint32_t a_local);

	// Our own actor a_local, made by a_quest's own start (one of its aliases that creates its reference holds it) whose run
	// we follow with the owner's actors (an encounter's, or a radiant's with actors made at runtime), still held by an
	// alias that is being forced onto our copy of theirs: removed as a follower's are (loaded or not). Nothing for a copy,
	// our companion, an NPC that is no runtime one, or one the quest's start didn't make (its conditions filled the alias
	// with another quest's actor, or a settler: left as it is, `followKept=`).
	void RemoveFollowedActor(std::uint32_t a_local, std::uint32_t a_quest);

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
