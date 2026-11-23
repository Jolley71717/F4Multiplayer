// Which queued hit-effect refreshes run (EffectTask::Judge, as EffectTasks' hook on the task queue's call asks it): the
// task queue keeps the effect as a bare pointer, and disabling an actor frees its effects with its AI process, so a
// refresh queued for a stand-in's effect and run after the stand-in was replaced crashed on a pure virtual call.

#include "Test.h"

#include "game/EffectTask.h"
#include "game/TextureFlush.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>

namespace
{
	using EffectTask::Effect;
	using EffectTask::Verdict;
	using Clock = EffectTask::Retired::Clock;
}

TEST("effect task: a live effect (its controller on its own vtable) is refreshed, as the engine has it")
{
	CHECK(EffectTask::Judge(Effect{}) == Verdict::kRun);
}

TEST("effect task: an effect of an actor we disabled since is skipped, and never read")
{
	CHECK(EffectTask::Judge({ .retired = true }) == Verdict::kRetired);
	CHECK(EffectTask::Judge({ .retired = true, .controller = false }) == Verdict::kRetired);
}

TEST("effect task: an effect destroyed some other way (its controller back on the base class's vtable) is skipped")
{
	CHECK(EffectTask::Judge({ .controller = false }) == Verdict::kDestroyed);
}

TEST("effect task: no effect is skipped")
{
	CHECK(EffectTask::Judge({ .present = false }) == Verdict::kNoEffect);
	CHECK(EffectTask::Judge({ .present = false, .retired = true }) == Verdict::kNoEffect);
}

TEST("effect task: every verdict has a name for the log")
{
	for (const auto verdict : { Verdict::kRun, Verdict::kNoEffect, Verdict::kRetired, Verdict::kDestroyed }) {
		CHECK(EffectTask::Name(verdict) != "?");
	}
}

TEST("effect task: a retired effect is kept as gone for KEEP, then let go of")
{
	EffectTask::Retired retired;
	const auto          t0 = Clock::time_point{} + std::chrono::hours(1);
	retired.Add(0x1000, t0);
	retired.Add(0, t0);  // no effect: nothing kept
	CHECK(retired.Size() == 1);
	CHECK(retired.Has(0x1000, t0));
	CHECK(retired.Has(0x1000, t0 + EffectTask::KEEP - std::chrono::milliseconds(1)));
	CHECK(!retired.Has(0x1000, t0 + EffectTask::KEEP));
	CHECK(!retired.Has(0x2000, t0));
	retired.Prune(t0 + EffectTask::KEEP - std::chrono::milliseconds(1));
	CHECK(retired.Size() == 1);
	retired.Prune(t0 + EffectTask::KEEP);
	CHECK(retired.Size() == 0);
}

TEST("effect task: retired again, an effect is kept KEEP from the last time")
{
	EffectTask::Retired retired;
	const auto          t0 = Clock::time_point{} + std::chrono::hours(1);
	retired.Add(0x1000, t0);
	retired.Add(0x1000, t0 + std::chrono::seconds(3));
	CHECK(retired.Has(0x1000, t0 + EffectTask::KEEP));
	CHECK(retired.Size() == 1);
}

TEST("effect task: the task queue's call, as 1.11.240 has it, is read as a call of the hit-effect refresh")
{
	// +C4D320 + 0x92C = +C4DC4C: E8 FF 6C F2 FF, a call of +B74950 (the effect's hit-effect refresh).
	constexpr std::uintptr_t BASE = 0x140000000;
	constexpr std::uintptr_t SITE = BASE + 0xC4D320 + EffectTask::TASK_CALL;
	static_assert(SITE == BASE + 0xC4DC4C);
	constexpr std::array<std::uint8_t, 5> CALL{ 0xE8, 0xFF, 0x6C, 0xF2, 0xFF };
	CHECK(TextureFlush::CallTarget(CALL, SITE) == BASE + 0xB74950);
	constexpr std::array<std::uint8_t, 5> JUMP{ 0xE9, 0xFF, 0x6C, 0xF2, 0xFF };
	CHECK(!TextureFlush::CallTarget(JUMP, SITE));
}
