#include "game/Puppets.h"

#include "game/IdleRelease.h"
#include "game/Prediction.h"

#include "Protocol.h"

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
			Kind                                  kind = Kind::kPlayer;
			std::uint32_t                         formId = 0;  // to tell a freed actor's address apart from a new actor
			std::uint32_t                         originalFlags = 0;  // SUPPRESSED_BEHAVIOR bits the actor had before
			Motion                                target;
			bool                                  hasTarget = false;
			bool                                  moving = false;  // locomotion graph is in its moving state
			std::uint8_t                          appliedFlags = 0;  // flags whose animation events were sent
			std::chrono::steady_clock::time_point aiUntil{};        // the engine's own update (with AI) runs until then: furniture use (power armor) needs it
			// What the stand-in actually covers per frame, smoothed: the animation runs at this speed so
			// the feet match the ground (the reported speed can lag behind).
			RE::NiPoint3                          lastTargetPosition;
			std::chrono::steady_clock::time_point lastTargetTime{};
			Prediction::SpeedWindow               measured;  // .speed is what the stand-in really covers
			std::chrono::steady_clock::time_point nextIdleRefresh{};
			std::chrono::steady_clock::time_point nextDrawAttempt{};
			bool                                  redraw = false;  // holster (if drawn) and draw again
			IdleRelease::Gate                     idleRelease;     // a mirrored NPC held by an idle (see ReleaseIdle)
			std::uint32_t                         idleStops = 0;   // stops sent, and taken by the graph (dev: mirror)
			std::uint32_t                         idleStopsTaken = 0;
		};

		std::mutex                                    lock;
		std::unordered_map<const RE::Actor*, Puppet> puppets;
		std::atomic<std::size_t>                      puppetCount{ 0 };

		// The entry for a live actor, or null. The map is keyed by address and the game reuses addresses,
		// so an entry only counts when its form ID is the actor's (lock held by the caller).
		Puppet* Find(const RE::Actor* a_actor)
		{
			const auto it = puppets.find(a_actor);
			return it != puppets.end() && it->second.formId == a_actor->GetFormID() ? &it->second : nullptr;
		}

		using UpdateFn = void (*)(RE::Actor*, float);
		std::uintptr_t hookedVtable = 0;
		UpdateFn       originalUpdate = nullptr;

		constexpr auto SUPPRESSED_BEHAVIOR =
			std::to_underlying(RE::Actor::BOOL_FLAGS::kMovementBlocked) |
			std::to_underlying(RE::Actor::BOOL_FLAGS::kAttackingDisabled) |
			std::to_underlying(RE::Actor::BOOL_FLAGS::kCastingDisabled);

		void SuppressBehavior(RE::Actor* a_actor, Kind a_kind)
		{
			a_actor->boolFlags.set(static_cast<RE::Actor::BOOL_FLAGS>(SUPPRESSED_BEHAVIOR));
			if (a_kind == Kind::kPlayer) {
				a_actor->boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			}
		}

		// A puppet's swings and punches are replays of the real ones (whose damage arrives through the
		// session), so they must hurt nobody: its outgoing damage is multiplied by zero. The zero is a
		// temporary modifier (-1 on top of the base 1), which the game does not carry into a save: a
		// crash mid-session leaves the NPC's real damage intact. Undone when an NPC is released.
		void SetAttackDamage(RE::Actor* a_actor, float a_multiplier)
		{
			const auto values = RE::ActorValue::GetSingleton();
			if (values && values->attackDamageMult) {
				const auto owner = static_cast<RE::ActorValueOwner*>(a_actor);
				const float current = owner->GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, *values->attackDamageMult);
				owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *values->attackDamageMult, (a_multiplier - 1.0f) - current);
			}
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

		// A mirrored NPC's graph refused "moveStart": something its own game started holds it. Dogmeat's
		// graph starts moving only from its plain standing idle, and a dog left standing sits, lies down
		// or turns on the spot after a few seconds, so the mirror slid after its owner's copy sitting
		// down. The AI ends the idle before walking off; with the AI off, we do: "IdleStop" gets it up
		// (the graph then plays its getting-up), "turnStop" ends a turn on the spot, and DriveAnimation's
		// "moveStart" is taken once it stands. Called right after DriveAnimation asked it to move.
		void ReleaseIdle(RE::Actor* a_actor)
		{
			static const RE::BSFixedString idleStop{ "IdleStop" };
			static const RE::BSFixedString turnStop{ "turnStop" };
			static const RE::BSFixedString doNotInterrupt{ "bDoNotInterrupt" };

			bool due = false;
			{
				std::scoped_lock l{ lock };
				const auto entry = Find(a_actor);
				if (!entry) {
					return;
				}
				due = entry->idleRelease.Due(!entry->moving, std::chrono::steady_clock::now());
			}
			if (!due) {
				return;
			}
			const auto graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			// What the engine's own idle stop checks: a paired or scripted animation that must play out.
			if (bool keep = false; graph->GetGraphVariableImplBool(doNotInterrupt, keep) && keep) {
				return;
			}
			const bool stood = graph->NotifyAnimationGraphImpl(idleStop);
			const bool turned = graph->NotifyAnimationGraphImpl(turnStop);
			{
				std::scoped_lock l{ lock };
				if (const auto entry = Find(a_actor)) {
					++entry->idleStops;
					entry->idleStopsTaken += stood || turned;
				}
			}
			if (stood || turned) {
				REX::INFO("Puppets: {:08X} was held by an idle while its owner's copy moves; stopped it (idle={} turn={})", a_actor->GetFormID(), stood, turned);
			}
		}

		// Draws or holsters the weapon to match the remote player. Retried at most once a second,
		// since drawing takes a moment and fails while the puppet's equipment is being changed.
		void MatchWeaponDrawn(RE::Actor* a_actor, const Puppet& a_puppet)
		{
			const bool wantDrawn = (a_puppet.target.flags & Protocol::kWeaponDrawn) != 0;
			const bool drawn = a_actor->GetWeaponMagicDrawn();
			const auto now = std::chrono::steady_clock::now();
			// A redraw holsters first; the next attempt draws.
			const bool holster = a_puppet.redraw && drawn;
			if (now < a_puppet.nextDrawAttempt || (wantDrawn == drawn && !holster)) {
				return;
			}
			a_actor->DrawWeaponMagicHands(wantDrawn && !holster);
			std::scoped_lock l{ lock };
			if (const auto entry = Find(a_actor)) {
				entry->nextDrawAttempt = now + 1s;
				entry->redraw = false;
			}
		}

		// With AI off nothing feeds the animation graph, so we do: Speed/Direction every frame,
		// moveStart/moveStop when the remote player starts or stops, and jump events when they
		// leave or reach the ground.
		void DriveAnimation(RE::Actor* a_actor, const Puppet& a_puppet)
		{
			static const RE::BSFixedString speedVar{ "Speed" };
			static const RE::BSFixedString directionVar{ "Direction" };
			static const RE::BSFixedString moveStart{ "moveStart" };
			static const RE::BSFixedString moveStop{ "moveStop" };
			static const RE::BSFixedString jumpStart{ "jumpStart" };
			static const RE::BSFixedString jumpFall{ "jumpFall" };
			static const RE::BSFixedString jumpLand{ "jumpLand" };
			static const RE::BSFixedString sneakStart{ "sneakStart" };
			static const RE::BSFixedString sneakStop{ "sneakStop" };

			const auto  graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			const auto& motion = a_puppet.target;

			bool moving = a_puppet.moving;
			if (!moving && motion.speed > MOVE_START) {
				moving = graph->NotifyAnimationGraphImpl(moveStart);
			} else if (moving && motion.speed < MOVE_STOP) {
				graph->NotifyAnimationGraphImpl(moveStop);
				moving = false;
			}

			// Crouching: the graph takes "sneakStart"/"sneakStop" (an NPC has no sneak key).
			const bool sneaking = (motion.flags & Protocol::kSneaking) != 0;
			const bool wasSneaking = (a_puppet.appliedFlags & Protocol::kSneaking) != 0;
			if (sneaking != wasSneaking) {
				graph->NotifyAnimationGraphImpl(sneaking ? sneakStart : sneakStop);
			}

			const bool inAir = (motion.flags & Protocol::kInAir) != 0;
			const bool wasInAir = (a_puppet.appliedFlags & Protocol::kInAir) != 0;
			if (inAir && !wasInAir) {
				graph->NotifyAnimationGraphImpl((motion.flags & Protocol::kJumping) ? jumpStart : jumpFall);
			} else if (!inAir && wasInAir) {
				graph->NotifyAnimationGraphImpl(jumpLand);
			}

			// The animation runs at the speed the stand-in really moves; the reported speed decides
			// only whether it is moving at all.
			graph->SetGraphVariableFloat(speedVar, moving ? (std::max)(a_puppet.measured.speed, motion.speed) : 0.0f);
			graph->SetGraphVariableFloat(directionVar, motion.direction);

			if (moving != a_puppet.moving || motion.flags != a_puppet.appliedFlags) {
				std::scoped_lock l{ lock };
				if (const auto entry = Find(a_actor)) {
					entry->moving = moving;
					entry->appliedFlags = motion.flags;
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
				if (const auto it = puppets.find(a_this); it != puppets.end() && it->second.formId == a_this->GetFormID()) {
					puppet = it->second;
				}
			}

			if (!puppet) {
				originalUpdate(a_this, a_delta);
				return;
			}

			// A mirrored NPC that died here ragdolls normally.
			if (puppet->kind == Kind::kNpc && a_this->IsDead(false)) {
				originalUpdate(a_this, a_delta);
				return;
			}
			// Climbing into or out of power armor is a furniture interaction the AI drives: let it.
			if (std::chrono::steady_clock::now() < puppet->aiUntil) {
				originalUpdate(a_this, a_delta);
				return;
			}

			SuppressBehavior(a_this, puppet->kind);
			if (puppet->kind == Kind::kPlayer) {
				KeepHealthy(a_this);
			}
			if (puppet->hasTarget) {
				a_this->SetPosition(puppet->target.position, true);
				a_this->SetHeading(puppet->target.heading);
				static_cast<RE::ActorState&>(*a_this).moveMode = puppet->target.moveMode;
				MatchWeaponDrawn(a_this, *puppet);
				// The engine feeds its locomotion graph from the actor's desired movement, which for an
				// actor with no AI is a stale walk; tell it what the stand-in really does so the run
				// cycle matches the ground covered.
				if (const auto process = a_this->currentProcess) {
					const float speed = puppet->moving ? (std::max)(puppet->measured.speed, puppet->target.speed) : 0.0f;
					const RE::NiPoint3 velocity{ std::sin(puppet->target.heading) * speed, std::cos(puppet->target.heading) * speed, 0.0f };
					if (const auto middle = process->middleHigh) {
						middle->desiredSpeed = speed;
					}
					if (const auto high = process->high) {
						high->pathingDesiredMovementSpeed = velocity;
						high->pathingCurrentMovementSpeed = velocity;
					}
				}
			}
			a_this->UpdateNoAI(a_delta);
			// After the engine's own update, which writes its idea of the speed (half of what a
			// teleported actor covers) into the graph: ours is the last word before the graph samples.
			if (puppet->hasTarget) {
				DriveAnimation(a_this, *puppet);
				// It has just asked a standing NPC's graph to move; a refusal means an idle holds it.
				if (puppet->kind == Kind::kNpc && !puppet->moving && puppet->target.speed > MOVE_START) {
					ReleaseIdle(a_this);
				}
			}
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
				const char* name = a_itemActivated->GetDisplayFullName();
				name = name ? name : "";
				a_result.Set(name, std::strlen(name));
				return true;
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

	void Register(RE::Actor* a_actor, Kind a_kind)
	{
		if (!a_actor) {
			return;
		}
		InstallHook(a_actor);
		if (a_kind == Kind::kPlayer) {
			InstallActivationHooks(a_actor);
		}
		const auto originalFlags = a_actor->boolFlags.underlying() & SUPPRESSED_BEHAVIOR;
		SuppressBehavior(a_actor, a_kind);
		SetAttackDamage(a_actor, 0.0f);
		a_actor->InitiateDoNothingPackage();

		std::scoped_lock l{ lock };
		puppets.insert_or_assign(a_actor, Puppet{
			.kind = a_kind,
			.formId = a_actor->GetFormID(),
			.originalFlags = originalFlags,
			.nextIdleRefresh = std::chrono::steady_clock::now() + IDLE_REFRESH,
		});
		puppetCount = puppets.size();
	}

	void Unregister(const RE::Actor* a_actor, std::uint32_t a_formId)
	{
		// The actor may be freed already (its owner noticed it gone), so it is never read here; the
		// entry goes only if it is still that actor's, not a newer actor's at the same address.
		std::scoped_lock l{ lock };
		if (const auto it = puppets.find(a_actor); it != puppets.end() && it->second.formId == a_formId) {
			puppets.erase(it);
		}
		puppetCount = puppets.size();
	}


	bool IsPuppet(const RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		const auto it = puppets.find(a_actor);
		return it != puppets.end() && it->second.kind == Kind::kPlayer && it->second.formId == a_actor->GetFormID();
	}

	void ReleaseNpc(RE::Actor* a_actor)
	{
		if (!a_actor || a_actor->IsDeleted()) {
			return;
		}
		std::uint32_t originalFlags = 0;
		{
			std::scoped_lock l{ lock };
			const auto entry = Find(a_actor);
			if (!entry) {
				return;  // not ours (a freed actor's address reused): leave its flags alone
			}
			originalFlags = entry->originalFlags;
		}
		Unregister(a_actor, a_actor->GetFormID());
		// Clear only what we set; a script may have blocked the NPC on purpose.
		a_actor->boolFlags.reset(static_cast<RE::Actor::BOOL_FLAGS>(SUPPRESSED_BEHAVIOR & ~originalFlags));
		SetAttackDamage(a_actor, 1.0f);
		// Drop the do-nothing package and pick the NPC's own again.
		RE::Console::ExecuteCommand(std::format("{:08X}.evp", a_actor->GetFormID()).c_str());
	}

	void RedrawWeapon(RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		if (const auto entry = Find(a_actor)) {
			entry->redraw = true;
			entry->nextDrawAttempt = std::chrono::steady_clock::now() + 1s;  // let the equipping finish
		}
	}

	void AllowAI(RE::Actor* a_actor, std::chrono::milliseconds a_for)
	{
		std::scoped_lock l{ lock };
		if (const auto entry = Find(a_actor)) {
			entry->aiUntil = std::chrono::steady_clock::now() + a_for;
		}
	}

	void SetTarget(RE::Actor* a_actor, const Motion& a_motion)
	{
		std::scoped_lock l{ lock };
		if (const auto entry = Find(a_actor)) {
			entry->target = a_motion;
			entry->hasTarget = true;
		}
	}

	std::string Describe(const RE::Actor* a_actor)
	{
		Puppet puppet;
		{
			std::scoped_lock l{ lock };
			const auto entry = Find(a_actor);
			if (!entry) {
				return "puppet=no";
			}
			puppet = *entry;
		}
		auto out = std::format("puppet: moving={} speed={:.0f} idleStops={} taken={} graph:", puppet.moving, puppet.target.speed, puppet.idleStops, puppet.idleStopsTaken);
		// What the locomotion graph holds: a dog that refuses to move shows bForceIdleStop (sitting or
		// lying), and one that runs shows its gait in iSyncLocomotionRangeID (3 is the run).
		const auto graph = static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor);
		for (const auto name : { "Speed", "iSyncLocomotionRangeID", "bAnimationDriven", "bForceIdleStop" }) {
			const RE::BSFixedString var{ name };
			float                   f = 0.0f;
			std::int32_t            n = 0;
			bool                    b = false;
			if (graph->GetGraphVariableImplFloat(var, f)) {
				out += std::format(" {}={:.0f}", name, f);
			} else if (graph->GetGraphVariableImplInt(var, n)) {
				out += std::format(" {}={}", name, n);
			} else if (graph->GetGraphVariableImplBool(var, b)) {
				out += std::format(" {}={}", name, b);
			}
		}
		return out;
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();

		std::vector<std::pair<const RE::Actor*, std::uint32_t>> due;
		{
			std::scoped_lock l{ lock };
			for (auto& [actor, puppet] : puppets) {
				if (now >= puppet.nextIdleRefresh) {
					puppet.nextIdleRefresh = now + IDLE_REFRESH;
					due.emplace_back(actor, puppet.formId);
				}
			}
		}

		// The owners (RemotePlayers, NpcSync) unregister actors the game deleted, but only on their
		// next update; never touch an actor that may already be gone.
		for (const auto& [actor, formId] : due) {
			const auto live = RE::TESForm::GetFormByID<RE::Actor>(formId);
			if (live == actor && !live->IsDead(false)) {
				live->InitiateDoNothingPackage();
			}
		}
	}
}
