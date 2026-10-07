#include "game/WeaponFire.h"
#include "game/AttackKind.h"

#include "game/ShotLoops.h"

#include "Protocol.h"

namespace WeaponFire
{
	namespace
	{
		std::atomic<std::uint32_t> pending{ 0 };
		std::atomic<std::uint32_t> total{ 0 };
		std::uint32_t              replayed = 0;  // shots received for a stand-in or mirrored NPC
		std::uint32_t              played = 0;    // ... that its animation graph accepted
		std::uint32_t              sounded = 0;   // ... with the gunshot played

		// Every reference receives its own animation graph's events through its
		// BSTEventSink<BSAnimationGraphEvent> base (at 0x38). An actor's graph sends "WeaponFire"
		// once per shot.
		constexpr std::size_t ANIM_SINK_OFFSET = 0x38;
		constexpr std::size_t PROCESS_EVENT_INDEX = 1;

		using ProcessEventFn = RE::BSEventNotifyControl (*)(RE::BSTEventSink<RE::BSAnimationGraphEvent>*, const RE::BSAnimationGraphEvent&, RE::BSTEventSource<RE::BSAnimationGraphEvent>*);
		ProcessEventFn originalPlayerEvent = nullptr;

		// A shot ("WeaponFire", once per bullet) or a melee swing or punch ("weaponSwing", at the hit
		// frame): both are replayed as an attack on the stand-in, which swings whatever it holds.
		bool IsAttackEvent(std::string_view a_tag)
		{
			return a_tag == "WeaponFire" || a_tag == "weaponSwing";
		}

		std::atomic<bool>        logging{ false };
		std::mutex               logLock;
		std::vector<std::string> logged;

		RE::BSEventNotifyControl OnPlayerAnimationEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			const std::string_view tag = a_event.tag.c_str() ? a_event.tag.c_str() : "";
			if (IsAttackEvent(tag)) {
				++pending;
				++total;
			}
			if (logging) {
				std::scoped_lock l{ logLock };
				if (logged.size() < 200) {
					logged.push_back(std::format("{}({})", tag, a_event.payload.c_str() ? a_event.payload.c_str() : ""));
				}
			}
			return originalPlayerEvent(a_this, a_event, a_source);
		}

		// Other actors (NPCs, stand-ins and mirrored NPCs share one class).
		ProcessEventFn             originalActorEvent = nullptr;
		std::atomic<std::uint32_t> loggedActor{ 0 };
		std::mutex                 npcLock;
		std::vector<std::uint32_t> npcShots;
		constexpr std::size_t      MAX_PENDING_NPC_SHOTS = 256;

