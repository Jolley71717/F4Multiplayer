#pragma once

#include "Protocol.h"

// Shared time of day and weather. Every player reports theirs; the server passes on only the
// first player's (the host's), and everyone else follows it. Main thread only.
namespace WorldClock
{
	// Reports our time and weather every few seconds. Call every frame while in a session.
	void Frame();

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	void Apply(const Protocol::WorldTime& a_time);

	void Reset();

	[[nodiscard]] std::string Describe();
}
