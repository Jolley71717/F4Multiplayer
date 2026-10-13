#pragma once

#include "Protocol.h"

#include <cstdint>
#include <unordered_set>

// Which of a session's container changes our world has (WorldSync). The server numbers them in
// session order and echoes each to its maker too, so a game knows exactly where its world stands:
// everything before Next() is in it already (or waiting for its container to load). That belongs to
// the world rather than the connection, so it is kept in the save, and after a load the server is
// asked for what came since. Our own changes are in our world already when they come back: live,
// they happened here; in a resend after a load, our game no longer has what they moved (we loaded a
// save from before them), so replaying them would take loot we don't have, or copy what we put in.
// Free of the game so the tests' players keep their worlds by it too.
class ContainerLedger
{
public:
	enum class Offer
	{
		kHave,   // before Next(): our world has it already
		kOurs,   // one of ours, new to the count: nothing to apply
		kApply,  // someone else's: apply it to our world
	};

	// Welcomed into a_session as a_playerId. True when that is another session than the one our world
	// has changes of: none of its changes are in our world yet (whatever waits for its container goes too).
	bool Welcome(std::uint64_t a_session, std::uint32_t a_playerId)
	{
		const bool fresh = a_session != session;
		if (fresh) {
			session = a_session;
			next = 0;
			ownIds.clear();
		}
		// A reconnect normally gets the old ID back (the server knows us by Protocol::Hello::identity),
		// but not while the server still holds the old connection; our changes under either ID are ours.
		ownIds.insert(a_playerId);
		return fresh;
	}

	// Change a_index, made by player a_playerId, reached us (live or in a world state).
	Offer Take(std::uint32_t a_index, std::uint32_t a_playerId)
	{
		if (a_index < next) {
			return Offer::kHave;
		}
		next = a_index + 1;
		return Ours(a_playerId) ? Offer::kOurs : Offer::kApply;
	}

	[[nodiscard]] bool Ours(std::uint32_t a_playerId) const { return ownIds.contains(a_playerId); }

	// What to ask the server for (in a Hello, or after loading a save).
	[[nodiscard]] Protocol::WorldRequest ResyncPoint() const { return { session, next }; }

	// A save's record of where its world stood.
	void Restore(std::uint64_t a_session, std::uint32_t a_next)
	{
		session = a_session;
		next = a_next;
	}

	// A save with no such record: its world has none of any session's changes.
	void Revert()
	{
		session = 0;
		next = 0;
	}

	[[nodiscard]] std::uint64_t Session() const { return session; }
	[[nodiscard]] std::uint32_t Next() const { return next; }

private:
	std::uint64_t                     session = 0;
	std::uint32_t                     next = 0;
	std::unordered_set<std::uint32_t> ownIds;  // our player IDs in this session
};