		RE::BSEventNotifyControl OnActorAnimationEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			const auto actor = reinterpret_cast<RE::Actor*>(reinterpret_cast<std::uintptr_t>(a_this) - ANIM_SINK_OFFSET);
			const auto id = actor->GetFormID();
			if (Protocol::IsShareableRef(id) && a_event.tag.c_str() && IsAttackEvent(a_event.tag.c_str())) {
				std::scoped_lock l{ npcLock };
				if (npcShots.size() < MAX_PENDING_NPC_SHOTS) {
					npcShots.push_back(id);
				}
			}
			if (logging && loggedActor != 0 && id == loggedActor) {
				std::scoped_lock l{ logLock };
				if (logged.size() < 200) {
					logged.push_back(std::format("{}({})", a_event.tag.c_str() ? a_event.tag.c_str() : "", a_event.payload.c_str() ? a_event.payload.c_str() : ""));
				}
			}
			return originalActorEvent(a_this, a_event, a_source);
		}
	}

	void InstallForNpcs(RE::Actor* a_npc)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (originalActorEvent || !originalPlayerEvent || !a_npc || !player || a_npc == player) {
			return;
		}
		const auto vtable = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(a_npc) + ANIM_SINK_OFFSET);
		const auto playerVtable = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(player) + ANIM_SINK_OFFSET);
		if (vtable == playerVtable) {
			return;
		}
		const auto slot = vtable + sizeof(void*) * PROCESS_EVENT_INDEX;
		originalActorEvent = *reinterpret_cast<ProcessEventFn*>(slot);
		REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&OnActorAnimationEvent));
		REX::INFO("WeaponFire: hooked NPC animation events (vtable {:X})", vtable);
	}

	void LogActor(RE::Actor* a_actor)
	{
		InstallForNpcs(a_actor);
		loggedActor = a_actor ? a_actor->GetFormID() : 0;
	}

	std::vector<std::uint32_t> TakeNpcShots()
	{
		std::scoped_lock l{ npcLock };
		return std::exchange(npcShots, {});
	}

	namespace
	{
		// The gunshot the game picked for the actor's weapon when it was equipped (by the
		// weapon's keywords: a suppressor changes it). The weapon's own sound fields are empty.
		RE::BGSSoundDescriptorForm* FireSound(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return nullptr;
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				if (!equipped.item.object || !equipped.item.object->Is(RE::ENUM_FORM_ID::kWEAP)) {
					continue;
				}
				const auto data = static_cast<RE::EquippedWeaponData*>(equipped.data.get());
				const auto mapping = data ? data->attackSoundData : nullptr;
				if (mapping && mapping->descriptor) {
					return mapping->descriptor;
				}
			}
			return nullptr;
		}

		// An automatic weapon's fire sound loops until the game stops it (when the trigger is
		// released). Replayed shots have no trigger, so each loop is ended after one shot's worth.
		constexpr std::uint16_t LOOP_FADE_MS = 40;

		ShotLoops<RE::BSSoundHandle> loopingShots;
		std::uint32_t                loopsEnded = 0;

		// Plays a sound at the actor and follows them while it lasts.
		bool PlaySound(RE::Actor* a_actor, const RE::BGSSoundDescriptorForm* a_sound)
		{
			constexpr std::uint32_t POSITIONED = 0x10;  // a 3D sound, heard from where it is
			const auto audio = RE::BSAudioManager::GetSingleton();
			RE::BSSoundHandle handle;
			if (!a_sound || !audio || !audio->GetSoundHandle(handle, a_sound, 0.0f, POSITIONED)) {
				return false;
			}
			handle.SetPosition(a_actor->data.location);
			if (const auto root = a_actor->Get3D()) {
				handle.SetObjectToFollow(root);
			}
			if (!handle.Play()) {
				return false;
			}
			if (handle.IsEnvelopeLoop()) {
				loopingShots.Add(handle, std::chrono::steady_clock::now());
			}
			return true;
		}
	}

	void Frame()
	{
		loopsEnded += static_cast<std::uint32_t>(loopingShots.EndDue(std::chrono::steady_clock::now(), [](RE::BSSoundHandle& a_handle) {
			a_handle.FadeOutAndRelease(LOOP_FADE_MS);
		}));
	}

	namespace
	{
		static_assert(std::to_underlying(RE::WEAPON_TYPE::kGun) == AttackKind::GUN_TYPE);

		// Whether the actor's equipped weapon is a gun (fists, nothing equipped, count as melee).
		bool HoldsGun(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return false;
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				const auto weapon = equipped.item.object ? equipped.item.object->As<RE::TESObjectWEAP>() : nullptr;
				if (weapon) {
					return AttackKind::ReplaysAsGun(std::to_underlying(weapon->weaponData.type.get()), true);
				}
			}
			return false;
		}

		std::uint32_t swung = 0;

		// ActionRightAttack in Fallout4.esm (the default object manager's lookup crashes the game).
		constexpr std::uint32_t RIGHT_ATTACK = 0x00013005;
	}

	void PlayShot(RE::Actor* a_actor)
	{
		static const RE::BSFixedString attackStart{ "attackStart" };
		++replayed;
		if (!a_actor || a_actor->IsDead(false) || !a_actor->GetWeaponMagicDrawn() || !a_actor->Get3D()) {
			return;
		}
		if (!HoldsGun(a_actor)) {
			// A swing or a punch. The melee graph refuses "attackStart", so the engine is asked for the
			// attack itself, as an NPC's AI would: the stand-in swings (with the weapon's own sound)
			// but hurts nobody, its damage multiplier being zero (Puppets).
			const auto action = RE::TESForm::GetFormByID<RE::BGSAction>(RIGHT_ATTACK);
			if (action && a_actor->PerformAction(action, nullptr)) {
				++played;
				++swung;
			}
			return;
		}
		if (static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
			// Nothing is fired, so the game plays no sound: play the gun's own, from the shooter.
			if (PlaySound(a_actor, FireSound(a_actor))) {
				++sounded;
			}
		}
	}

	void Install()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || originalPlayerEvent) {
			return;
		}
		const auto vtable = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(player) + ANIM_SINK_OFFSET);
		const auto slot = vtable + sizeof(void*) * PROCESS_EVENT_INDEX;
		originalPlayerEvent = *reinterpret_cast<ProcessEventFn*>(slot);
		REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&OnPlayerAnimationEvent));
		REX::INFO("WeaponFire: hooked player animation events (vtable {:X})", vtable);
	}

	std::uint32_t TakeShots()
	{
		return pending.exchange(0);
	}

	std::string Describe()
	{
		return std::format("shots={} replayed={}/{} swung={} sounded={} loops={}/{}", total.load(), played, replayed, swung, sounded, loopingShots.Active(), loopsEnded);
	}

	std::string LogAnimationEvents(bool a_start)
	{
		std::scoped_lock l{ logLock };
		logging = a_start;
		std::string out;
		for (const auto& entry : logged) {
			out += entry + " ";
		}
		logged.clear();
		return out.empty() ? "none" : out;
	}
}
