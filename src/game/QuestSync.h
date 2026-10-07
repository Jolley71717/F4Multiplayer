#pragma once

#include "Protocol.h"

// Quest progress: when a side quest (or, with a shared story, a story or faction quest) reaches a
// new stage in one player's game, the others' games set the same stage once they have started that
// quest too, and not in the middle of a conversation or scene, nor in the opening. Misc quests
// (radiant, ambient) are not shared because their targets differ per game. Main thread only.
namespace QuestSync
{
	void Apply(const Protocol::QuestStage& a_stage);

	// The player has started this quest: one of its objectives has been shown. (A quest can be
	// running long before that: a new game already runs some side quests in the background.)
	[[nodiscard]] bool PlayerStarted(const RE::TESQuest* a_quest);

	// Another player completed a quest: tell the player who did it.
	void ApplyDone(const Protocol::QuestDone& a_done);

	// Looks for stage changes (twice a second) and applies stages that couldn't be applied yet.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forget the known stages; the next Frame takes a fresh baseline (after loading a save, which
	// can move quests back; the session's stages are then applied again).
	void Rebaseline();

	// When a stage change of a quest that gives XP (shared and misc quests) was last seen here.
	[[nodiscard]] std::chrono::steady_clock::time_point LastStageChange();

	std::string Describe();
}
