// Which queued hit reactions run (CombatReaction::Judge, as CombatReactions' hook on the task queue's call asks it): the
// engine's reaction hands the victim's AI process to AIProcess::EnterCombat unchecked, so a victim disabled since the
// hit (a stand-in replaced in a fight) crashed the game.

#include "Test.h"

#include "game/CombatReaction.h"
#include "game/TextureFlush.h"

#include <array>
#include <cstdint>
#include <initializer_list>

namespace
{
	using CombatReaction::Verdict;
	using CombatReaction::Victim;
}

TEST("combat reaction: an enabled actor with its AI process reacts, as the engine has it")
{
	CHECK(CombatReaction::Judge(Victim{}) == Verdict::kReact);
}

TEST("combat reaction: a stand-in disabled since the hit (its process deleted with it) is skipped, not handed to EnterCombat")
{
	CHECK(CombatReaction::Judge({ .disabled = true, .process = false }) == Verdict::kDisabled);
	CHECK(CombatReaction::Judge({ .disabled = true, .process = true }) == Verdict::kDisabled);  // off is off, whatever is left of it
}

TEST("combat reaction: an enabled actor with no AI process is skipped (the reaction's other caller returns at once for one)")
{
	CHECK(CombatReaction::Judge({ .process = false }) == Verdict::kNoProcess);
}

TEST("combat reaction: a deleted victim and no victim at all are skipped")
{
	CHECK(CombatReaction::Judge({ .deleted = true }) == Verdict::kDeleted);
	CHECK(CombatReaction::Judge({ .deleted = true, .disabled = true, .process = false }) == Verdict::kDeleted);
	CHECK(CombatReaction::Judge({ .present = false }) == Verdict::kNoVictim);
	CHECK(CombatReaction::Judge({ .present = false, .process = false }) == Verdict::kNoVictim);
}

TEST("combat reaction: every verdict has a name for the log")
{
	for (const auto verdict : { Verdict::kReact, Verdict::kNoVictim, Verdict::kDeleted, Verdict::kDisabled, Verdict::kNoProcess }) {
		CHECK(CombatReaction::Name(verdict) != "?");
	}
}

TEST("combat reaction: the task queue's call, as 1.11.240 has it, is read as a call of the reaction")
{
	// +C4D320 + 0x2DAA = +C500CA: E8 A1 6E 08 00, a call of +CD6F70 (the victim's reaction to an attacker).
	constexpr std::uintptr_t BASE = 0x140000000;
	constexpr std::uintptr_t SITE = BASE + 0xC4D320 + CombatReaction::TASK_CALL;
	static_assert(SITE == BASE + 0xC500CA);
	constexpr std::array<std::uint8_t, 5> CALL{ 0xE8, 0xA1, 0x6E, 0x08, 0x00 };
	CHECK(TextureFlush::CallTarget(CALL, SITE) == BASE + 0xCD6F70);
	// A jump there (another plugin's hook) is not a call: the hook stays out.
	constexpr std::array<std::uint8_t, 5> JUMP{ 0xE9, 0xA1, 0x6E, 0x08, 0x00 };
	CHECK(!TextureFlush::CallTarget(JUMP, SITE));
}
