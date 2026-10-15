#include "game/WeaponFire.h"

#include "game/BlastShield.h"
#include "game/DeferredDelete.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/AttackKind.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"

#include "game/ShotLoops.h"

#include "Protocol.h"

namespace WeaponFire
{
	// Defined with the explosion watch below.
	void          ArmGhostWatch(std::uint32_t a_actorId);
	std::uint32_t GhostsRemoved();

	namespace
	{
		std::atomic<std::uint32_t> pending{ 0 };
		std::atomic<std::uint32_t> total{ 0 };
		std::uint32_t              replayed = 0;  // shots received for a stand-in or mirrored NPC
		std::uint32_t              played = 0;    // ... that its animation graph accepted
		std::uint32_t              sounded = 0;   // ... with the gunshot played
		std::uint32_t              theirs = 0;    // ... of which with the shooter's own gun sound
		std::uint32_t              lastSound = 0;  // the keyword sound mapping last played
		bool                       lastLooped = false;  // ... and whether its sound loops (automatic fire)

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
		std::uint32_t            explosionRefusedRadius = 0;  // ... not set off: no throw makes one that big (or it has no radius)
		std::uint32_t            copiesLent = 0;     // our copies of NPCs other games run, kept from a friend's explosion (BlastShield)
		std::uint32_t            blastsUndone = 0;   // ... given back what it took from them
		std::uint32_t            lentKilled = 0;     // ... killed by our player's blows meanwhile
		std::uint32_t            explosionsLogged = 0;
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
			if (Protocol::IsShareableRef(id) && IsThrowEvent(npcTag, npcPayload)) {
				std::scoped_lock l{ actionLock };
				// Only an NPC we run: one mirrored from a friend's game threw in theirs, and their game reports what
				// went off. Watched here too, the same explosion could be reported from both games.
				if (ownedNpcs.contains(id)) {
					if (npcActions.size() < MAX_PENDING_ACTIONS) {
						npcActions.push_back({ id, Protocol::ShotAction::kThrow });
					}
					ArmExplosionWatch();
				}
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
		// The gunshot the game picked for the actor's gun when it was equipped (by the gun's
		// keywords, which its mods add: a suppressor or an automatic receiver changes it). The
		// weapon's own sound fields are empty. Only a gun's counts: a grenade is equipped beside it.
		const RE::BGSSoundKeywordMapping* FireSound(RE::Actor* a_actor)
		{
			const auto process = a_actor ? a_actor->currentProcess : nullptr;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return nullptr;
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				const auto weapon = equipped.item.object ? equipped.item.object->As<RE::TESObjectWEAP>() : nullptr;
				if (!weapon || !AttackKind::ReplaysAsGun(std::to_underlying(weapon->weaponData.type.get()), true)) {
					continue;
				}
				const auto data = static_cast<RE::EquippedWeaponData*>(equipped.data.get());
				const auto mapping = data ? data->attackSoundData : nullptr;
				if (mapping && mapping->descriptor) {
					return mapping;
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
				lastLooped = true;
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
			lastLooped = handle.IsEnvelopeLoop();
			if (lastLooped) {
				loopingShots.Add(a_actor->GetFormID(), handle, now);
			}
			return true;
		}
	}

	std::uint32_t ShotSound(RE::Actor* a_actor)
	{
		const auto mapping = FireSound(a_actor);
		return mapping ? mapping->GetFormID() : 0;
	}

	void ScanExplosions();
	void RemoveGhostGrenades();
	void EndDueLending();

	void Frame()
	{
		loopsEnded += static_cast<std::uint32_t>(loopingShots.EndDue(std::chrono::steady_clock::now(), [](RE::BSSoundHandle& a_handle) {
			a_handle.FadeOutAndRelease(LOOP_FADE_MS);
		}));
		ScanExplosions();
		RemoveGhostGrenades();
		EndDueLending();
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
		std::uint32_t unseen = 0;  // shots heard from a stand-in whose gun was put away (no firing animation)

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

	void PlayShot(RE::Actor* a_actor, std::uint32_t a_sound)
	{
		static const RE::BSFixedString attackStart{ "attackStart" };
		++replayed;
		if (!a_actor || a_actor->IsDead(false) || !a_actor->Get3D()) {
			return;
		}
		// A gun put away (the stand-in draws it again after it is dressed) still sounds: only the firing animation
		// needs it out (AttackKind::ReplayShot).
		const auto replay = AttackKind::ReplayShot(HoldsGun(a_actor), a_actor->GetWeaponMagicDrawn());
		if (replay == AttackKind::Replay::kNone) {
			return;
		}
		if (replay == AttackKind::Replay::kSwing) {
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
		if (replay == AttackKind::Replay::kSoundOnly) {
			++unseen;
		} else if (static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
		}
		// Nothing is fired, so the game plays no sound: play the gun's own, from the shooter. The
		// shooter's game picked it from their gun's mods; the stand-in's copy of the gun is a new one
		// of the same base weapon, without those mods, so the sound picked here can belong to another
		// receiver (an automatic one's looping fire, for a semi-automatic pistol's single shots) or
		// miss the suppressor. This game's pick is only for a shooter that sent none we know.
		const auto shooters = a_sound ? RE::TESForm::GetFormByID<RE::BGSSoundKeywordMapping>(a_sound) : nullptr;
		const auto mapping = shooters && shooters->descriptor ? shooters : FireSound(a_actor);
		if (mapping && PlaySound(a_actor, mapping->descriptor)) {
			++sounded;
			theirs += mapping == shooters;
			lastSound = mapping->GetFormID();
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
			// What the engine had in hand as the wind-up began; the ghost watch logs it again mid-throw.
			REX::INFO("WeaponFire: throw replayed on {:08X}: taken={} holds{}", a_actor->GetFormID(), taken, Equipment::DescribeHeld(a_actor));
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
		return std::format("shots={} replayed={}/{} swung={} sounded={} unseen={} theirs={} kssm={:08X}{} loops={}/{} actions={} explosions={}/{} ghosts={} attaches={} loads={} explosionRefusedRadius={} lent={}/{}/{}", total.load(), played, replayed, swung, sounded, unseen, theirs, lastSound, lastLooped ? "(loop)" : "", loopingShots.Active(), loopsEnded, actionsPlayed, explosionsReported, explosionsSetOff, GhostsRemoved(), throwAttaches.load(), Equipment::AttachesAsked(), explosionRefusedRadius, copiesLent, blastsUndone, lentKilled);
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
		constexpr float EXPLOSION_RANGE = RemoteEdits::EXPLOSION_RANGE;  // from the player, both for reporting and for setting off
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
			Clock::time_point started;
			std::uint32_t     removed = 0;  // its projectiles removed so far
		};
		std::unordered_map<std::uint32_t, ThrowWatch>  ghostWatch;  // stand-in -> watch
		std::uint32_t                                  ghostsRemoved = 0;
		std::uint32_t                                  ghostsLogged = 0;
		// The references around a throwing stand-in are walked this often, not every frame: a grenade lives far longer.
		constexpr auto                                 GHOST_CHECK_INTERVAL = 50ms;
		Clock::time_point                              nextGhostCheck{};
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
		ghostWatch[a_actorId] = { now + GHOST_WATCH, now + MID_THROW, now };
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
		if (now < nextGhostCheck) {
			return;
		}
		nextGhostCheck = now + GHOST_CHECK_INTERVAL;
		std::erase_if(ghostWatch, [&](const auto& a_entry) {
			if (now <= a_entry.second.until) {
				return false;
			}
			// A grenade it let go of later than this is a real one in our world: it goes off here, hurting what is beside it.
			if (a_entry.second.removed == 0 && ghostsLogged < 40) {
				++ghostsLogged;
				REX::INFO("WeaponFire: no projectile of {:08X}'s removed in the {} ms after its replayed throw", a_entry.first, std::chrono::duration_cast<std::chrono::milliseconds>(GHOST_WATCH).count());
			}
			return true;
		});
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
				REX::INFO("WeaponFire: {:08X} mid-throw holds{} (attach events so far {})", actorId, Equipment::DescribeHeld(actor), throwAttaches.load());
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
					++watch.removed;
					if (ghostsLogged < 40) {
						++ghostsLogged;
						REX::INFO("WeaponFire: removed {:08X}'s own projectile {:08X} {} ms after its replayed throw", actorId, ghost->GetFormID(), std::chrono::duration_cast<std::chrono::milliseconds>(now - watch.started).count());
					}
				}
			}
		}
	}

