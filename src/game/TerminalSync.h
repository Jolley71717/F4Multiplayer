#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Choices our player makes at a quest's terminals, which no stage carries (TerminalRules.h: Easy City Downs' race): sent
// to the friends, and theirs made here by calling the quest script's function the terminal calls. Main thread only.
namespace TerminalSync
{
	// Reads the rows' properties (a few times a second) and makes friends' choices that are due. While in a session.
	void Frame();

	// A friend's choice, or the session's latest of a row (kTerminalChoice).
	void Apply(const Protocol::TerminalChoice& a_choice);

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// A save was loaded: our readings start afresh, and a friend's latest of a row is made again under the same rules.
	void OnGameLoaded();

	// The session ended: nothing of it is kept.
	void Reset();

	std::string Describe();
}
