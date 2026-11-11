#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Settlement attacks are the host's (common/AttackBook.h has the rules and the game's scripts behind them; docs/DESIGN.md,
// "Settlement attacks"). Looks at the random attack quests (AttackBook::ATTACKS) twice a second in a session:
// - the host's game (bSharedAttacks) reports each run of its own, as it is rolled, begins, is defended and ends, says
//   whether the settlement is the host's, answers a friend's question about one, and tells a change of owner of any
//   settlement it has spoken of;
// - a friend's game runs each of the host's attacks as a mirror (the same quest at the same settlement, started with
//   both forced as RadiantSync's starts are), defused (its done stage) as soon as it runs, so neither the game nor any
//   save of it ever holds a mirror that rolls damage, and whose own timer is pushed back so it never begins or fights on
//   its own; it stops with the host's run, its timer cancelled first. The host's attackers are the friend's to fight
//   (Spawns shares them while the host's game has them loaded), and "Defend X" is shown while they and the friend's
//   player are at the settlement. A roll of the friend's own game at a settlement the host owns, or where the host's
//   attack is, is defused and stopped, its attackers removed; one at a settlement the host's game hasn't said is the
//   host's is asked about, again every minute while it runs, and kept until the answer says otherwise.
// Out of a session too: after every load, a run a session left defused (stage 100 done, 20 not) is stopped, and the runs
// a session left behind as it ended are closed down once the world is there.
// Main thread only.
namespace SettlementAttacks
{
	// Welcomed: whether our game hosts (its attacks are the session's).
	void OnWelcome(bool a_hosting);

	// Every frame: the leftovers of a load and of a session's end closed down; in a session (a_inSession), the looks, the
	// reports, and the mirrors' and our own runs' steps.
	void Frame(bool a_inSession);

	// The server's word: the host's attack (to a friend's game), or a friend's question (to the host's game).
	void OnAttack(const Protocol::SettlementAttack& a_attack);
	// Whether a settlement is the host's (to a friend's game).
	void OnOwner(const Protocol::SettlementOwner& a_owner);

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Before every save (autosaves too): every run the host's game decides (a mirror, our own roll at the host's
	// settlement, one being closed down) takes its done stage now if it hasn't, so the save never holds one that rolls
	// damage.
	void BeforeSave();

	// A save was loaded (in a session or not): the runs are read afresh (a mirror the save held is one again while the
	// host's attack is live), and any other a session left defused is stopped.
	void OnGameLoaded();

	// The session ended: our mirrors, and our own rolls at the host's settlements, are closed down (done stage, timer
	// cancelled, stopped), in the world now or once it is back; a mirror's start still waiting is given up.
	void Reset();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// For the dev channel's `attacks`: every attack quest running here, and what the host's game said.
	[[nodiscard]] std::string List();
}
