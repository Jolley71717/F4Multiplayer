#pragma once

#include "Protocol.h"
#include "game/RadiantRules.h"

// Radiant quests (the Minutemen's, the Brotherhood's, the Institute's, the Railroad's and Diamond City's wanted posters,
// but those with NPCs made at runtime), shared by their run (RadiantRules has which and why): when this player's game
// hands one out in a session that shares the story, the
// friends' games start the same quest with the same settlement or dungeon, and from then on its stages go between them
// under that run's name (not as QuestSync's stages, which name only the quest: a second run of it would be the
// first's). A friend's game that runs the quest with other targets, not yet given to its player, stops that run for
// this one where it may (RadiantRules::Replaces; a Railroad job stopped so is counted off the jobs on offer at HQ, as
// its take would have: RadiantRules::OFFERS). Each game sets each stage of a run once, so the reward (the quest's
// own stage) is paid once in each. A run whose fills a game can't match stays each player's own there, said in the
// log. Main thread only; QuestSync calls it.
namespace RadiantSync
{
	// Whether the session can share radiants now: bSharedRadiants and bSyncQuests on, and the story shared.
	[[nodiscard]] bool Enabled();

	// The radiant a quest is, by the game file that made it and its form ID there (RadiantRules::RADIANTS), whatever load
	// order this game has: nothing for a quest that is none, or from a mod. The runtime form ID overloads look the quest up.
	[[nodiscard]] const RadiantRules::Entry* EntryOf(const RE::TESQuest* a_quest);
	[[nodiscard]] const RadiantRules::Entry* EntryOf(std::uint32_t a_questId);
	[[nodiscard]] RadiantRules::Share        ShareOf(const RE::TESQuest* a_quest);
	[[nodiscard]] RadiantRules::Share        ShareOf(std::uint32_t a_questId);
	[[nodiscard]] bool                       IsRadiant(const RE::TESQuest* a_quest);
	[[nodiscard]] bool                       IsRadiant(std::uint32_t a_questId);

	// For the developer's commands (`radiantstart`): starts a radiant here the way the story manager's event does, with the
	// aliases the event fills (a_ref: the settlement's workshop, the quest giver; a_location: any location the event gives,
	// 0 for none) forced for the start and put back after, and a_aliases (alias ID, form) named by the caller too. It is then this game's own run, shared like any (Frame). The
	// reply says whether it is running. SettlementAttacks starts its mirror of the host's attack so too (a_who names the
	// caller in the log; one start at a time: an error while an earlier one waits for its quest to run).
	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location, std::span<const std::pair<std::uint32_t, std::uint32_t>> a_aliases,
		std::string_view a_who = "the developer's start");

	// A start of a_quest by StartWithEvent still waiting for the quest to run is given up: its forced aliases are put back
	// now (SettlementAttacks, as a session ends with a mirror's start pending). True when one was waiting and its quest
	// isn't running yet (it may still start a moment later, with what the aliases find then).
	bool CancelStart(std::uint32_t a_quest);

	// What fills a quest's reference and location aliases in our game, as the friends' games name it (a plugin's form as
	// it is, our runtime NPC by the spawn id it is shared under or as one the session doesn't share, the player left
	// out): what a run is shared with (EncounterRuns.h shares encounters' runs so).
	std::vector<Protocol::RadiantFill> FillsHere(RE::TESQuest* a_quest);

	// This player's id in the session (the welcome's).
	void SetLocalPlayer(std::uint32_t a_id);
	// Welcomed into a_session (after SetLocalPlayer and the story's sharing are set): the runs this world shared in it
	// (a save's record, or those kept when the player left it) are shared again as they were.
	void OnWelcome(std::uint64_t a_session);

	// A friend's game started a run; their stages of it.
	void OnStart(const Protocol::RadiantStart& a_start);
	void OnStage(const Protocol::RadiantStage& a_stage);

	// Someone joined: our runs going now are sent again, for them (the others' games ignore runs they know), and the
	// stages done here under every run shared here (one who came back knows a run only as it was when they left).
	void OnPlayerJoined();
	// A friend's game loaded a save: it knows our runs only as its save had them (its record), or not at all, so ours
	// going are sent again, and the stages done here under every run shared here. One it finished before the load is never
	// started there again (RadiantRules::Book keeps them, and its record).
	void OnPlayerReloaded(std::uint32_t a_playerId);

	// Whether this radiant's run here is shared with friends now (its friends' stages are set here).
	[[nodiscard]] bool Shares(std::uint32_t a_quest);

	// Whether the run of this radiant shared here is a friend's whose actors are made at runtime and theirs
	// (RadiantRules::Entry::spawned): our game's own actors made for the quest are not what stands there (Spawns removes
	// them as they come), and our copies of theirs are forced into its aliases as they are placed (Frame). Followed until
	// the quest stops here, the owner leaves or loads a save, or the session ends; a quest left hollow by that (an
	// objective shown, nothing standing to do it to) is run again as our own after a while (RadiantRules::HollowJob).
	[[nodiscard]] bool FollowsActors(std::uint32_t a_quest);

	// A friend left the session: their runs we follow with their actors are followed no more (their copies go with them).
	void OnPlayerLeft(std::uint32_t a_playerId);

	// Forces our reference a_local into the quest's reference alias a_alias (ReferenceAlias.ForceRefTo, run by the script
	// VM a frame or so later). False when it could not be asked. For a followed run's aliases (here, and EncounterRuns).
	bool ForceAliasRef(RE::TESQuest* a_quest, std::uint32_t a_alias, std::uint32_t a_local);

	// Stages of a radiant done here by this player (QuestSync's poll): reported under the runs shared here.
	void OnDone(const RE::TESQuest* a_quest, std::span<const std::uint16_t> a_stages);

	// Looks for runs this game starts, finishes starting friends' and ends those whose quest stopped. a_inSession: welcomed.
	void Frame(bool a_inSession);

	// The messages to send (QuestSync's outbox takes them, as the story's).
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// The session ended; another server; a save loaded.
	void OnSessionEnded();
	void OnServerChanged();
	void Rebaseline();

	// The co-save's record of the runs (RadiantRules::Saved): written with each save, read back with it (on the save's or
	// load's thread, frames held out), and taken by the Rebaseline after the load. Empty: none to write.
	std::vector<std::uint8_t> SaveRecord();
	void                      LoadRecord(std::span<const std::uint8_t> a_bytes);
	void                      RevertRecord();

	// For the developer's commands: a quest's aliases, their fill definitions and fills here.
	std::string DumpAliases(const RE::TESQuest* a_quest);

	std::string Describe();
}
