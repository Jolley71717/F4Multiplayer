#pragma once

#include "Protocol.h"

// Quest progress: each stage a side quest (or, with a shared story, a story or faction quest) gets
// done in one player's game is set in the others' games too, in the order it was done, once they have
// started that quest, and not in the middle of a conversation or scene, nor in the opening. A stage
// that would move the player away, take their caps or move their own story on is left out (see
// StageRules). Misc quests (radiant, ambient) are not shared because their targets differ per game.
// Main thread only.
namespace QuestSync
{
	// A stage from another player: live (they just did it), or from the session after joining or
	// loading. Waits its turn; some aren't set at all (see StageRules).
	void Apply(const Protocol::QuestStage& a_stage, bool a_live);

	// The stages from other players still waiting for this quest, ascending.
	[[nodiscard]] std::vector<std::uint16_t> Waiting(std::uint32_t a_quest);

	// The player has started this quest: one of its objectives has been shown. (A quest can be
	// running long before that: a new game already runs some side quests in the background.)
	[[nodiscard]] bool PlayerStarted(const RE::TESQuest* a_quest);

	// Another player completed a quest: tell the player who did it.
	void ApplyDone(const Protocol::QuestDone& a_done);

	// Looks for stages done here (twice a second) and, during a session, sets the next waiting stages
	// from other players.
	void Frame(bool a_inSession);

	// The reports to send (a_sending: welcomed), a few hundred a second at most. While joining or
	// rejoining a session (a_inSession) they are kept for when the connection is back, since a stage is
	// reported only once, the story's too (sent if the welcome shares the story); playing alone, or after
	// minutes of trying a host who doesn't answer, they are dropped (see StageLog::Outbox).
	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending, bool a_inSession);

	// The server address changed (a friend's invite accepted, hosting started or stopped, leaving):
	// nothing kept for the last session is sent to the next.
	void OnServerChanged();

	// The session ended: the stages still waiting from it are dropped, not set in the player's own
	// game, except those that give the player their controls back after a stage set here took them
	// (set even though the story is the player's own again). What other players moved lately goes too.
	void OnSessionEnded();

	// Forget the known stages and the waiting ones; the next Frame takes a fresh baseline (after
	// loading a save, which can move quests back; the session's stages then arrive again).
	void Rebaseline();

	// When a stage change of a quest that gives XP (shared and misc quests) was last seen here.
	[[nodiscard]] std::chrono::steady_clock::time_point LastStageChange();

	std::string Describe();
}
