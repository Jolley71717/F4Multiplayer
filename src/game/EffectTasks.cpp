#include "game/EffectTasks.h"

#include "game/EffectTask.h"
#include "game/TextureFlush.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace EffectTasks
{
	namespace
	{
		// 1.11.240, by Address Library ID (EffectTask.h: how each was read).
		constexpr REL::ID     TASK_UNPACK{ 2229323 };  // the task queue's unpack (+C4D320): calls the refresh for message 0x0F at +0x92C
		constexpr REL::ID     REFRESH{ 2226006 };      // the effect's hit-effect refresh (+B74950): (ActiveEffect*)
		constexpr std::size_t CALL_STUB = 14;          // what a write_call<5> takes of the trampoline

		using RefreshFn = void (*)(RE::ActiveEffect*);
		RefreshFn originalRefresh = nullptr;

		// The hook runs on whichever thread runs the task queue (a job thread); Retire on the main thread.
		std::atomic<bool>          installed{ false };
		std::atomic<std::uint32_t> skipped{ 0 };
		std::atomic<std::uint32_t> retiredCount{ 0 };
		std::uintptr_t             controllerVtable = 0;  // ActiveEffectReferenceEffectController's, set before the hook goes in
		std::mutex                 lock;
		EffectTask::Retired        retired;

		EffectTask::Effect FactsOf(RE::ActiveEffect* a_effect)
		{
			if (!a_effect) {
				return { .present = false };
			}
			const auto at = reinterpret_cast<std::uintptr_t>(a_effect);
			{
				std::scoped_lock l{ lock };
				if (retired.Has(at, EffectTask::Retired::Clock::now())) {
					return { .retired = true };
				}
			}
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(at + EffectTask::CONTROLLER);
			return { .controller = vtable == controllerVtable };
		}

		void RefreshHook(RE::ActiveEffect* a_effect)
		{
			const auto verdict = EffectTask::Judge(FactsOf(a_effect));
			if (verdict == EffectTask::Verdict::kRun) {
				originalRefresh(a_effect);
				return;
			}
			const auto count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= EffectTask::LOGGED) {
				REX::WARN("EffectTasks: a queued hit-effect refresh of effect {:X} skipped: {}{}", reinterpret_cast<std::uintptr_t>(a_effect), EffectTask::Name(verdict),
					count == EffectTask::LOGGED ? " (the last one logged; net status counts them)" : "");
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
			REX::WARN("EffectTasks: no room for the hook ({} bytes free); a hit-effect refresh queued for an actor disabled since can still crash the game", trampoline.free_size());
			return false;
		}
		const auto                  site = TASK_UNPACK.address() + EffectTask::TASK_CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != REFRESH.address()) {
			REX::WARN("EffectTasks: the task queue doesn't call the hit-effect refresh where 1.11.240 does (another plugin's patch?); left as it is");
			return false;
		}
		controllerVtable = RE::VTABLE::ActiveEffectReferenceEffectController[0].address();
		originalRefresh = reinterpret_cast<RefreshFn>(trampoline.write_call<5>(site, &RefreshHook));
		installed = true;
		REX::INFO("EffectTasks: the task queue's hit-effect refreshes (+{:X}+{:X}) skip an effect freed since (its actor disabled)", TASK_UNPACK.offset(), EffectTask::TASK_CALL);
		return true;
	}

	void Retire(RE::TESObjectREFR* a_ref)
	{
		const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
		if (!actor || !installed) {
			return;
		}
		const auto list = static_cast<RE::MagicTarget*>(actor)->GetActiveEffectList();
		if (!list || list->data.empty()) {
			return;
		}
		const auto     now = EffectTask::Retired::Clock::now();
		std::uint32_t  added = 0;
		std::scoped_lock l{ lock };
		retired.Prune(now);
		for (const auto& effect : list->data) {
			if (const auto e = effect.get()) {
				retired.Add(reinterpret_cast<std::uintptr_t>(e), now);
				++added;
			}
		}
		retiredCount.fetch_add(added, std::memory_order_relaxed);
	}

	std::string Describe()
	{
		return std::format("effectGuard={} effectsRetired={} effectRefreshesSkipped={}", installed ? "on" : "off", retiredCount.load(std::memory_order_relaxed),
			skipped.load(std::memory_order_relaxed));
	}
}
