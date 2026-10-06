#include "game/Puppets.h"

namespace Puppets
{
	namespace
	{
		// Actor::Update vtable slot (see RE/A/Actor.h).
		constexpr std::size_t UPDATE_INDEX = 0xCF;

		// The engine periodically re-evaluates AI packages, so the idle package is re-applied.
		constexpr auto IDLE_REFRESH = 2s;

		struct Puppet
		{
			RE::NiPoint3                          position;
			float                                 heading = 0.0f;
			bool                                  hasTarget = false;
			std::chrono::steady_clock::time_point nextIdleRefresh{};
		};

		std::mutex                                    lock;
		std::unordered_map<const RE::Actor*, Puppet> puppets;
		std::atomic<std::size_t>                      puppetCount{ 0 };

		using UpdateFn = void (*)(RE::Actor*, float);
		std::uintptr_t hookedVtable = 0;
		UpdateFn       originalUpdate = nullptr;

		constexpr auto SUPPRESSED_BEHAVIOR =
			std::to_underlying(RE::Actor::BOOL_FLAGS::kMovementBlocked) |
			std::to_underlying(RE::Actor::BOOL_FLAGS::kAttackingDisabled) |
			std::to_underlying(RE::Actor::BOOL_FLAGS::kCastingDisabled);

		void SuppressBehavior(RE::Actor* a_actor)
		{
			a_actor->boolFlags.set(static_cast<RE::Actor::BOOL_FLAGS>(SUPPRESSED_BEHAVIOR));
		}

		void UpdateHook(RE::Actor* a_this, float a_delta)
		{
			if (puppetCount.load(std::memory_order_relaxed) == 0) {
				originalUpdate(a_this, a_delta);
				return;
			}

			std::optional<Puppet> puppet;
			{
				std::scoped_lock l{ lock };
				if (const auto it = puppets.find(a_this); it != puppets.end()) {
					puppet = it->second;
				}
			}

			if (!puppet) {
				originalUpdate(a_this, a_delta);
				return;
			}

			SuppressBehavior(a_this);
			if (puppet->hasTarget) {
				a_this->SetPosition(puppet->position, true);
				a_this->SetHeading(puppet->heading);
			}
			a_this->UpdateNoAI(a_delta);
		}

		void InstallHook(RE::Actor* a_actor)
		{
			const auto vtable = *reinterpret_cast<std::uintptr_t*>(a_actor);
			if (hookedVtable == vtable) {
				return;
			}
			if (hookedVtable != 0) {
				// A different actor class (e.g. a creature) would need its own hook.
				REX::WARN("Puppets: actor {:08X} uses an unhooked vtable", a_actor->GetFormID());
				return;
			}

			const auto slot = vtable + sizeof(void*) * UPDATE_INDEX;
			originalUpdate = *reinterpret_cast<UpdateFn*>(slot);
			const auto hook = reinterpret_cast<std::uintptr_t>(&UpdateHook);
			REL::WriteSafeData(slot, hook);
			hookedVtable = vtable;
			REX::INFO("Puppets: hooked actor update (vtable {:X})", vtable);
		}
	}

	void Register(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		InstallHook(a_actor);
		SuppressBehavior(a_actor);
		a_actor->InitiateDoNothingPackage();

		std::scoped_lock l{ lock };
		puppets.try_emplace(a_actor, Puppet{ .nextIdleRefresh = std::chrono::steady_clock::now() + IDLE_REFRESH });
		puppetCount = puppets.size();
	}

	void Unregister(RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		puppets.erase(a_actor);
		puppetCount = puppets.size();
	}

	void Clear()
	{
		std::scoped_lock l{ lock };
		puppets.clear();
		puppetCount = 0;
	}

	bool IsPuppet(const RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		return puppets.contains(a_actor);
	}

	void SetTarget(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_heading)
	{
		std::scoped_lock l{ lock };
		if (const auto it = puppets.find(a_actor); it != puppets.end()) {
			it->second.position = a_position;
			it->second.heading = a_heading;
			it->second.hasTarget = true;
		}
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();

		std::vector<RE::Actor*> refresh;
		{
			std::scoped_lock l{ lock };
			for (auto& [actor, puppet] : puppets) {
				if (now >= puppet.nextIdleRefresh) {
					puppet.nextIdleRefresh = now + IDLE_REFRESH;
					refresh.push_back(const_cast<RE::Actor*>(actor));
				}
			}
		}

		for (const auto actor : refresh) {
			actor->InitiateDoNothingPackage();
		}
	}
}
