#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Shared quest rewards (common/RewardBook.h): on a shared quest, both players get the full reward when either turns it in.
// A stage's own script already runs in every game the stage is set in; this gives friends what never reaches them that
// way: a dialogue line's reward (RewardLines.h) and a turn-in stage's that their games skip (a StageRules kPays stage),
// with its quest XP. Our game claims what our player got from those and reports it; a friend's reward the server sends
// (kQuestReward) is given here once, unless our player got the same in their own game. Main thread only (the container
// and activation sinks queue under a lock).
namespace QuestRewards
{
	// Registers the event sinks (once, with the session's).
	void Install();

	// Our moments (the line our player hears, the stages seen done), the claims, and a friend's rewards due.
	void Frame(bool a_inSession);

	// A stage of a quest the session shares, done here by our player (not a friend's set here), seen now: a turn-in if
	// its StageRules row is a payment (kPays), which the friends' games never set.
	void OnOwnStage(std::uint32_t a_quest, std::uint16_t a_stage, std::uint8_t a_ruleFlags);

	// XP our player gained that is their own (Party's: not a friend's share given here, nor ours).
	void OnOwnXp(float a_xp);

	// A friend's reward from the server.
	void OnReward(const Protocol::QuestReward& a_reward);

	// The reports to send (a_sending: welcomed); kept, a few dozen at most, until then.
	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending);

	// The session ended (or the connection dropped): moments and gains still open go; friends' rewards not yet given here stay
	// (they are our player's), as do our reports not yet sent (a reconnect sends them).
	void OnSessionEnded();

	// Another server: its ids are new, and nothing of ours is for it.
	void OnServerChanged();

	// For `net status`.
	[[nodiscard]] std::string Describe();
}
