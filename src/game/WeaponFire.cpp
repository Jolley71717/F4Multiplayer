#include "game/WeaponFire.h"

#include "game/DeferredDelete.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/AttackKind.h"

#include "game/ShotLoops.h"

#include "Protocol.h"

namespace WeaponFire
{
	// Defined with the explosion watch below.
	void          ArmGhostWatch(std::uint32_t a_actorId);
	std::uint32_t GhostsRemoved();
	// Defined with the shown throwable below.
	void          ShowThrowable(RE::Actor* a_actor);
	void          MarkThrowRelease(std::uint32_t a_actorId);
	void          ArmPlayerThrowSample();
	std::uint32_t ThrowablesShown();

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
		const char* EmoteIdle(std::uint8_t a_action);

		bool IsAttackEvent(std::string_view a_tag)
		{
			return a_tag == "WeaponFire" || a_tag == "weaponSwing";
		}

		// A grenade or mine leaving the hand: the graph's "WeaponFire" with payload "2" (a gun's has none).
		bool IsThrowEvent(std::string_view a_tag, std::string_view a_payload)
		{
			return a_tag == "WeaponFire" && a_payload == "2";
		}

		// The throw's wind-up: the graph asks the engine to put the throwable (equip index 2) in the hand
		// with "weaponAttach(2)", and to take it away after the throw with "weaponDetach(2)". Counted
		// for NPC graphs (a stand-in's replayed throw among them) to tell a wind-up that never asked
		// from one the engine had nothing to show for.
		bool IsThrowAttachEvent(std::string_view a_tag, std::string_view a_payload)
		{
			return a_tag == "weaponAttach" && a_payload == "2";
		}

		std::atomic<std::uint32_t> throwAttaches{ 0 };

		// Other things the graph tells us the actor did, replayed on the stand-in as ShotActions.
		std::optional<std::uint8_t> ActionOf(std::string_view a_tag)
		{
			if (a_tag == "reloadStateEnter") {  // what the graph announces; "reloadStart" is what it takes
				return Protocol::ShotAction::kReload;
			}
			if (a_tag == "sightedStateEnter") {
				return Protocol::ShotAction::kAimStart;
			}
			if (a_tag == "sightedStateExit") {
				return Protocol::ShotAction::kAimStop;
			}
			return std::nullopt;
		}

		std::mutex                 actionLock;
		std::unordered_set<std::uint32_t> ownedNpcs;  // the NPCs we run: only their actions are worth queuing
		std::vector<std::uint8_t>  playerActions;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> npcActions;
		constexpr std::size_t      MAX_PENDING_ACTIONS = 64;

		std::atomic<bool>        logging{ false };
		std::uint32_t            explosionsReported = 0;  // explosions seen after our throws and sent
		std::uint32_t            explosionsSetOff = 0;    // explosions from other players set off here
		std::mutex               logLock;
		std::vector<std::string> logged;

