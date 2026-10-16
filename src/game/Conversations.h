#pragma once

#include "Protocol.h"

// Shared conversations: lines spoken by NPCs we run, by the NPC we're talking to, and by the
// local player are reported, so friends nearby see them ("Name: line") even though dialogue
// plays in only one game. Main thread only.
namespace Conversations
{
	// Watches who is talking and queues new lines (only while a_connected), and ends a conversation
	// left without its speaker (Dialogue::Frame). Call every frame.
	void Frame(bool a_connected);

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Someone said something in another player's game: shown if we're close to the speaker.
	void Apply(const Protocol::Line& a_line);

	void Reset();

	[[nodiscard]] std::string Describe();

	// For the dev channel: the recorded line an actor is saying as we would report it (info, response,
	// voice type, the engine's own voice file for it and the one we resolve), and a recording played
	// at an actor the way a friend's line is, with the audio manager's flags and priority to try.
	[[nodiscard]] std::string DescribeLine(RE::Actor* a_actor);
	[[nodiscard]] std::string PlayLine(RE::Actor* a_actor, std::uint32_t a_info, std::uint8_t a_response, std::uint32_t a_voiceType, std::uint32_t a_flags,
		std::uint8_t a_priority);
}
