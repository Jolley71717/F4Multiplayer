#pragma once

#include "Protocol.h"

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