	// --- Copies kept from a friend's explosion (BlastShield) -----------------------------------
	namespace
	{
		BlastShield::Book lending;
		std::uint32_t     lendingLogged = 0;
		std::uint32_t     loanEndsLogged = 0;

		const RE::ActorValueInfo* HealthValue()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->health : nullptr;
		}

		// Lends health to our copies of NPCs another game runs that a friend's explosion at a_at reaches, before it is
		// set off: their owner's game deals its damage to them, and a death of one here would not be believed.
		void LendNear(const RE::NiPoint3& a_at, float a_radius, std::uint32_t a_from)
		{
			const auto health = HealthValue();
			const auto tes = RE::TES::GetSingleton();
			const auto local = NpcSync::LocalPlayer();
			if (!health || !tes || local == 0) {
				return;
			}
			// Nothing is changed while the engine walks its lists: the copies are collected first.
			std::vector<std::uint32_t> copies;
			tes->ForEachReferenceInRange(a_at, a_radius + BlastShield::MARGIN, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && !actor->IsPlayerRef() && Protocol::IsShareableRef(actor->GetFormID()) && !actor->IsDead(false) && !Puppets::IsPuppet(actor) &&
					BlastShield::Shields(NpcSync::Owner(actor->GetFormID()), local)) {
					copies.push_back(actor->GetFormID());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			const auto  now = BlastShield::Clock::now();
			std::string lent;
			std::string held;
			for (const auto id : copies) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
				if (!actor) {
					continue;
				}
				auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
				const float was = values.GetActorValue(*health);
				const float amount = lending.Begin(id, was, now);
				if (amount <= 0.0f) {
					if (lending.Lent(id)) {
						held += std::format(" {:08X}", id);
					}
					continue;
				}
				// A permanent modifier, so healing reaches the lent health too (BlastShield). It raises the health with the
				// maximum; should it raise only the maximum, the copy is healed up to it.
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *health, amount);
				if (values.GetActorValue(*health) < was + amount * 0.5f) {
					values.RestoreActorValue(*health, amount);
				}
				++copiesLent;
				lent += std::format(" {:08X}(player {}'s, {:.0f} -> {:.0f})", id, NpcSync::Owner(id), was, values.GetActorValue(*health));
			}
			if ((!lent.empty() || !held.empty()) && lendingLogged < 40) {
				++lendingLogged;
				REX::INFO("WeaponFire: player {}'s explosion kept from hurting our copies of NPCs other games run:{}{}{}", a_from, lent.empty() ? " none newly" : lent,
					held.empty() ? "" : "; still lent from an earlier one:", held);
			}
		}

