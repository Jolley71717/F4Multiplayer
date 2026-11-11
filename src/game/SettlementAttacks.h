#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Settlement attacks are the host's (common/AttackBook.h has the rules and the game's scripts behind them; docs/DESIGN.md,
// "Settlement attacks"). Looks at the random attack quests (AttackBook::ATTACKS) twice a second in a session:
// - the host's game (bSharedAttacks) reports each run of its own, as it is rolled, begins, is defended and ends, says
//   whether the settlement is the host's, and answers a friend's question about one;
// - a friend's game runs each of the host's attacks as a mirror (the same quest at the same settlement, started with
//   both forced as RadiantSync's starts are), whose own timer is pushed back so it never begins, fights or resolves on
//   its own: its done stage is set as the host's attackers die or as the host's run ends, so its shutdown stage never
//   rolls damage, and it stops with the host's run. The host's attackers are the friend's to fight (Spawns shares them
//   while the host's game has them loaded). A roll of the friend's own game at a settlement the host owns, or where the
//   host's attack is, is defused (its done stage) and stopped, its attackers removed; one at a settlement the host's
//   game hasn't spoken of is asked about first, and kept until the answer says otherwise.
// Main thread only.
namespace SettlementAttacks
{
	// Welcomed: whether our game hosts (its attacks are the session's).
	void OnWelcome(bool a_hosting);

	// While in a session: the looks, the reports, and the mirrors' and our own runs' steps.
	void Frame();

	// The server's word: the host's attack (to a friend's game), or a friend's question (to the host's game).
	void OnAttack(const Protocol::SettlementAttack& a_attack);
	// Whether a settlement is the host's (to a friend's game).
	void OnOwner(const Protocol::SettlementOwner& a_owner);

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// A save was loaded: the runs are read afresh (a mirror the save held is one again while the host's attack is live).
	void OnGameLoaded();

	// The session ended: our mirrors end here, defused (done stage, then stopped), so no attack of the host's is left
	// running in our save.
	void Reset();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// For the dev channel's `attacks`: every attack quest running here, and what the host's game said.
	[[nodiscard]] std::string List();
}
