#include "game/CombatReactions.h"

#include "game/CombatReaction.h"
#include "game/TextureFlush.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>

namespace CombatReactions
{
	namespace
	{
		// 1.11.240, by Address Library ID (CombatReaction.h: how each was read).
		constexpr REL::ID     TASK_UNPACK{ 2229323 };  // the task queue's unpack (+C4D320): resolves a task's handles, calls the reaction at +0x2DAA
		constexpr REL::ID     REACT{ 2231180 };        // the victim's reaction to an attacker (+CD6F70): (Actor* victim, Actor* attacker, const BGSObjectInstance*, a fourth word)
		constexpr std::size_t CALL_STUB = 14;          // what a write_call<5> takes of the trampoline

		using ReactFn = void (*)(RE::Actor*, RE::Actor*, const void*, std::uintptr_t);
		ReactFn originalReact = nullptr;

		// The hook runs on whichever thread runs the task queue (a job thread in UpdateNonRenderSafeAITasks).
		std::atomic<bool>          installed{ false };
		std::atomic<std::uint32_t> skipped{ 0 };

		CombatReaction::Victim FactsOf(RE::Actor* a_victim)
		{
			if (!a_victim) {
				return { .present = false };
			}
			return { .deleted = a_victim->IsDeleted(), .disabled = a_victim->IsDisabled(), .process = a_victim->currentProcess != nullptr };
		}

		void ReactHook(RE::Actor* a_victim, RE::Actor* a_attacker, const void* a_weapon, std::uintptr_t a_extra)
		{
			const auto verdict = CombatReaction::Judge(FactsOf(a_victim));
			if (verdict == CombatReaction::Verdict::kReact) {
				originalReact(a_victim, a_attacker, a_weapon, a_extra);
				return;
			}
			const auto count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= CombatReaction::LOGGED) {
				REX::WARN("CombatReactions: {:08X}'s queued reaction to {:08X}'s hit skipped: {}{}", a_victim ? a_victim->GetFormID() : 0, a_attacker ? a_attacker->GetFormID() : 0,
					CombatReaction::Name(verdict), count == CombatReaction::LOGGED ? " (the last one logged; net status counts them)" : "");
			}
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("CombatReactions: no room for the hook ({} bytes free); a hit reaction queued for an actor disabled since can still crash the game", trampoline.free_size());
			return false;
		}
		const auto                  site = TASK_UNPACK.address() + CombatReaction::TASK_CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != REACT.address()) {
			REX::WARN("CombatReactions: the task queue doesn't call the victim's reaction where 1.11.240 does (another plugin's patch?); left as it is");
			return false;
		}
		originalReact = reinterpret_cast<ReactFn>(trampoline.write_call<5>(site, &ReactHook));
		installed = true;
		REX::INFO("CombatReactions: the task queue's hit reactions (+{:X}+{:X}) skip a victim that is disabled, deleted or has no AI process", TASK_UNPACK.offset(), CombatReaction::TASK_CALL);
		return true;
	}

	std::string Describe()
	{
		return std::format("reactionGuard={} reactionsSkipped={}", installed ? "on" : "off", skipped.load(std::memory_order_relaxed));
	}
}
