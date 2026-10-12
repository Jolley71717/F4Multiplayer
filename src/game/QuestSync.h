#pragma once

#include "Protocol.h"

// Quest progress: each stage a side quest (or, with a shared story, a story or faction quest) gets
// done in one player's game is set in the others' games too, in order, once they have started that
// quest, and not in the middle of a conversation or scene, nor in the opening. Misc quests (radiant,
// ambient) are not shared because their targets differ per game. Main thread only.
namespace QuestSync
{
	// A stage from another player (or from the session, after joining or loading): waits its turn.
	void Apply(const Protocol::QuestStage& a_stage);

	// The stages from other players still waiting for this quest, ascending.
	[[nodiscard]] std::vector<std::uint16_t> Waiting(std::uint32_t a_quest);

	// The player has started this quest: one of its objectives has been shown. (A quest can be
	// running long before that: a new game already runs some side quests in the background.)
	[[nodiscard]] bool PlayerStarted(const RE::TESQuest* a_quest);

	// Another player completed a quest: tell the player who did it.
	void ApplyDone(const Protocol::QuestDone& a_done);

	// Looks for stages done here (twice a second) and sets the next waiting stages from other players.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forget the known stages and the waiting ones; the next Frame takes a fresh baseline (after
	// loading a save, which can move quests back; the session's stages then arrive again).
	void Rebaseline();

	// When a stage change of a quest that gives XP (shared and misc quests) was last seen here.
	[[nodiscard]] std::chrono::steady_clock::time_point LastStageChange();

	std::string Describe();
}