		RE::BSEventNotifyControl OnPlayerAnimationEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			const std::string_view tag = a_event.tag.c_str() ? a_event.tag.c_str() : "";
			const std::string_view payload = a_event.payload.c_str() ? a_event.payload.c_str() : "";
			if (IsThrowEvent(tag, payload)) {
				std::scoped_lock l{ actionLock };
				if (playerActions.size() < MAX_PENDING_ACTIONS) {
					playerActions.push_back(Protocol::ShotAction::kThrow);
				}
				ArmExplosionWatch();
			} else if (IsAttackEvent(tag)) {
				++pending;
				++total;
			} else if (IsThrowAttachEvent(tag, payload)) {
				ArmPlayerThrowSample();  // where the engine hangs the grenade on an actor it does show it for
			}
			if (const auto action = ActionOf(tag)) {
				std::scoped_lock l{ actionLock };
				if (playerActions.size() < MAX_PENDING_ACTIONS) {
					playerActions.push_back(*action);
				}
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
			const std::string_view npcTag = a_event.tag.c_str() ? a_event.tag.c_str() : "";
			const std::string_view npcPayload = a_event.payload.c_str() ? a_event.payload.c_str() : "";
			// A stand-in's grenade leaving its hand (or the graph taking it away): the one the plugin hung comes off.
			if (IsThrowEvent(npcTag, npcPayload) || (npcTag == "weaponDetach" && npcPayload == "2")) {
				MarkThrowRelease(id);
			}
			if (Protocol::IsShareableRef(id) && IsThrowEvent(npcTag, npcPayload)) {
				std::scoped_lock l{ actionLock };
				if (ownedNpcs.contains(id) && npcActions.size() < MAX_PENDING_ACTIONS) {
					npcActions.push_back({ id, Protocol::ShotAction::kThrow });
				}
				ArmExplosionWatch();
			} else if (Protocol::IsShareableRef(id) && IsAttackEvent(npcTag)) {
				std::scoped_lock l{ npcLock };
				if (npcShots.size() < MAX_PENDING_NPC_SHOTS) {
					npcShots.push_back(id);
				}
			} else if (IsThrowAttachEvent(npcTag, npcPayload)) {
				++throwAttaches;
			}
			if (const auto action = Protocol::IsShareableRef(id) && a_event.tag.c_str() ? ActionOf(a_event.tag.c_str()) : std::nullopt) {
				std::scoped_lock l{ actionLock };
				if (ownedNpcs.contains(id) && npcActions.size() < MAX_PENDING_ACTIONS) {
					npcActions.push_back({ id, *action });
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

	std::vector<std::uint8_t> TakeActions()
	{
		std::scoped_lock l{ actionLock };
		return std::exchange(playerActions, {});
	}

	void QueueAction(std::uint8_t a_action)
	{
		std::scoped_lock l{ actionLock };
		if (playerActions.size() < MAX_PENDING_ACTIONS) {
			playerActions.push_back(a_action);
		}
	}

	const char* EmoteIdleName(std::uint8_t a_action)
	{
		return EmoteIdle(a_action);
	}

	std::vector<std::pair<std::uint32_t, std::uint8_t>> TakeNpcActions()
	{
		std::scoped_lock l{ actionLock };
		return std::exchange(npcActions, {});
	}

	void SetOwnedNpcs(std::vector<std::uint32_t> a_ids)
	{
		std::scoped_lock l{ actionLock };
		ownedNpcs = std::unordered_set<std::uint32_t>(a_ids.begin(), a_ids.end());
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

		// Plays a sound at the actor and follows them while it lasts. If the actor's previous shot is
		// still sounding (a looping automatic-weapon sound), that one runs on instead: one burst.
		bool PlaySound(RE::Actor* a_actor, const RE::BGSSoundDescriptorForm* a_sound)
		{
			const auto now = std::chrono::steady_clock::now();
			if (loopingShots.Extend(a_actor->GetFormID(), now)) {
				return true;
			}
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
				loopingShots.Add(a_actor->GetFormID(), handle, now);
			}
			return true;
		}
	}

	void ScanExplosions();
	void RemoveGhostGrenades();
	void UpdateShownThrowables();

	void Frame()
	{
		loopsEnded += static_cast<std::uint32_t>(loopingShots.EndDue(std::chrono::steady_clock::now(), [](RE::BSSoundHandle& a_handle) {
			a_handle.FadeOutAndRelease(LOOP_FADE_MS);
		}));
		ScanExplosions();
		RemoveGhostGrenades();
		UpdateShownThrowables();
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
		constexpr std::uint32_t SIGHTED = 0x00004A57;          // ActionSighted
		constexpr std::uint32_t SIGHTED_RELEASE = 0x00004A58;  // ActionSightedRelease
		constexpr std::uint32_t THROW = 0x00004E32;            // ActionThrow
		std::uint32_t           actionsPlayed = 0;

		// The idle behind an emote action (editor IDs from Fallout4.esm), or nullptr for other actions.
		const char* EmoteIdle(std::uint8_t a_action)
		{
			switch (a_action) {
			case Protocol::ShotAction::kPoint:
				return "IdlePointing";
			case Protocol::ShotAction::kCheer:
				return "IdleCheeringStanding";
			case Protocol::ShotAction::kClap:
				return "IdleClapping";
			default:
				return nullptr;
			}
		}
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
		// The graph refuses a new attack while the last one still plays (automatic fire arrives faster
		// than the animation), so the animation is best effort; the sound is played for every shot.
		if (static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
		}
		// Nothing is fired, so the game plays no sound: play the gun's own, from the shooter.
		if (PlaySound(a_actor, FireSound(a_actor))) {
			++sounded;
		}
	}

	void PlayAction(RE::Actor* a_actor, std::uint8_t a_action)
	{
		static const RE::BSFixedString reloadStart{ "reloadStart" };
		if (!a_actor || a_actor->IsDead(false) || !a_actor->Get3D()) {
			return;
		}
		// Emotes: an idle, played the way the console does (the stand-in takes it while standing still).
		if (const auto idle = EmoteIdle(a_action)) {
			RE::Console::ExecuteCommand(std::format("{:08X}.playidle {}", a_actor->GetFormID(), idle).c_str());
			++actionsPlayed;
			return;
		}
		// A throw needs no drawn weapon: the grenade in the stand-in's hand (Equipment::HoldThrowable) flies.
		if (a_action == Protocol::ShotAction::kThrow) {
			const auto action = RE::TESForm::GetFormByID<RE::BGSAction>(THROW);
			const bool taken = action && a_actor->PerformAction(action, nullptr);
			actionsPlayed += taken;
			if (taken) {
				ShowThrowable(a_actor);
			}
			// What the engine had in hand as the wind-up began; the ghost watch logs it again mid-throw.
			REX::INFO("WeaponFire: throw replayed on {:08X}: taken={} holds{} hands{}", a_actor->GetFormID(), taken, Equipment::DescribeHeld(a_actor), Equipment::DescribeHands(a_actor));
			ArmGhostWatch(a_actor->GetFormID());
			return;
		}
		if (!a_actor->GetWeaponMagicDrawn()) {
			return;
		}
		bool taken = false;
		switch (a_action) {
		case Protocol::ShotAction::kReload:
			// The graph takes this one (unlike "attackStart" on a melee weapon).
			taken = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(reloadStart);
			break;
		case Protocol::ShotAction::kAimStart:
		case Protocol::ShotAction::kAimStop: {
			// Aiming has no graph event an NPC takes; the engine's sighted actions raise and lower the gun.
			const auto action = RE::TESForm::GetFormByID<RE::BGSAction>(a_action == Protocol::ShotAction::kAimStart ? SIGHTED : SIGHTED_RELEASE);
			taken = action && a_actor->PerformAction(action, nullptr);
			break;
		}
		default:
			break;
		}
		actionsPlayed += taken;
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
		return std::format("shots={} replayed={}/{} swung={} sounded={} loops={}/{} actions={} explosions={}/{} ghosts={} attaches={} loads={} shown={}", total.load(), played, replayed, swung, sounded, loopingShots.Active(), loopsEnded, actionsPlayed, explosionsReported, explosionsSetOff, GhostsRemoved(), throwAttaches.load(), Equipment::AttachesAsked(), ThrowablesShown());
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

	// --- Explosions after a throw ------------------------------------------------------------
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		constexpr auto  EXPLOSION_WATCH = 8s;         // a frag grenade goes off within about 3 s; a mine when stepped on
		constexpr float EXPLOSION_RANGE = 6000.0f;    // from the player, both for reporting and for setting off
		std::atomic<Clock::time_point>         watchUntil{};  // written from the animation hooks, read in Frame
		Clock::time_point                      nextScan{};
		std::unordered_set<std::uint32_t>      seenExplosions;
		std::unordered_set<std::uint32_t>      setOffExplosions;  // the ones we placed for a friend's throw: never reported back as ours
		std::vector<Protocol::Explosion>       explosions;
		std::mutex                             explosionLock;

		// A stand-in that replayed a throw really throws the grenade it holds (the equipment sync gave it
		// one), so the friend's game got a second grenade landing at the stand-in's feet on top of the
		// owner's explosion. For a moment after the replay, anything the stand-in shoots is removed.
		constexpr auto                                        GHOST_WATCH = 2s;
		// The wind-up is where the grenade should show: what the engine holds then goes to the log once.
		constexpr auto                                        MID_THROW = 400ms;
		struct ThrowWatch
		{
			Clock::time_point until;
			Clock::time_point sampleAt;  // zero once logged
		};
		std::unordered_map<std::uint32_t, ThrowWatch>  ghostWatch;  // stand-in -> watch
		std::uint32_t                                  ghostsRemoved = 0;
	}

	void ReleaseSoundsOf(std::uint32_t a_actorFormId)
	{
		loopsEnded += static_cast<std::uint32_t>(loopingShots.EndFor(a_actorFormId, [](RE::BSSoundHandle& a_handle) {
			a_handle.FadeOutAndRelease(LOOP_FADE_MS);
		}));
	}

	void ArmExplosionWatch()
	{
		watchUntil = Clock::now() + EXPLOSION_WATCH;
	}

	void ScanExplosions()
	{
		const auto now = Clock::now();
		if (now > watchUntil.load()) {
			seenExplosions.clear();
			return;
		}
		if (now < nextScan) {
			return;
		}
		nextScan = now + 100ms;
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return;
		}
		// Every loaded cell around the player, not only the one they stand in: outdoors a grenade thrown
		// across a cell seam went off in the next cell over and was never reported (the thrower's friends
		// saw the throw and nothing else), while the same throw the other way was.
		const auto space = cell->IsInterior() ? std::pair{ cell->GetFormID(), 0u } : std::pair{ 0u, cell->worldSpace ? cell->worldSpace->GetFormID() : 0u };
		RE::TES::GetSingleton()->ForEachReferenceInRange(player->data.location, EXPLOSION_RANGE, [&](RE::TESObjectREFR* a_ref) {
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			if (!base || !base->Is(RE::ENUM_FORM_ID::kEXPL) || setOffExplosions.contains(a_ref->GetFormID()) || !seenExplosions.insert(a_ref->GetFormID()).second) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			std::scoped_lock e{ explosionLock };
			explosions.push_back({ 0, base->GetFormID(), a_ref->data.location.x, a_ref->data.location.y, a_ref->data.location.z, space.first, space.second });
			++explosionsReported;
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	void ArmGhostWatch(std::uint32_t a_actorId)
	{
		const auto now = Clock::now();
		ghostWatch[a_actorId] = { now + GHOST_WATCH, now + MID_THROW };
	}

	std::uint32_t GhostsRemoved()
	{
		return ghostsRemoved;
	}

	void RemoveGhostGrenades()
	{
		if (ghostWatch.empty()) {
			return;
		}
		const auto now = Clock::now();
		std::erase_if(ghostWatch, [&](const auto& a_entry) { return now > a_entry.second.until; });
		const auto tes = RE::TES::GetSingleton();
		if (!tes) {
			return;
		}
		for (auto& [actorId, watch] : ghostWatch) {
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(actorId);
			if (!actor || !actor->Get3D()) {
				continue;
			}
			if (watch.sampleAt != Clock::time_point{} && now >= watch.sampleAt) {
				watch.sampleAt = {};
				REX::INFO("WeaponFire: {:08X} mid-throw holds{} hands{} (attach events so far {})", actorId, Equipment::DescribeHeld(actor), Equipment::DescribeHands(actor), throwAttaches.load());
			}
			// Nothing is changed while the engine walks its lists: the ghosts are collected as handles and
			// dealt with afterwards, and only once their 3D is in (deleting a reference still loading is
			// the loader-thread crash DeferredDelete describes); one not ready yet is caught next frame.
			std::vector<RE::ObjectRefHandle> ghosts;
			tes->ForEachReferenceInRange(actor->data.location, 1000.0f, [&](RE::TESObjectREFR* a_ref) {
				const auto projectile = a_ref ? a_ref->As<RE::Projectile>() : nullptr;
				if (projectile && !projectile->IsDisabled() && projectile->shooter.get().get() == actor) {
					ghosts.push_back(projectile->GetHandle());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto& handle : ghosts) {
				const auto ghost = handle.get();
				if (ghost && !ghost->IsDeleted() && DeferredDelete::Safe(ghost->Get3D() != nullptr, 0s)) {
					ghost->Disable();
					ghost->SetDelete(true);
					++ghostsRemoved;
				}
			}
		}
	}

	// --- The grenade in the stand-in's hand ---------------------------------------------------
	namespace
	{
		// The engine shows a throwable during a throw of its own: on the player too the biped's grenade
		// slot stays empty, so at the graph's weaponAttach(2) it hangs the model from somewhere the plugin
		// could not reach, and for an NPC the plugin dressed it hangs nothing (the equip manager listed
		// the grenade, AttachWeapon was asked and loaded none; seen on the rig). So the stand-in's is hung
		// by the plugin: the weapon's own model, from the model DB, under the throwing hand for the
		// wind-up, off again when the graph reports the release or the ghost watch ends.
		constexpr const char* HAND_NODES[] = { "LArm_Hand", "RArm_Hand", "Weapon" };  // the first found: throws are left-handed
		constexpr RE::NiPoint3 IN_PALM{ 6.0f, 0.0f, 0.0f };  // a hand bone starts at the wrist and runs toward the fingers

		std::unordered_map<std::uint32_t, RE::NiPointer<RE::NiAVObject>> shownThrowables;  // stand-in -> the model hung
		std::uint32_t                                                    throwablesShown = 0;
		std::mutex                                                       releaseLock;
		std::vector<std::uint32_t>                                       released;  // from the NPC animation hook, drained in Frame
		std::atomic<Clock::time_point>                                   playerSampleAt{};

		void HideThrowable(std::uint32_t a_actorId)
		{
			const auto it = shownThrowables.find(a_actorId);
			if (it == shownThrowables.end()) {
				return;
			}
			// Taken off its parent only while that parent lives: when the stand-in's 3D is gone, so is the
			// node the model hung from, and the model (ours to release) goes with the map entry.
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_actorId);
			const auto model = it->second.get();
			if (actor && actor->Get3D() && model && model->parent) {
				model->parent->DetachChild(model);
			}
			shownThrowables.erase(it);
		}
	}

	void ShowThrowable(RE::Actor* a_actor)
	{
		const auto id = a_actor->GetFormID();
		const auto weapon = Equipment::HeldThrowable(a_actor);
		const auto root = a_actor->Get3D();
		const char* path = weapon ? weapon->GetModel() : nullptr;
		if (!root || !path || !*path || shownThrowables.contains(id)) {
			return;
		}
		RE::NiNode* hand = nullptr;
		const char* handName = "none";
		for (const auto name : HAND_NODES) {
			const auto found = root->GetObjectByName(RE::BSFixedString{ name });
			if (hand = found ? found->IsNode() : nullptr; hand) {
				handName = name;
				break;
			}
		}
		if (!hand) {
			REX::WARN("WeaponFire: {:08X} has no hand node to hang a throwable from", id);
			return;
		}
		// Loaded the way the engine loads a reference's model (from the archives if not cached: a small
		// file, on this thread), and cloned so the DB's own copy stays untouched.
		RE::BSModelDB::DBTraits::ArgsType args{};
		args.lodFadeMult = RE::ENUM_LOD_MULT::kNone;
		args.loadLevel = 0;
		args.prepareAfterLoad = 1;
		args.performProcess = 1;
		args.loadTextures = 1;
		RE::NiPointer<RE::NiNode> loaded;
		const auto error = RE::BSModelDB::Demand(path, &loaded, args);
		if (error != RE::BSResource::ErrorCode::kNone || !loaded) {
			REX::WARN("WeaponFire: throwable model '{}' did not load for {:08X} (error {})", path, id, std::to_underlying(error));
			return;
		}
		RE::NiCloningProcess cloning{};
		cloning.copyType = RE::NiCloningProcess::CopyType::kCopyExact;
		cloning.scale = { 1.0f, 1.0f, 1.0f };
		const auto cloned = loaded->CreateClone(cloning);
		if (cloned) {
			cloned->ProcessClone(cloning);
		}
		RE::NiPointer<RE::NiAVObject> model{ cloned ? static_cast<RE::NiAVObject*>(cloned) : static_cast<RE::NiAVObject*>(loaded.get()) };
		model->local.MakeIdentity();
		model->local.translate = IN_PALM;
		hand->AttachChild(model.get(), true);
		model->SetAppCulled(false);
		shownThrowables[id] = model;
		++throwablesShown;
		REX::INFO("WeaponFire: hung '{}' under {} of {:08X} (cloned={})", path, handName, id, cloned != nullptr);
	}

	void MarkThrowRelease(std::uint32_t a_actorId)
	{
		std::scoped_lock l{ releaseLock };
		if (released.size() < 64) {
			released.push_back(a_actorId);
		}
	}

	void ArmPlayerThrowSample()
	{
		playerSampleAt = Clock::now() + MID_THROW;
	}

	std::uint32_t ThrowablesShown()
	{
		return throwablesShown;
	}

	void UpdateShownThrowables()
	{
		if (!shownThrowables.empty()) {
			std::vector<std::uint32_t> done;
			{
				std::scoped_lock l{ releaseLock };
				done = std::exchange(released, {});
			}
			// The ghost watch outlives any throw: a stand-in no longer watched has long let go.
			for (const auto& shown : shownThrowables) {
				if (!ghostWatch.contains(shown.first)) {
					done.push_back(shown.first);
				}
			}
			for (const auto actorId : done) {
				HideThrowable(actorId);
			}
		} else {
			std::scoped_lock l{ releaseLock };
			released.clear();
		}
		// The player's own throw, 400 ms in: where the engine hangs a grenade on an actor it shows it for.
		if (const auto at = playerSampleAt.load(); at != Clock::time_point{} && Clock::now() >= at) {
			playerSampleAt = Clock::time_point{};
			const auto player = RE::PlayerCharacter::GetSingleton();
			REX::INFO("WeaponFire: player mid-throw holds{} hands{}", Equipment::DescribeHeld(player), Equipment::DescribeHands(player));
		}
	}

	std::vector<Protocol::Explosion> TakeExplosions()
	{
		std::scoped_lock l{ explosionLock };
		return std::exchange(explosions, {});
	}

	void ApplyExplosion(const Protocol::Explosion& a_explosion)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		const auto base = RE::TESForm::GetFormByID<RE::TESBoundObject>(a_explosion.base);
		if (!cell || !base || !base->Is(RE::ENUM_FORM_ID::kEXPL)) {
			return;
		}
		const auto space = cell->IsInterior() ? std::pair{ cell->GetFormID(), 0u } : std::pair{ 0u, cell->worldSpace ? cell->worldSpace->GetFormID() : 0u };
		const RE::NiPoint3 at{ a_explosion.x, a_explosion.y, a_explosion.z };
		// Written so that a NaN distance fails too (a comparison with NaN is always false).
		if (space != std::pair{ a_explosion.cell, a_explosion.worldspace } || !(at.GetDistance(player->data.location) <= EXPLOSION_RANGE)) {
			return;
		}
		RE::NEW_REFR_DATA data;
		data.location = at;
		data.object = base;
		data.interior = cell->IsInterior() ? cell : nullptr;
		data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
		data.clearStillLoadingFlag = true;
		const auto handle = EngineRefs::CreateReferenceAtLocation(data);
		if (const auto ref = handle.get()) {
			// A friend's explosion we just placed is in range of our own scan after our next throw: it
			// was reported back as ours, and went off a second time for everyone.
			if (setOffExplosions.size() > 256) {
				setOffExplosions.clear();
			}
			setOffExplosions.insert(ref->GetFormID());
		}
		++explosionsSetOff;
	}
}
