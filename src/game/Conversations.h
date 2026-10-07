#pragma once

#include "Protocol.h"

// Shared conversations: lines spoken by NPCs we run, by the NPC we're talking to, and by the
// local player are reported, so friends nearby see them ("Name: line") even though dialogue
// plays in only one game. Main thread only.
namespace Conversations
{
	// Watches who is talking and queues new lines (only while a_connected). Call every frame.
	void Frame(bool a_connected);

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Someone said something in another player's game: shown if we're close to the speaker.
	void Apply(const Protocol::Line& a_line);

	void Reset();

	[[nodiscard]] std::string Describe();
}
