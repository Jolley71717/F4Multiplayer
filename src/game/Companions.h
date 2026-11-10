#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// One owner per companion (protocol 64; the rules in CompanionRules.h). Reads the Followers quest's aliases (Companion,
// DogmeatCompanion) a few times a second, reports what they hold to the server, and, when the server says the NPC is
// another player's, dismisses our copy through the Followers script's quiet dismissal (no message, no settlement menu)
// once our player is out of its dialogue, with a notice naming whose it is. Main thread only.
namespace Companions
{
	// While in a session: the aliases' contents, the reports, and a dismissal that is due.
	void Frame();

	// The server's word on an NPC (kCompanionOwner).
	void OnOwner(const Protocol::CompanionOwner& a_owner);

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// A save was loaded: the aliases are reported again.
	void OnGameLoaded();

	// The session ended.
	void Reset();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// For the dev channel's `companions`: what the aliases hold, whose the server says each is, and what waits.
	[[nodiscard]] std::string List();
}
