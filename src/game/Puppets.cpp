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
			std::uint16_t                         moveMode = 0;
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
			std::to_underlying(RE::Actor::BOOL_FLAGS::kCastingDisabled) |
			std::to_underlying(RE::Actor::BOOL_FLAGS::kEssential);

		void SuppressBehavior(RE::Actor* a_actor)
		{
			a_actor->boolFlags.set(static_cast<RE::Actor::BOOL_FLAGS>(SUPPRESSED_BEHAVIOR));
		}

		// Damage to a stand-in is meaningless (the real player is elsewhere), and a dying or
		// staggered puppet fights the forced transform, so keep it at full health.
		void KeepHealthy(RE::Actor* a_actor)
		{
			const auto values = RE::ActorValue::GetSingleton();
			if (values && values->health) {
				static_cast<RE::ActorValueOwner*>(a_actor)->RestoreActorValue(*values->health, 100000.0f);
			}
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
			KeepHealthy(a_this);
			if (puppet->hasTarget) {
				a_this->SetPosition(puppet->position, true);
				a_this->SetHeading(puppet->heading);
				static_cast<RE::ActorState&>(*a_this).moveMode = puppet->moveMode;
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

		// Activation (pressing E) is handled by the base form. Puppets must not start the
		// base NPC's dialogue or show an activation prompt.
		constexpr std::size_t ACTIVATE_INDEX = 0x40;       // TESForm::Activate
		constexpr std::size_t ACTIVATE_TEXT_INDEX = 0x60;  // TESBoundObject::GetActivateText

		using ActivateFn = bool (*)(RE::TESForm*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::TESBoundObject*, std::int32_t);
		using ActivateTextFn = bool (*)(RE::TESBoundObject*, RE::TESObjectREFR*, RE::BSString&);
		std::uintptr_t hookedBaseVtable = 0;
		ActivateFn     originalActivate = nullptr;
		ActivateTextFn originalActivateText = nullptr;

		bool IsPuppetRef(RE::TESObjectREFR* a_ref)
		{
			const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return actor && IsPuppet(actor);
		}

		bool ActivateHook(RE::TESForm* a_this, RE::TESObjectREFR* a_itemActivated, RE::TESObjectREFR* a_actionRef, RE::TESBoundObject* a_objectToGet, std::int32_t a_count)
		{
			if (IsPuppetRef(a_itemActivated)) {
				return false;
			}
			return originalActivate(a_this, a_itemActivated, a_actionRef, a_objectToGet, a_count);
		}

		bool ActivateTextHook(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_itemActivated, RE::BSString& a_result)
		{
			if (IsPuppetRef(a_itemActivated)) {
				return false;
			}
			return originalActivateText(a_this, a_itemActivated, a_result);
		}

		void InstallActivationHooks(RE::Actor* a_actor)
		{
			const auto base = a_actor->GetObjectReference();
			if (!base) {
				return;
			}
			const auto vtable = *reinterpret_cast<std::uintptr_t*>(base);
			if (hookedBaseVtable == vtable) {
				return;
			}
			if (hookedBaseVtable != 0) {
				REX::WARN("Puppets: base form of {:08X} uses an unhooked vtable", a_actor->GetFormID());
				return;
			}

			const auto activateSlot = vtable + sizeof(void*) * ACTIVATE_INDEX;
			const auto textSlot = vtable + sizeof(void*) * ACTIVATE_TEXT_INDEX;
			originalActivate = *reinterpret_cast<ActivateFn*>(activateSlot);
			originalActivateText = *reinterpret_cast<ActivateTextFn*>(textSlot);
			REL::WriteSafeData(activateSlot, reinterpret_cast<std::uintptr_t>(&ActivateHook));
			REL::WriteSafeData(textSlot, reinterpret_cast<std::uintptr_t>(&ActivateTextHook));
			hookedBaseVtable = vtable;
			REX::INFO("Puppets: hooked NPC activation (vtable {:X})", vtable);
		}
	}

	void Register(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		InstallHook(a_actor);
		InstallActivationHooks(a_actor);
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

	void SetTarget(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_heading, float a_speed, float a_direction, std::uint16_t a_moveMode)
	{
		std::scoped_lock l{ lock };
		if (const auto it = puppets.find(a_actor); it != puppets.end()) {
			it->second.position = a_position;
			it->second.heading = a_heading;
			it->second.speed = a_speed;
			it->second.direction = a_direction;
			it->second.moveMode = a_moveMode;
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
