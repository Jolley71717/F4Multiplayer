#pragma once

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
#include "Protocol.h"

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
#include "game/RadiantRules.h"

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// Radiant quests (the Minutemen's, the Brotherhood's, the Institute's, the Railroad's and Diamond City's wanted posters,

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// but those with NPCs made at runtime), shared by their run (RadiantRules has which and why): when this player's game

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// hands one out in a session that shares the story, the

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// friends' games start the same quest with the same settlement or dungeon, and from then on its stages go between them

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// under that run's name (not as QuestSync's stages, which name only the quest: a second run of it would be the

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// first's). A friend's game that runs the quest with other targets, not yet given to its player, stops that run for

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// this one where it may (RadiantRules::Replaces; a Railroad job stopped so is counted off the jobs on offer at HQ, as

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// its take would have: RadiantRules::OFFERS). Each game sets each stage of a run once, so the reward (the quest's

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// own stage) is paid once in each. A run whose fills a game can't match stays each player's own there, said in the

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
// log. Main thread only; QuestSync calls it.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
namespace RadiantSync

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
{

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Whether the session can share radiants now: bSharedRadiants and bSyncQuests on, and the story shared.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] bool Enabled();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// The radiant a quest is, by the game file that made it and its form ID there (RadiantRules::RADIANTS), whatever load

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// order this game has: nothing for a quest that is none, or from a mod. The runtime form ID overloads look the quest up.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] const RadiantRules::Entry* EntryOf(const RE::TESQuest* a_quest);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] const RadiantRules::Entry* EntryOf(std::uint32_t a_questId);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] RadiantRules::Share        ShareOf(const RE::TESQuest* a_quest);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] RadiantRules::Share        ShareOf(std::uint32_t a_questId);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] bool                       IsRadiant(const RE::TESQuest* a_quest);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] bool                       IsRadiant(std::uint32_t a_questId);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// This player's id in the session (the welcome's).

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void SetLocalPlayer(std::uint32_t a_id);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Welcomed into a_session (after SetLocalPlayer and the story's sharing are set): the runs this world shared in it

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// (a save's record, or those kept when the player left it) are shared again as they were.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnWelcome(std::uint64_t a_session);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// A friend's game started a run; their stages of it.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnStart(const Protocol::RadiantStart& a_start);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnStage(const Protocol::RadiantStage& a_stage);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Someone joined: our runs going now are sent again, for them (the others' games ignore runs they know), and the

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// stages done here under every run shared here (one who came back knows a run only as it was when they left).

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnPlayerJoined();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// A friend's game loaded a save: it knows our runs only as its save had them (its record), or not at all, so ours

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// going are sent again, and the stages done here under every run shared here. One it finished before the load is never

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// started there again (RadiantRules::Book keeps them, and its record).

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnPlayerReloaded(std::uint32_t a_playerId);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Whether this radiant's run here is shared with friends now (its friends' stages are set here).

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	[[nodiscard]] bool Shares(std::uint32_t a_quest);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Stages of a radiant done here by this player (QuestSync's poll): reported under the runs shared here.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnDone(const RE::TESQuest* a_quest, std::span<const std::uint16_t> a_stages);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// Looks for runs this game starts, finishes starting friends' and ends those whose quest stopped. a_inSession: welcomed.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void Frame(bool a_inSession);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// The messages to send (QuestSync's outbox takes them, as the story's).

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// The session ended; another server; a save loaded.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnSessionEnded();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void OnServerChanged();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void Rebaseline();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// The co-save's record of the runs (RadiantRules::Saved): written with each save, read back with it (on the save's or

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// load's thread, frames held out), and taken by the Rebaseline after the load. Empty: none to write.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	std::vector<std::uint8_t> SaveRecord();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void                      LoadRecord(std::span<const std::uint8_t> a_bytes);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	void                      RevertRecord();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	// For the developer's commands: a quest's aliases, their fill definitions and fills here.

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	std::string DumpAliases(const RE::TESQuest* a_quest);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);


	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
	std::string Describe();

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
}

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running.
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location);
