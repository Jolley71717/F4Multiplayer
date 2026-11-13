#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// One owner per companion (protocol 65; the rules in CompanionRules.h). Reads the Followers quest's aliases (Companion,
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

	// Whether the server last said this NPC is another player's companion (the one the friend's aliases hold, or held for a
	// friend who left): a Danse at a friend's side, mirrored here, is theirs, not a Brotherhood member about to turn.
	[[nodiscard]] bool FollowsFriend(std::uint32_t a_refId);

	// A save was loaded: the aliases are reported again.
	void OnGameLoaded();

	// The session ended.
	void Reset();

	// Whether the Followers aliases can be read now: no save or load on, no loading screen, not settling after a load, our
	// player in a cell. Read otherwise, they can come back empty.
	[[nodiscard]] bool AliasesReadable();

	// Whether our Followers aliases hold the NPC now (read now; any session or none; check AliasesReadable first).
	[[nodiscard]] bool Follows(std::uint32_t a_refId);

	// Whether the NPC is our player's companion: our aliases hold them, and the server's book doesn't say they are another's.
	[[nodiscard]] bool Ours(std::uint32_t a_refId);

	// The companion following our player now (the Followers quest's Companion alias; not Dogmeat), or null. Main thread.
	// QuestSync: a friend's kick-out that dismisses them (StageRules::DISMISSES) warns the player once before it lands.
	[[nodiscard]] RE::Actor* Following();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// For the dev channel's `companions`: what the aliases hold, whose the server says each is, and what waits.
	[[nodiscard]] std::string List();
}
