#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

// A magic effect's queued hit-effect refresh that reaches a freed effect crashed the game (2026-11-22, the host, twice in
// two tries: Nuka-World's RobCo Battlezone robots fighting a friend's stand-in, which they killed; the stand-in was
// replaced, and the game aborted on a pure virtual call on a job thread the same second). Read from disk in 1.11.240:
// - An effect that shows hit-effect art (a shader or art on its target) refreshes that art through the task queue: 2226031
//   (+B76400) checks the effect's hit-effect controller (ActiveEffect+0x10, its own vtable at +0x60 and +0x68) and hands
//   the effect itself to the task queue's message 0x0F (2229195, +C46060). That message keeps the effect as a bare
//   pointer: no reference is taken (the queue's other messages that keep an object take one: lock inc), and no handle.
// - The task queue runs it later, on a job thread (UpdateNonRenderSafeAITasks): its unpack (2229323, +C4D320) calls
//   2226006 (+B74950) with the effect at +0x92C, which frees the effect's hit-effect list and calls the controller's
//   vtable at +0x48 and +0x58 (2226586, +B94390).
// - An actor's effects live in its AI process (AIProcess::GetActiveEffectList, the middle-high data), and
//   TESObjectREFR::Disable deletes an actor's AI process at once (CombatReaction.h), its effects with it. An effect's
//   destructor leaves its controller on the base class's vtable, whose +0x58 is a pure virtual: a refresh queued before the
//   disable and run after it ends in abort() ("pure virtual call"), as both crash reports show (+B943F9 under +B74A4C
//   under +C4DC51). A stand-in is disabled when it is replaced (RemotePlayers' DeleteActor), often in the middle of a fight.
// So the unpack's call goes through EffectTasks' hook, which runs the refresh only for an effect that could have it:
// - not one of the effects an actor of ours had when we disabled it (Retired: kept KEEP, long past the queue's next run,
//   and never read through), and
// - one whose controller still has its own vtable (ActiveEffectReferenceEffectController's): one destroyed since by any
//   other way (the engine's own, another plugin's) is skipped too. Reading it is no riskier than the engine's call, which
//   reads the effect at once.
// Kept free of the game so it can be tested; EffectTasks.cpp reads the facts and puts it to the engine.
namespace EffectTask
{
	// The task queue's unpack, and its call of the hit-effect refresh for message 0x0F (E8 rel32).
	inline constexpr std::size_t TASK_CALL = 0x92C;
	// The effect's hit-effect controller, whose vtable the refresh calls.
	inline constexpr std::size_t CONTROLLER = 0x10;

	// How long an effect of an actor we disabled is kept as gone: the queue runs every frame (the rig's one caught refresh
	// came 18 ms after the disable), and the engine soon gives a freed effect's address to a new one, whose art a longer
	// hold would leave stale.
	inline constexpr auto KEEP = std::chrono::seconds(1);

	// The first few skips are logged; the rest are only counted.
	inline constexpr std::uint32_t LOGGED = 5;

	struct Effect
	{
		bool present = true;     // the task gave an effect
		bool retired = false;    // one an actor of ours had when we disabled it
		bool controller = true;  // its controller has its own vtable (only read when not retired)
	};

	enum class Verdict
	{
		kRun,        // the engine's refresh runs
		kNoEffect,   // skipped: no effect
		kRetired,    // skipped: its actor was disabled by us since it was queued
		kDestroyed,  // skipped: the effect was destroyed since (its controller is on the base class's vtable)
	};

	constexpr Verdict Judge(const Effect& a_effect)
	{
		if (!a_effect.present) {
			return Verdict::kNoEffect;
		}
		if (a_effect.retired) {
			return Verdict::kRetired;
		}
		if (!a_effect.controller) {
			return Verdict::kDestroyed;
		}
		return Verdict::kRun;
	}

	constexpr std::string_view Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kRun:
			return "runs";
		case Verdict::kNoEffect:
			return "no effect";
		case Verdict::kRetired:
			return "its actor was disabled since";
		case Verdict::kDestroyed:
			return "the effect was destroyed since";
		}
		return "?";
	}

	// The effects of actors we disabled, by address, each kept KEEP from its retirement. An address the engine gives a new
	// effect within that time has its refresh skipped too: the art of that one effect is not refreshed once, nothing more.
	class Retired
	{
	public:
		using Clock = std::chrono::steady_clock;

		void Add(std::uintptr_t a_effect, Clock::time_point a_now)
		{
			if (a_effect) {
				until[a_effect] = a_now + KEEP;
			}
		}

		[[nodiscard]] bool Has(std::uintptr_t a_effect, Clock::time_point a_now) const
		{
			const auto it = until.find(a_effect);
			return it != until.end() && a_now < it->second;
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(until, [&](const auto& a_entry) { return a_now >= a_entry.second; });
		}

		[[nodiscard]] std::size_t Size() const { return until.size(); }

	private:
		std::unordered_map<std::uintptr_t, Clock::time_point> until;
	};
}
