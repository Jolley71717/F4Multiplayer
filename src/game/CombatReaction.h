#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// A hit's reaction that reaches an actor with no AI process crashed the game (2026-11-03, a client, Nuka-World raiders
// firing at a friend's stand-in). Read from disk in 1.11.240:
// - The victim's reaction to an attacker (Address Library ID 2231180, +CD6F70: victim, attacker, the weapon's instance,
//   and a fourth word) checks the attacker's AI process (Actor::currentProcess, +300) but not the victim's: it reads the
//   victim's at +0xDA and, when the victim is to fight back, hands it to AIProcess::EnterCombat (2231840, +D00F80) at
//   +0x5D7 as its this, whose first instruction reads it (cmp [rcx+10h],0): with none, an access violation. On the way
//   it also calls the victim's own combat and alarm functions, which may read the process too.
// - It runs from a queued task: the task queue's unpack (2229323, +C4D320) turns the task's two handles back into the
//   victim and the attacker and calls the reaction at +0x2DAA. A handle to a disabled reference still resolves, and
//   TESObjectREFR::Disable deletes an actor's AI process at once (currentProcess null), so a reaction queued by a hit
//   and run after the victim was disabled crashes. A stand-in is disabled when it is replaced (RemotePlayers' DeleteActor,
//   Bury, Disable: a new face, a place made again, a respawn in range), often in the middle of a fight.
// - The reaction's other caller (2231148, +CD2CA0, at +0x2298) runs as the hit is handled and returns at once, at its
//   entry, when the victim has no process: the engine's own rule. Only the task's call is hooked.
// So the task's call goes through CombatReactions' hook, which lets the reaction run only for a victim that could have
// it: not deleted, not disabled, with its process. Anything else is skipped and counted. Kept free of the game so it
// can be tested; CombatReactions.cpp reads the facts and puts it to the engine.
namespace CombatReaction
{
	// The task queue's unpack, and its call of the reaction (E8 rel32).
	inline constexpr std::size_t TASK_CALL = 0x2DAA;

	// The first few skips are logged (with the victim and the attacker); the rest are only counted.
	inline constexpr std::uint32_t LOGGED = 5;

	struct Victim
	{
		bool present = true;   // the task's handle gave an actor
		bool deleted = false;  // its form is deleted
		bool disabled = false;
		bool process = true;   // Actor::currentProcess is set
	};

	enum class Verdict
	{
		kReact,      // the engine's reaction runs
		kNoVictim,   // skipped: no actor
		kDeleted,    // skipped: the victim is deleted
		kDisabled,   // skipped: the victim is disabled (its process was deleted with it)
		kNoProcess,  // skipped: the victim has no AI process
	};

	constexpr Verdict Judge(const Victim& a_victim)
	{
		if (!a_victim.present) {
			return Verdict::kNoVictim;
		}
		if (a_victim.deleted) {
			return Verdict::kDeleted;
		}
		if (a_victim.disabled) {
			return Verdict::kDisabled;
		}
		if (!a_victim.process) {
			return Verdict::kNoProcess;
		}
		return Verdict::kReact;
	}

	constexpr std::string_view Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kReact:
			return "reacts";
		case Verdict::kNoVictim:
			return "no victim";
		case Verdict::kDeleted:
			return "deleted";
		case Verdict::kDisabled:
			return "disabled";
		case Verdict::kNoProcess:
			return "no AI process";
		}
		return "?";
	}
}
