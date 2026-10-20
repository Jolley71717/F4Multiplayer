#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiPoint3;
	class TESObjectREFR;
}

// World traps shared (rules in src/game/TrapRules.h): a mine, a tripwire, a tension trigger, a trap (a grenade bouquet, a
// gun trap, an IED...) or a Far Harbor bear trap that went off, was disarmed or was taken in one game is spent in every
// game, and stays spent for the session: through a friend coming by later, and a reload.
//
// Our game watches the traps loaded around our player, four times a second: a scripted one by its script's state, a mine
// by whether it is still there. One seen armed that went off, was disarmed or went (a mine picked up, a bear trap taken)
// is reported (kReportTrap); the server keeps the session's list (kTraps), passes each report on at once, and sends the
// list to whoever joins or loads. What it holds of a loaded trap still armed here is done to it: set off as it went off
// there (a mine's blast, a tripwire's trip, which sets off what it is linked to), when it went off in a friend's game just
// now where it is in ours; otherwise spent quietly, through its own script (never its disarm, which rewards whoever ran
// it). A trap's press is not replayed by the presser's stand-in (WorldSync's activation sharing): its state is shared.
//
// Main thread only.
namespace Traps
{
	// Every frame while welcomed, in the world and out of the opening.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// From the server: the session's spent traps (a world state's list), or one spent just now (live).
	void Apply(const Protocol::TrapStates& a_traps);

	// Our player activated a reference (WorldSync's activation watch): a mine going a moment later was picked up.
	void OnPlayerActivated(std::uint32_t a_ref);

	// Whether a loaded reference is a scripted trap (not a mine: a mine is no activator).
	bool IsTrap(RE::TESObjectREFR* a_ref);

	// Whether an explosion here is a trap's (WeaponFire's explosion watch leaves it out: every game sets off its own).
	bool Claims(const RE::NiPoint3& a_at);

	// A save was loaded: our traps are the save's (the session's list stays, and is applied again).
	void OnGameLoaded();

	// The session ended.
	void Reset();

	std::string Describe();

	// For the dev commands: the traps we watch, one line each.
	std::string List();
}