		// Ends the copy's loan: it is set to the health it keeps with the loan still in it (what the explosions took
		// comes back), and only then is the loan taken away, from a copy that has more than it (BlastShield).
		void EndLendingOf(std::uint32_t a_id)
		{
			if (!lending.Lent(a_id)) {
				return;
			}
			const auto health = HealthValue();
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
			if (!health || !actor || actor->IsDeleted()) {
				lending.Forget(a_id);
				return;
			}
			auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
			const float had = values.GetActorValue(*health);
			const bool  dead = actor->IsDead(false);
			const auto  release = lending.End(a_id, had, dead);
			if (!release) {
				return;
			}
			const auto setTo = [&](float a_target) {
				const float delta = a_target - values.GetActorValue(*health);
				if (delta < 0.0f) {
					values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *health, delta);
				} else if (delta > 0.0f) {
					values.RestoreActorValue(*health, delta);
				}
			};
			setTo(release->health + release->lent);
			const float set = values.GetActorValue(*health);
			if (!dead && set - release->lent < release->health - 0.5f) {
				// Healing fell short: a damage modifier up instead, so taking the loan away cannot kill it.
				REX::WARN("WeaponFire: healing {:08X} to {:.0f} (with the loan) left it at {:.0f}; raising its damage modifier instead", a_id, release->health + release->lent, set);
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *health, release->health + release->lent - set);
			}
			values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *health, -release->lent);
			const float after = values.GetActorValue(*health);
			if (!dead && !release->kill && had - release->lent < release->health - 0.5f) {
				++blastsUndone;
			}
			if (!dead && !release->kill && after <= 0.0f) {
				REX::WARN("WeaponFire: {:08X} has {:.0f} health after its loan ended", a_id, after);
			}
			if (loanEndsLogged < 40 || release->kill) {
				++loanEndsLogged;
				REX::INFO("WeaponFire: {:08X}'s loan ended (player {}'s game runs it): {:.0f} of its own before, {:.0f} now{}{}", a_id, NpcSync::Owner(a_id), had - release->lent, after,
					dead ? ", dead meanwhile" : "", release->kill ? ", killed by our player's blows" : "");
			}
			if (release->kill) {
				// Our player's blows alone took it to nothing while it had the loan: it dies of them now.
				++lentKilled;
				RE::Console::ExecuteCommand(std::format("{:08X}.kill", a_id).c_str());
			}
		}
	}

	void EndDueLending()
	{
		if (lending.Size() == 0) {
			return;
		}
		for (const auto id : lending.Due(BlastShield::Clock::now())) {
			EndLendingOf(id);
		}
	}

	void OurBlows(const std::vector<std::pair<std::uint32_t, float>>& a_blows)
	{
		if (lending.Size() == 0) {
			return;
		}
		// All of a frame's blows counted first (a shotgun's pellets), then each copy's lending ends once.
		for (const auto& [id, damage] : a_blows) {
			lending.OurBlow(id, damage);
		}
		for (const auto& [id, damage] : a_blows) {
			EndLendingOf(id);
		}
	}

	void EndLending(std::uint32_t a_refId)
	{
		EndLendingOf(a_refId);
	}

	void EndAllLending()
	{
		for (const auto id : lending.Due(BlastShield::Clock::time_point::max())) {
			EndLendingOf(id);
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
		const auto form = RE::TESForm::GetFormByID(a_explosion.base);
		const auto base = form ? form->As<RE::BGSExplosion>() : nullptr;
		if (!cell || !base) {
			return;
		}
		const auto&             us = player->data.location;
		const RemoteEdits::Spot here = cell->IsInterior() ? RemoteEdits::Spot{ cell->GetFormID(), 0, us.x, us.y, us.z } :
		                                                    RemoteEdits::Spot{ 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0, us.x, us.y, us.z };
		// Near us, and only what a throw makes (a grenade, a mine, a Fat Man's shell), not a nuke or an artillery
		// strike (RemoteEdits::SetOff). Near us or our companion it goes off all the same: it is what kills the
		// enemies there in our game.
		const float radius = base->data.outerRadius;
		const auto  blast = RemoteEdits::SetOff(RemoteEdits::SpotOf(a_explosion), here, radius);
		if (blast == RemoteEdits::Blast::kAway) {
			return;
		}
		// The radius is read from the explosion's record by its layout in CommonLib: the first ones go to the
		// log, to show a grenade reads about 500.
		if (explosionsLogged < 5) {
			++explosionsLogged;
			REX::INFO("WeaponFire: explosion {:08X} radius {:.0f} from player {}: {}", a_explosion.base, radius, a_explosion.playerId,
				blast == RemoteEdits::Blast::kSetOff ? "set off" : "not set off, no throw makes one that size");
		}
		if (blast != RemoteEdits::Blast::kSetOff) {
			++explosionRefusedRadius;
			return;
		}
		const RE::NiPoint3 at{ a_explosion.x, a_explosion.y, a_explosion.z };
		LendNear(at, radius, a_explosion.playerId);
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
