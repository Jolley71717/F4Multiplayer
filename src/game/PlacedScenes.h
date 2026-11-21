#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// A town's scene over placed NPCs (Diamond City's synth accusation) played once for the session: the game whose claim the
// server kept first runs it, the others follow it with their stages marked and the runner's NPCs shown
// (PlacedSceneRules.h has the rules). Main thread only; QuestSync, NpcSync, Spawns and WorldSync call it.
namespace PlacedScenes
{
	// A friend's stage (live, or from the session's state): true when it is one of these quests', taken here (QuestSync
	// sets none of it then).
	bool OnFriendStage(const Protocol::QuestStage& a_stage, bool a_live);

	// Reads our quests, sends our run's stages, sets a claim's stage for a friend's stand-in in its trigger's area, and
	// stops a run of ours that yielded once the runner's ended. QuestSync's poll calls it. a_inSession: welcomed.
	void Frame(bool a_inSession);

	// The reports to send (QuestSync's outbox takes them).
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Whether our run is the session's and the NPC is one it runs: NpcSync claims it as a conversation's speaker is, and
	// never hands it off.
	[[nodiscard]] bool Holds(std::uint32_t a_ref);

	// Whether our run of the quest is the session's: the actors its start made are ours to share calm (Spawns).
	[[nodiscard]] bool SharesActors(std::uint32_t a_quest);

	// Whether our run of the quest yielded to a friend's: the actors its start made are removed, theirs stand (Spawns).
	[[nodiscard]] bool FollowsActors(std::uint32_t a_quest);

	// Whether the NPC stays off here: a run we followed without ever running it ended (its shutdown deleted the NPC in the
	// runner's game), so a death reported of it doesn't switch it on (WorldSync).
	[[nodiscard]] bool KeepsOff(std::uint32_t a_ref);

	// Another server: our stages go to it again (its state has none of them yet).
	void OnServerChanged();

	// The session ended: a run of ours that yielded is stopped, and a followed run's NPCs we showed are switched off (our
	// quest never ran them).
	void OnSessionEnded();

	// A save loaded: what we knew is of another world.
	void Rebaseline();

	std::string Describe();
}
