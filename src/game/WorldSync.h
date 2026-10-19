#pragma once

#include "Protocol.h"

#include "game/FoundDead.h"

namespace LootSync
{
	class Loot;
}

// Shared world state: things that happen in one player's world are replayed in everyone's.
// Actor deaths and health, container contents, picked-up items, doors and locks (and the stages that open the doors
// only a quest's script opens: ScriptDoors), and (through QuestSync) quest stages. Also forwards NPC hits on remote
// players' stand-ins. Main thread only.
//
// Remote changes are applied only while the player is in the world (not during loading screens
// or at the main menu) and to loaded references; anything else waits until it can be applied.
namespace WorldSync
{
	// Registers game event sinks. Call once game data is loaded.
	void Install();

	// The server welcomed us (the session's side of it goes through SessionSteps).
	void OnWelcome(bool a_friendlyFire);

	// The loot engine: which of the session's container changes our world holds (or waits to), and our IDs in
	// it (kept in the save). Session moves it through loads, hellos and welcomes with SessionSteps.
	[[nodiscard]] LootSync::Loot& Loot();

	// Another player's game reported a death, or the server replays one it knows (no player named): killed
	// here once loaded, the corpse put where it fell for them and pushed by their stand-in if theirs was the
	// kill. With everyone on their own story, a unique or essential character dies here only if the reporter
	// was there to see it (DeathReports).
	void ApplyRemoteDeath(const Protocol::ActorDeath& a_death);

	// What makes the actor a character of a player's own story (unique, essential or protected, in a quest's alias).
	[[nodiscard]] FoundDead::Npc DescribeStory(RE::Actor* a_actor);
	void ApplyRemoteActivation(std::uint32_t a_refId, std::uint32_t a_playerId);  // another player pressed a button, switch or lever
	void ApplyWorldState(const Protocol::WorldState& a_state);
	// A session's quest stage that opens a door only that stage opens (ScriptDoors: Vault 114's gear door), live or
	// caught up: set here once the door is loaded. False for any other stage, which is QuestSync's.
	[[nodiscard]] bool ApplyScriptDoor(const Protocol::QuestStage& a_stage);
	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health);
	// Another player took a_damage (a fraction of the maximum) off their copy of an NPC: taken off here if our game runs it
	// (its health then goes to everyone, HealthShare); nothing otherwise.
	void ApplyRemoteDamage(std::uint32_t a_refId, float a_damage);
	// Who runs these NPCs (as NpcSync is told). When we host, one another player's game runs that is dead
	// in ours is reported dead: our world is the session's (with everyone on their own story, not a story
	// character: FoundDead::Shared). The server gives nobody a dead NPC, so a death we hold for one that now has
	// an owner was undone (an older build's mistake, undone by the host's game): it is forgotten, our copy alive.
	void OnActorOwners(const std::vector<Protocol::ActorOwner>& a_owners);
	// Whether our game may offer to run this NPC (NpcSync): not one the session has dead, nor any until the session's
	// world state has come after a load or a welcome (our game may have it alive from a save older than its death).
	// One whose cell reset since our game killed it is ours to offer again (SessionDeaths). a_essential: our game
	// keeps it essential (IsEssential), so a death the session has for it is held, not applied, and it may be offered
	// marked so (the host's game's offer undoes an older build's mistake: Server's handleClaims).
	[[nodiscard]] bool MayClaim(std::uint32_t a_refId, bool a_essential = false);
	// The game won't let it die: essential by its base, or on the reference (a quest's alias: Dogmeat's). At the
	// end of its health it goes down and gets up, so going down is never reported as a death, and a death heard
	// of for it waits until it can die here.
	[[nodiscard]] bool IsEssential(RE::Actor* a_actor);
	void ApplyContainerChange(const Protocol::IndexedContainerChange& a_change);
	void ApplyRemotePickup(std::uint32_t a_refId);
	void ApplyRemoteRefState(const Protocol::RefState& a_state);

	// Applies pending remote changes as references load, and collects local events. Call every frame.
	// a_hosting: this game runs the session's server; its world is the session's (actors dead in it are dead for all).
	void Frame(bool a_hosting);

	// Before a save or a load: the deaths and the player's item moves since the last frame are looked at
	// now, while the world they were made in stands, and what is the session's joins what TakeOutgoing
	// returns (ahead of a load's request, so the server numbers it before answering).
	void FlushLoot();

	// How many actors the player killed since the last call (any actor, shared or not).
	[[nodiscard]] std::uint32_t TakePlayerKills();

	// Local events to send to the server since the last call (a request for container changes a gap
	// still lacks among them).
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
