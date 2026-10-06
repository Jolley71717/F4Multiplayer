#pragma once

#include "Protocol.h"

// Quest progress: when a story, faction or side quest reaches a new stage in one player's game,
// the others' games set the same stage. Misc quests (radiant, ambient) are not shared because their
// targets differ per game. Main thread only.
namespace QuestSync
{
	void Apply(const Protocol::QuestStage& a_stage);

	// Another player completed a quest: tell the player who did it.
	void ApplyDone(const Protocol::QuestDone& a_done);

	// Looks for stage changes (twice a second) and applies stages that couldn't be applied yet.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forget the known stages; the next Frame takes a fresh baseline (after loading a save, which
	// can move quests back; the session's stages are then applied again).
	void Rebaseline();

	std::string Describe();
}
