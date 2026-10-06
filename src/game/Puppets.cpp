#include "game/Puppets.h"

namespace Puppets
{
	namespace
	{
		// Actor::Update vtable slot (see RE/A/Actor.h).
		constexpr std::size_t UPDATE_INDEX = 0xCF;

		// The engine periodically re-evaluates AI packages, so the idle package is re-applied.
		constexpr auto IDLE_REFRESH = 2s;

		// Locomotion starts above MOVE_START and stops below MOVE_STOP (units/second).
		constexpr float MOVE_START = 25.0f;
		constexpr float MOVE_STOP = 10.0f;

		struct Puppet
		{
			RE::NiPoint3                          position;
			float                                 heading = 0.0f;
			float                                 speed = 0.0f;
			float                                 direction = 0.0f;
			bool                                  hasTarget = false;
			bool                                  moving = false;  // locomotion graph is in its moving state
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

		// With AI off nothing feeds the locomotion graph, so we do: Speed/Direction every
		// frame, plus moveStart/moveStop events when the remote player starts or stops.
		void DriveLocomotion(RE::Actor* a_actor, const Puppet& a_puppet)
		{
			static const RE::BSFixedString speedVar{ "Speed" };
			static const RE::BSFixedString directionVar{ "Direction" };
			static const RE::BSFixedString moveStart{ "moveStart" };
			static const RE::BSFixedString moveStop{ "moveStop" };

			const auto graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);

			bool moving = a_puppet.moving;
			if (!moving && a_puppet.speed > MOVE_START) {
				moving = graph->NotifyAnimationGraphImpl(moveStart);
			} else if (moving && a_puppet.speed < MOVE_STOP) {
				graph->NotifyAnimationGraphImpl(moveStop);
				moving = false;
			}

			graph->SetGraphVariableFloat(speedVar, moving ? a_puppet.speed : 0.0f);
			graph->SetGraphVariableFloat(directionVar, a_puppet.direction);

			if (moving != a_puppet.moving) {
				std::scoped_lock l{ lock };
				if (const auto it = puppets.find(a_actor); it != puppets.end()) {
					it->second.moving = moving;
				}
			}
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
				DriveLocomotion(a_this, *puppet);
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

	void SetTarget(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_heading, float a_speed, float a_direction)
	{
		std::scoped_lock l{ lock };
		if (const auto it = puppets.find(a_actor); it != puppets.end()) {
			it->second.position = a_position;
			it->second.heading = a_heading;
			it->second.speed = a_speed;
			it->second.direction = a_direction;
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
