#include "game/WeaponFire.h"

#include "game/BlastShield.h"
#include "game/CopyWatch.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/HandThrowable.h"
#include "game/MuzzleFlash.h"
#include "game/AttackKind.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/ThrowReplays.h"

#include "game/ShotLoops.h"

#include "Protocol.h"

namespace WeaponFire
{
	// Defined with the explosion watch below.
	void          ArmGhostWatch(std::uint32_t a_actorId);
	std::uint32_t GhostsRemoved();
	// Defined with the grenade in the stand-in's hand below.
	void        QueueHandCue(std::uint32_t a_actorId, HandThrowable::Cue a_cue);
	void        ArmPlayerThrowSample();
	std::string DescribeHandThrowables();

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

		// A friend's throw replayed on a stand-in or our copy of an NPC plays, but its release is kept from the engine,
		// so nothing leaves the hand (ThrowReplays). The release comes on the animation threads.
		std::mutex                 replayLock;
		ThrowReplays::Book         throwReplays;  // under replayLock
		std::atomic<std::uint32_t> releasesHeld{ 0 };
		std::atomic<std::uint32_t> releasesLogged{ 0 };

		void ArmReplayedThrow(std::uint32_t a_actorId)
		{
			std::scoped_lock l{ replayLock };
			throwReplays.Arm(a_actorId, ThrowReplays::Clock::now());
		}

		// Whether the actor's graph never let go of its replayed throw; it is waited for no longer.
		bool ForgetReplayedThrow(std::uint32_t a_actorId)
		{
			std::scoped_lock l{ replayLock };
			const bool unreleased = throwReplays.Unreleased(a_actorId);
			throwReplays.Forget(a_actorId);
			return unreleased;
		}

		// Whether this release of a throw is a replayed throw's, kept from the engine.
		bool HoldsReplayedRelease(std::uint32_t a_actorId)
		{
			std::optional<ThrowReplays::Clock::duration> after;
			{
				std::scoped_lock l{ replayLock };
				after = throwReplays.Release(a_actorId, ThrowReplays::Clock::now());
			}
			if (!after) {
				return false;
			}
			++releasesHeld;
			if (releasesLogged.fetch_add(1) < 40) {
				REX::INFO("WeaponFire: {:08X} let go of its replayed throw {} ms after the replay; kept from the engine, so no grenade of its own leaves its hand", a_actorId,
					std::chrono::duration_cast<std::chrono::milliseconds>(*after).count());
			}
			return true;
		}

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
		std::uint32_t            copiesShielded = 0;  // our copies of NPCs other games run made ghosts against a friend's explosion (BlastShield)
		std::uint32_t            shieldsEnded = 0;    // ... and no ghosts again
		std::uint32_t            shieldsHurt = 0;     // ... that lost health meanwhile all the same
		std::uint32_t            shieldedDied = 0;    // ... that died meanwhile all the same
		std::uint32_t            ghostsStuck = 0;     // ... still ghosts five frames after we took the ghost off
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
			} else if (IsThrowAttachEvent(tag, payload)) {
				ArmPlayerThrowSample();  // where the engine hangs the grenade on an actor it shows it for
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
			// The wind-up and the release, for the grenade shown in a stand-in's hand (bGrenadeInHand).
			if (HandThrowables()) {
				if (const auto cue = HandThrowable::CueOf(npcTag, npcPayload); cue != HandThrowable::Cue::kNone) {
					QueueHandCue(id, cue);
				}
			}
			// A friend's throw replayed here: the engine is not told the grenade left the hand, so it launches none.
			if (IsThrowEvent(npcTag, npcPayload) && HoldsReplayedRelease(id)) {
				return RE::BSEventNotifyControl::kContinue;
			}
			// A plugin's NPC, or one our game made at runtime that we run under a spawn id (Spawns): a stand-in, a copy of a
			// friend's spawn or one not shared is in no other game to replay it.
			const bool shared = Protocol::IsShareableRef(id) || (Protocol::IsSpawnRef(id) && [&] {
				std::scoped_lock l{ actionLock };
				return ownedNpcs.contains(id);
			}());
			if (shared && IsThrowEvent(npcTag, npcPayload)) {
				std::scoped_lock l{ actionLock };
				// Only an NPC we run: one mirrored from a friend's game threw in theirs, and their game reports what
				// went off. Watched here too, the same explosion could be reported from both games.
				if (ownedNpcs.contains(id)) {
					if (npcActions.size() < MAX_PENDING_ACTIONS) {
						npcActions.push_back({ id, Protocol::ShotAction::kThrow });
					}
					ArmExplosionWatch();
				}
			} else if (shared && IsAttackEvent(npcTag)) {
				std::scoped_lock l{ npcLock };
				if (npcShots.size() < MAX_PENDING_NPC_SHOTS) {
					npcShots.push_back(id);
				}
			} else if (IsThrowAttachEvent(npcTag, npcPayload)) {
				++throwAttaches;
			}
			if (const auto action = shared && a_event.tag.c_str() ? ActionOf(a_event.tag.c_str()) : std::nullopt) {
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
		std::uint32_t                longCut = 0;  // shot sounds still sounding ShotLoops::LONGEST_SHOT after they started, ended then

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
			} else {
				loopingShots.Watch(a_actor->GetFormID(), handle, now);
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
	void UpdateHandThrowables();
	namespace
	{
		void EndDueShields();
	}

	void Frame()
	{
		loopsEnded += static_cast<std::uint32_t>(loopingShots.EndDue(std::chrono::steady_clock::now(), [](RE::BSSoundHandle& a_handle) {
			a_handle.FadeOutAndRelease(LOOP_FADE_MS);
		}));
		loopingShots.EndLong(std::chrono::steady_clock::now(), [](RE::BSSoundHandle& a_handle) {
			if (a_handle.IsPlaying()) {
				a_handle.FadeOutAndRelease(LOOP_FADE_MS);
				++longCut;
			}
		});
		ScanExplosions();
		RemoveGhostGrenades();
		UpdateHandThrowables();
		EndDueShields();
		MuzzleFlash::Frame();
	}

	namespace
	{
		static_assert(std::to_underlying(RE::WEAPON_TYPE::kGun) == AttackKind::GUN_TYPE);

		// Whether one of the actor's equipped weapons is a gun (fists, nothing equipped, count as melee; a grenade beside
		// the gun does not hide it: AttackKind::HoldsGun).
		bool HoldsGun(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return false;
			}
			std::vector<std::int32_t> types;
			RE::BSAutoLock            l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				if (const auto weapon = equipped.item.object ? equipped.item.object->As<RE::TESObjectWEAP>() : nullptr) {
					types.push_back(std::to_underlying(weapon->weaponData.type.get()));
				}
			}
			return AttackKind::HoldsGun(types);
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
		bool animated = false;
		if (replay == AttackKind::Replay::kSoundOnly) {
			++unseen;
		} else if (static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
			animated = true;
		}
		// The engine shows the gun's flash and light with the firing animation; a shot whose animation the graph
		// refused gets the flash from MuzzleFlash instead (nothing is fired: ROADMAP #17).
		if (replay == AttackKind::Replay::kFire) {
			MuzzleFlash::OnShot(a_actor, animated);
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
		// A throw needs no drawn weapon, only the grenade in the actor's hand (Equipment::HoldThrowable), which the
		// throw shows and never lets go of: its release is kept from the engine (ThrowReplays). The owner's explosion
		// comes as its own report.
		if (a_action == Protocol::ShotAction::kThrow) {
			ArmReplayedThrow(a_actor->GetFormID());
			const auto action = RE::TESForm::GetFormByID<RE::BGSAction>(THROW);
			const bool taken = action && a_actor->PerformAction(action, nullptr);
			actionsPlayed += taken;
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
		return std::format("shots={} replayed={}/{} swung={} sounded={} unseen={} theirs={} kssm={:08X}{} loops={}/{} long={} actions={} explosions={}/{} ghosts={} attaches={} loads={} explosionRefusedRadius={} shielded={}/{}/{}/{} ghostsStuck={} held={} inhand={}", total.load(), played, replayed, swung, sounded, unseen, theirs, lastSound, lastLooped ? "(loop)" : "", loopingShots.Active(), loopsEnded, longCut, actionsPlayed, explosionsReported, explosionsSetOff, GhostsRemoved(), throwAttaches.load(), Equipment::AttachesAsked(), explosionRefusedRadius, copiesShielded, shieldsEnded, shieldsHurt, shieldedDied, ghostsStuck, releasesHeld.load(), DescribeHandThrowables()) + " " + MuzzleFlash::Describe();
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
		// Explosion references we know of, each by its id and a handle: the same one only while the handle still names it,
		// as the engine gives a freed reference's id to the next new one, often the next explosion. Kept by id alone (and
		// the reported ones forgotten when a watch ended, while an explosion lingers longer), our own grenades went
		// unreported when their ids had named friends' explosions we placed, and the next throw's watch reported the last
		// explosion again (2026-10-15: 1 report for 3 throws one way, 9 for 7 the other).
		class ExplosionBook
		{
		public:
			[[nodiscard]] bool Has(RE::TESObjectREFR* a_ref) const
			{
				const auto it = refs.find(a_ref->GetFormID());
				return it != refs.end() && it->second.get().get() == a_ref;
			}

			void Add(RE::TESObjectREFR* a_ref)
			{
				if (refs.size() >= MAX) {
					std::erase_if(refs, [](const auto& a_entry) { return a_entry.second.get().get() == nullptr; });
					if (refs.size() >= MAX) {
						refs.clear();
					}
				}
				refs[a_ref->GetFormID()] = a_ref->GetHandle();
			}

		private:
			static constexpr std::size_t MAX = 256;

			std::unordered_map<std::uint32_t, RE::ObjectRefHandle> refs;
		};
		ExplosionBook                          seenExplosions;    // reported as ours
		ExplosionBook                          setOffExplosions;  // the ones we placed for a friend's throw: never reported back as ours
		std::uint32_t                          reportsLogged = 0;
		std::vector<Protocol::Explosion>       explosions;
		std::mutex                             explosionLock;

		// A stand-in that replayed a throw really throws the grenade it holds (the equipment sync gave it
		// one), so the friend's game got a second grenade landing at the stand-in's feet on top of the
		// owner's explosion. Its release is kept from the engine now (ThrowReplays); should a grenade leave
		// its hand all the same, anything the stand-in (or copy) shoots while the release is waited for is removed.
		constexpr auto                                        GHOST_WATCH = ThrowReplays::WINDOW;
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
		loopingShots.EndWatchedFor(a_actorFormId, [](RE::BSSoundHandle& a_handle) {
			if (a_handle.IsPlaying()) {
				a_handle.FadeOutAndRelease(LOOP_FADE_MS);
			}
		});
	}

	void ArmExplosionWatch()
	{
		watchUntil = Clock::now() + EXPLOSION_WATCH;
	}

	void ScanExplosions()
	{
		const auto now = Clock::now();
		if (now > watchUntil.load()) {
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
			// One going away has lost its id first: the last throw's, a 00000000 where it went off 6.5 s before, was
			// reported again by the next throw's watch.
			if (!base || !base->Is(RE::ENUM_FORM_ID::kEXPL) || a_ref->GetFormID() == 0 || a_ref->IsDeleted() || setOffExplosions.Has(a_ref) || seenExplosions.Has(a_ref)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			seenExplosions.Add(a_ref);
			if (reportsLogged < 20) {
				++reportsLogged;
				REX::INFO("WeaponFire: our explosion {:08X} ({:08X}) at ({:.0f},{:.0f},{:.0f}) reported", a_ref->GetFormID(), base->GetFormID(), a_ref->data.location.x,
					a_ref->data.location.y, a_ref->data.location.z);
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
			const bool unreleased = ForgetReplayedThrow(a_entry.first);
			if (a_entry.second.removed == 0 && ghostsLogged < 40) {
				++ghostsLogged;
				REX::INFO("WeaponFire: no projectile of {:08X}'s removed in the {} ms after its replayed throw{}", a_entry.first, std::chrono::duration_cast<std::chrono::milliseconds>(GHOST_WATCH).count(),
					unreleased ? "; its graph never let go of the throw" : "");
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
				REX::INFO("WeaponFire: {:08X} mid-throw holds{} hands{} (attach events so far {})", actorId, Equipment::DescribeHeld(actor), Equipment::DescribeHands(actor), throwAttaches.load());
			}
			// Nothing is changed while the engine walks its lists: the ghosts are collected as handles and
			// dealt with afterwards (PROJECTILE_ENDED), each once.
			std::vector<RE::ObjectRefHandle> ghosts;
			tes->ForEachReferenceInRange(actor->data.location, 1000.0f, [&](RE::TESObjectREFR* a_ref) {
				const auto projectile = a_ref ? a_ref->As<RE::Projectile>() : nullptr;
				if (projectile && !projectile->IsDisabled() && (projectile->flags & PROJECTILE_ENDED) == 0 && projectile->shooter.get().get() == actor) {
					ghosts.push_back(projectile->GetHandle());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto& handle : ghosts) {
				const auto ref = handle.get();
				const auto ghost = ref ? ref->As<RE::Projectile>() : nullptr;
				if (ghost && !ghost->IsDeleted()) {
					ghost->flags |= PROJECTILE_ENDED;
					++ghostsRemoved;
					++watch.removed;
					if (ghostsLogged < 40) {
						++ghostsLogged;
						REX::INFO("WeaponFire: ended {:08X}'s own projectile {:08X} {} ms after its replayed throw", actorId, ghost->GetFormID(), std::chrono::duration_cast<std::chrono::milliseconds>(now - watch.started).count());
					}
				}
			}
		}
	}

	// --- The grenade in a stand-in's hand (HandThrowable.h; bGrenadeInHand, on by default) -------------
	namespace
	{
		std::atomic<bool> handThrowablesOn{ false };   // set from bGrenadeInHand at load (main.cpp)
		std::atomic<bool> handModelFallback{ false };  // the dev's handthrowable model only

		std::mutex                                                handCueLock;
		std::vector<std::pair<std::uint32_t, HandThrowable::Cue>> handCues;  // from the NPC animation hook, drained in Frame
		constexpr std::size_t                                     MAX_HAND_CUES = 64;
		constexpr std::size_t                                     MAX_HAND_MODELS = 16;

		// What hangs, kept alive by us while it hangs: the model (the database's own node, never a copy), the hand it
		// hangs under and the actor's 3D root. With the root held, the whole skeleton the hand is in stays alive when
		// the engine lets go of the actor's 3D (a cell change, a rebuild): the model always comes off a live hand, and
		// the root and hand compared with the actor's current ones are never addresses a new 3D got.
		// When the stand-in's 3D already holds the grenade in a culled WeaponLeft of the engine's own (seen on the rig on
		// every replayed throw: the engine hangs it but draws it only on throws it starts itself), that node is drawn
		// instead (unculled) and culled again on the way out; nothing is hung and nothing shared is touched.
		struct InHand
		{
			RE::NiPointer<RE::NiNode>     model;  // the database's node we hung, or the stand-in's own WeaponLeft we drew
			RE::NiPointer<RE::NiNode>     hand;
			RE::NiPointer<RE::NiAVObject> root;
			Clock::time_point             sampleAt;          // when to log where the model is (zero once logged)
			bool                          unculled = false;  // model is the engine's WeaponLeft, drawn by us
			std::vector<RE::NiPointer<RE::NiAVObject>> innerUnculled;  // ... and those of its children that were culled too
		};
		HandThrowable::Book                                          handBook;
		std::unordered_map<std::uint32_t, InHand>                    inHand;      // actor -> what hangs in its hand
		std::unordered_map<std::uint32_t, RE::NiPointer<RE::NiNode>> handModels;  // throwable -> its model, from the database
		std::uint32_t                                                handsHung = 0;
		std::uint32_t                                                handsDropped = 0;
		std::uint32_t                                                handsLogged = 0;
		constexpr std::uint32_t                                      MAX_HAND_LOGS = 60;
		std::atomic<Clock::time_point>                               playerSampleAt{};

		// The weapon's model as the engine's model database keeps it, loaded as the engine loads a reference's model
		// (from the archives if it is not in memory yet: a small file, on this thread), and kept for the next throw.
		// It is the database's one node for that file, which the engine clones whenever it shows the model itself
		// (round 3 cloned it too, and crashed in BSGeometry::ProcessClone): nothing of it is changed here but where it
		// hangs, so a copy the engine makes meanwhile is the same as ever.
		RE::NiNode* HandModel(RE::TESObjectWEAP* a_weapon)
		{
			const auto id = a_weapon->GetFormID();
			if (const auto it = handModels.find(id); it != handModels.end()) {
				return it->second.get();
			}
			const char* path = a_weapon->GetModel();
			if (!path || !*path || handModels.size() >= MAX_HAND_MODELS) {
				return nullptr;
			}
			RE::BSModelDB::DBTraits::ArgsType args{};
			args.lodFadeMult = RE::ENUM_LOD_MULT::kNone;
			args.loadLevel = 0;
			args.prepareAfterLoad = 1;
			args.performProcess = 1;
			args.loadTextures = 1;
			RE::NiPointer<RE::NiNode> loaded;
			const auto error = RE::BSModelDB::Demand(path, &loaded, args);
			if (error != RE::BSResource::ErrorCode::kNone || !loaded) {
				REX::WARN("WeaponFire: the model of throwable {:08X} did not load (error {})", id, std::to_underlying(error));
				return nullptr;
			}
			handModels.emplace(id, loaded);
			return loaded.get();
		}

		void LogHand(std::string_view a_line)
		{
			if (handsLogged < MAX_HAND_LOGS) {
				++handsLogged;
				REX::INFO("WeaponFire: {}", a_line);
			}
		}

		// The actor's weaponAttach(2) came: the throwable's model goes under its LArm_Hand when HandThrowable says so.
		void HangThrowable(std::uint32_t a_actorId)
		{
			HandThrowable::AtAttach seen;
			seen.enabled = handThrowablesOn;
			{
				std::scoped_lock l{ replayLock };
				seen.replayed = throwReplays.Unreleased(a_actorId);
			}
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_actorId);
			const auto root = actor && !actor->IsDead(false) ? actor->Get3D() : nullptr;
			seen.loaded = root != nullptr;
			const auto hand = Equipment::ChildNode(root, HandThrowable::HAND);
			seen.handFound = hand != nullptr;
			// The engine's own WeaponLeft: drawn (it shows its own), or culled with the grenade in it (ours to draw).
			const auto engines = Equipment::DirectChild(hand, HandThrowable::ENGINE_NODE);
			const auto enginesNode = engines ? engines->IsNode() : nullptr;
			seen.engineShows = engines && !engines->GetAppCulled();
			seen.engineHides = enginesNode && engines->GetAppCulled() && [&] {
				for (const auto& child : enginesNode->children) {
					if (child.get()) {
						return true;
					}
				}
				return false;
			}();
			const auto weapon = seen.loaded ? Equipment::HeldThrowable(actor) : nullptr;
			seen.throwable = weapon ? weapon->GetFormID() : 0;
			seen.modelFallback = handModelFallback;
			const auto skip = handBook.ShouldHang(a_actorId, seen);
			if (skip != HandThrowable::Skip::kNone) {
				if (skip != HandThrowable::Skip::kNotReplayed) {  // an NPC's own throw is no news
					LogHand(std::format("no throwable hung on {:08X}: {}", a_actorId, HandThrowable::Name(skip)));
				}
				return;
			}
			if (seen.engineHides) {
				enginesNode->SetAppCulled(false);
				// What it holds (the grenade's model, one level down) may be culled on its own as well.
				std::vector<RE::NiPointer<RE::NiAVObject>> inner;
				std::string                                 innerSeen;
				for (const auto& child : enginesNode->children) {
					if (const auto object = child.get()) {
						innerSeen += std::format(" '{}'{}", object->name.c_str() ? object->name.c_str() : "", object->GetAppCulled() ? "(culled)" : "");
						if (object->GetAppCulled()) {
							object->SetAppCulled(false);
							inner.emplace_back(object);
						}
					}
				}
				const auto now = Clock::now();
				handBook.Hung(a_actorId, seen.throwable, now, false);
				inHand[a_actorId] = { RE::NiPointer<RE::NiNode>{ enginesNode }, RE::NiPointer<RE::NiNode>{ hand }, RE::NiPointer<RE::NiAVObject>{ root }, now + MID_THROW, true, std::move(inner) };
				++handsHung;
				LogHand(std::format("drew the engine's own {} under {} of {:08X} (throwable {:08X}, holding{})", HandThrowable::ENGINE_NODE, HandThrowable::HAND, a_actorId, seen.throwable, innerSeen));
				return;
			}
			const auto model = HandModel(weapon);
			// A node has one parent: a model hanging anywhere at all is left where it is.
			if (!model || model->parent) {
				LogHand(std::format("no throwable hung on {:08X}: {}", a_actorId, model ? "its model hangs elsewhere" : "no model"));
				return;
			}
			hand->AttachChild(model, true);
			if (model->parent != hand) {
				LogHand(std::format("no throwable hung on {:08X}: the hand did not take its model", a_actorId));
				return;
			}
			const auto now = Clock::now();
			handBook.Hung(a_actorId, seen.throwable, now);
			inHand[a_actorId] = { RE::NiPointer<RE::NiNode>{ model }, RE::NiPointer<RE::NiNode>{ hand }, RE::NiPointer<RE::NiAVObject>{ root }, now + MID_THROW };
			++handsHung;
			LogHand(std::format("hung throwable {:08X}'s model '{}' under {} of {:08X}{}", seen.throwable, model->name.c_str() ? model->name.c_str() : "", HandThrowable::HAND, a_actorId,
				model->GetAppCulled() ? " (culled)" : ""));
		}

		// Takes the model off the hand it was hung under (alive while we hold it, whether or not it is still in the
		// actor's 3D) and lets go of what we held. One moved elsewhere meanwhile is left where it is.
		void TakeOff(std::uint32_t a_actorId, HandThrowable::Drop a_why)
		{
			handBook.Forget(a_actorId);
			const auto it = inHand.find(a_actorId);
			if (it == inHand.end()) {
				return;
			}
			const auto& held = it->second;
			const bool onHand = held.model && held.hand && held.model->parent == held.hand.get();
			if (held.unculled) {
				// The engine's own nodes, culled again as the engine had them.
				for (const auto& inner : held.innerUnculled) {
					inner->SetAppCulled(true);
				}
				held.model->SetAppCulled(true);
			} else if (onHand) {
				held.hand->DetachChild(held.model.get());
			}
			inHand.erase(it);
			++handsDropped;
			LogHand(std::format("took throwable off {:08X}: {}{}", a_actorId, HandThrowable::Name(a_why), onHand ? "" : " (it was no longer on the hand)"));
		}
	}

	bool HandThrowables()
	{
		return handThrowablesOn.load(std::memory_order_relaxed);
	}

	bool HandModelFallback()
	{
		return handModelFallback.load(std::memory_order_relaxed);
	}

	void SetHandThrowables(bool a_on, bool a_modelFallback)
	{
		const bool fallback = a_on && a_modelFallback;
		handThrowablesOn = a_on;
		const bool hadFallback = handModelFallback.exchange(fallback);
		// Turned on, or kept with the database's node allowed as before (or never): nothing hangs that has to come off.
		if (a_on && (fallback || !hadFallback)) {
			return;
		}
		// Off, or the database's node no longer allowed: every grenade comes out of every hand.
		for (const auto actorId : handBook.Actors()) {
			TakeOff(actorId, HandThrowable::Drop::kOff);
		}
		handModels.clear();  // the database's own nodes, held for the next throw
		if (!a_on) {
			std::scoped_lock l{ handCueLock };
			handCues.clear();
		}
	}

	void DropHandThrowable(std::uint32_t a_actorFormId)
	{
		MuzzleFlash::Drop(a_actorFormId);  // called before the actor is deleted, like this
		TakeOff(a_actorFormId, HandThrowable::Drop::kDeleted);
	}

	void DropHandThrowables()
	{
		MuzzleFlash::DropAll();  // before a save or a load, and when the session ends, like this
		for (const auto actorId : handBook.Actors()) {
			TakeOff(actorId, HandThrowable::Drop::kDeleted);
		}
		std::scoped_lock l{ handCueLock };
		handCues.clear();
	}

	void QueueHandCue(std::uint32_t a_actorId, HandThrowable::Cue a_cue)
	{
		std::scoped_lock l{ handCueLock };
		if (handCues.size() < MAX_HAND_CUES) {
			handCues.emplace_back(a_actorId, a_cue);
		}
	}

	void ArmPlayerThrowSample()
	{
		playerSampleAt = Clock::now() + MID_THROW;
	}

	std::string DescribeHandThrowables()
	{
		return HandThrowables() ? std::format("{}/{}{}", handsHung, handsDropped, HandModelFallback() ? "+model" : "") : "off";
	}

	void UpdateHandThrowables()
	{
		std::vector<std::pair<std::uint32_t, HandThrowable::Cue>> cues;
		{
			std::scoped_lock l{ handCueLock };
			cues = std::exchange(handCues, {});
		}
		// In the order the graph sent them: an attach and its release in one batch (a hitch) hang nothing for long.
		for (const auto& [actorId, cue] : cues) {
			if (cue == HandThrowable::Cue::kAttach) {
				HangThrowable(actorId);
			} else {
				handBook.Released(actorId);
			}
		}
		const auto now = Clock::now();
		for (const auto actorId : handBook.Actors()) {
			const auto it = inHand.find(actorId);
			if (it == inHand.end()) {
				handBook.Forget(actorId);
				continue;
			}
			auto& held = it->second;
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(actorId);
			HandThrowable::WhileHeld seen;
			seen.enabled = HandThrowables();
			seen.loaded = actor && !actor->IsDead(false) && actor->Get3D() == held.root.get();
			const auto hand = seen.loaded ? Equipment::ChildNode(held.root.get(), HandThrowable::HAND) : nullptr;
			seen.underHand = hand && hand == held.hand.get() && held.model->parent == hand;
			const auto engines = seen.underHand ? Equipment::DirectChild(hand, HandThrowable::ENGINE_NODE) : nullptr;
			seen.engineShows = engines && engines != held.model.get() && !engines->GetAppCulled();
			const auto weapon = seen.loaded ? Equipment::HeldThrowable(actor) : nullptr;
			seen.throwable = weapon ? weapon->GetFormID() : 0;
			if (const auto drop = handBook.Check(actorId, seen, now); drop != HandThrowable::Drop::kKeep) {
				TakeOff(actorId, drop);
				continue;
			}
			// Where the model is drawn against the hand, once: the skeleton's update has to carry it along.
			if (held.sampleAt != Clock::time_point{} && now >= held.sampleAt) {
				held.sampleAt = {};
				const auto& model = held.model->world.translate;
				const auto& palm = held.hand->world.translate;
				LogHand(std::format("{:08X}'s throwable at ({:.0f},{:.0f},{:.0f}), its hand at ({:.0f},{:.0f},{:.0f}){}", actorId, model.x, model.y, model.z, palm.x, palm.y, palm.z,
					held.model->GetAppCulled() ? ", culled" : ""));
			}
		}
		// The player's own throw, 400 ms in: where the engine hangs a grenade on an actor it shows one for.
		if (const auto at = playerSampleAt.load(); at != Clock::time_point{} && now >= at) {
			playerSampleAt = Clock::time_point{};
			const auto player = RE::PlayerCharacter::GetSingleton();
			REX::INFO("WeaponFire: player mid-throw holds{} hands{}", Equipment::DescribeHeld(player), Equipment::DescribeHands(player));
		}
	}

	// --- Copies kept from a friend's explosion (BlastShield) -----------------------------------
	// Every copy goes by the id the session knows it by (NpcSync::SessionId): a plugin's reference, or the spawn id of our
	// copy of a friend's runtime NPC, as NpcSync names the owner and the end of the mirror.
	namespace
	{
		BlastShield::Book shields;
		std::uint64_t     frames = 0;  // WeaponFire::Frame calls, for the ghost's frames
		std::uint32_t     shieldsLogged = 0;
		std::uint32_t     shieldEndsLogged = 0;
		std::uint32_t     wereGhosts = 0;  // copies left alone, ghosts already (their own story's, not ours to clear)
		// The copies' health when they were made ghosts, for the log when the ghost comes off (a ghost the explosion
		// still hurt shows there).
		std::unordered_map<std::uint32_t, float> healthWhenShielded;
		std::unordered_set<std::uint32_t>        unghostLater;  // ghosts of ours not loaded when they were to come off

		// Our copy under the session's id (nullptr: none here, or gone).
		RE::Actor* CopyOf(std::uint32_t a_id)
		{
			const auto local = NpcSync::LocalRef(a_id);
			const auto actor = local != 0 ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
			return actor && !actor->IsDeleted() ? actor : nullptr;
		}

		// The copy for the log: its form id here, and the spawn id it goes by when that differs.
		std::string Name(std::uint32_t a_id)
		{
			const auto local = NpcSync::LocalRef(a_id);
			return local != 0 && local != a_id ? std::format("{:08X} (spawn {:08X})", local, a_id) : std::format("{:08X}", a_id);
		}

		float HealthOf(RE::Actor* a_actor)
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values && values->health ? static_cast<RE::ActorValueOwner*>(a_actor)->GetActorValue(*values->health) : -1.0f;
		}

		// The actor's ghost flag as its extra data holds it (a flag after the extra data's header): "on", "off", or
		// "none" when it has no such data. For the log, and to leave alone an actor that is a ghost already.
		const char* GhostData(RE::Actor* a_actor)
		{
			const auto extra = a_actor->extraList ? a_actor->extraList->GetByType(RE::EXTRA_DATA_TYPE::kGhost) : nullptr;
			if (!extra) {
				return "none";
			}
			return *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(extra) + sizeof(RE::BSExtraData)) ? "on" : "off";
		}

		bool WasGhost(RE::Actor* a_actor)
		{
			const auto npc = a_actor->GetNPC();
			return (npc && npc->IsGhost()) || std::string_view{ GhostData(a_actor) } == "on";
		}

		// What a copy's ghost reads a frame, and five, after we set it or took it off: whether the console's command
		// takes at once or a frame later, and whether a ghost taken off stays off. For the log.
		struct GhostReadback
		{
			std::uint32_t id = 0;
			bool          on = false;
			std::uint64_t frame = 0;
		};

		constexpr std::uint64_t    READBACK_LAST = 5;
		std::vector<GhostReadback> ghostReadbacks;
		std::uint32_t              readbacksLogged = 0;

		// The console's command on our copy under the session's id a_id (a loaded one: the caller found it).
		void SetGhost(std::uint32_t a_id, bool a_on)
		{
			const auto actor = CopyOf(a_id);
			if (!actor) {
				return;
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.setghost {}", actor->GetFormID(), a_on ? 1 : 0).c_str());
			NpcSync::CountCommands(1);
			if (readbacksLogged < 120) {
				++readbacksLogged;
				REX::INFO("WeaponFire: {}.setghost {} sent, the ghost reads {} at once", Name(a_id), a_on ? 1 : 0, CopyWatch::GhostState(actor));
			}
			if (ghostReadbacks.size() < BlastShield::MAX_SHIELDED) {
				ghostReadbacks.push_back({ a_id, a_on, frames });
			}
		}

		void ReadBackGhosts()
		{
			std::erase_if(ghostReadbacks, [](const GhostReadback& a_readback) {
				const auto after = frames - a_readback.frame;
				if (after != 1 && after != READBACK_LAST) {
					return after > READBACK_LAST;
				}
				const auto actor = CopyOf(a_readback.id);
				const bool still = actor && a_readback.on != actor->GetGhost();
				if (after == READBACK_LAST && still && !a_readback.on) {
					++ghostsStuck;
				}
				if (readbacksLogged < 120 || (after == READBACK_LAST && still)) {
					++readbacksLogged;
					REX::INFO("WeaponFire: {}, {} frame{} after setghost {}: the ghost reads {}{}", Name(a_readback.id), after, after == 1 ? "" : "s", a_readback.on ? 1 : 0,
						CopyWatch::GhostState(actor), still ? ", not yet as set" : "");
				}
				return after == READBACK_LAST;
			});
		}

		// Makes ghosts of our copies of NPCs another game runs that a friend's explosion at a_at reaches, before it is
		// set off: their owner's game deals its damage to them, and a death of one here would not be believed.
		void ShieldNear(const RE::NiPoint3& a_at, float a_radius, std::uint32_t a_from)
		{
			const auto tes = RE::TES::GetSingleton();
			const auto local = NpcSync::LocalPlayer();
			if (!tes || local == 0) {
				return;
			}
			// Nothing is changed while the engine walks its lists: the copies are collected first, by the session's id.
			// Never the player, a stand-in (a puppet), an NPC our game runs, one nobody does, or one never shared
			// (BlastShield::Shields).
			std::vector<std::uint32_t> copies;
			tes->ForEachReferenceInRange(a_at, a_radius + BlastShield::MARGIN, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && !actor->IsPlayerRef() && !Puppets::IsPuppet(actor)) {
					const auto id = NpcSync::SessionId(actor->GetFormID());
					if (BlastShield::Shields({ .id = id, .owner = NpcSync::Owner(id), .dead = actor->IsDead(false) }, local)) {
						copies.push_back(id);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			const auto  now = BlastShield::Clock::now();
			std::string made;
			std::string held;
			for (const auto id : copies) {
				const auto actor = CopyOf(id);
				if (!actor) {
					continue;
				}
				if (!shields.Shielded(id) && WasGhost(actor)) {
					++wereGhosts;
					continue;
				}
				if (!shields.Begin(id, now, frames)) {
					if (shields.Shielded(id)) {
						held += std::format(" {}", Name(id));
					}
					continue;
				}
				SetGhost(id, true);
				++copiesShielded;
				healthWhenShielded[id] = HealthOf(actor);
				made += std::format(" {}(player {}'s, health {:.0f}, ghost {})", Name(id), NpcSync::Owner(id), HealthOf(actor), CopyWatch::GhostState(actor));
			}
			if ((!made.empty() || !held.empty()) && shieldsLogged < 40) {
				++shieldsLogged;
				REX::INFO("WeaponFire: player {}'s explosion passes by our copies of NPCs other games run, made ghosts:{}{}{}", a_from, made.empty() ? " none newly" : made,
					held.empty() ? "" : "; ghosts still from an earlier one:", held);
			}
		}

		// Takes the copy's ghost off (it was put on by ShieldNear), and logs what the explosion took meanwhile: nothing,
		// if ghosts are passed by as they should be.
		void Unshield(std::uint32_t a_id, const char* a_why)
		{
			const auto before = healthWhenShielded.find(a_id);
			const float was = before != healthWhenShielded.end() ? before->second : -1.0f;
			if (before != healthWhenShielded.end()) {
				healthWhenShielded.erase(before);
			}
			const auto actor = CopyOf(a_id);
			if (!actor) {
				// Not loaded now: its ghost comes off when it is again. Our copy of a friend's spawn that is gone for good
				// (Spawns deleted it) is placed afresh, no ghost, should the spawn come back.
				if (NpcSync::LocalRef(a_id) != 0 && unghostLater.size() < BlastShield::MAX_SHIELDED) {
					unghostLater.insert(a_id);
				}
				return;
			}
			SetGhost(a_id, false);
			++shieldsEnded;
			const float now = HealthOf(actor);
			if (now < was - 0.5f) {
				++shieldsHurt;
			}
			if (shieldEndsLogged < 40 || now < was - 0.5f) {
				++shieldEndsLogged;
				REX::INFO("WeaponFire: {} is no ghost again ({}): health {:.0f} then, {:.0f} now{}", Name(a_id), a_why, was, now, actor->IsDead(false) ? ", dead" : "");
			}
		}

		// Deaths in our world (WorldSync's death watch, on whatever thread the engine sends them), by form id, looked at
		// in Frame (a spawn's id is looked up on the main thread).
		std::mutex                 deathLock;
		std::vector<std::uint32_t> deathsSeen;

		// A copy that dies while it is a ghost of ours is one the explosion still reached: counted and logged.
		void ReportShieldedDeaths()
		{
			std::vector<std::uint32_t> deaths;
			{
				std::scoped_lock l{ deathLock };
				deaths.swap(deathsSeen);
			}
			for (const auto formId : deaths) {
				const auto id = NpcSync::SessionId(formId);
				if (id != 0 && shields.Shielded(id)) {
					++shieldedDied;
					const auto actor = CopyOf(id);
					REX::WARN("WeaponFire: our copy {} died while a ghost against a friend's explosion (health {:.0f}, ghost {})", Name(id), actor ? HealthOf(actor) : -1.0f,
						CopyWatch::GhostState(actor));
				}
			}
		}

		void EndDueShields()
		{
			++frames;
			ReadBackGhosts();
			ReportShieldedDeaths();
			std::erase_if(unghostLater, [](std::uint32_t a_id) {
				if (NpcSync::LocalRef(a_id) == 0) {
					return true;  // our copy of a friend's spawn, gone for good
				}
				const auto actor = CopyOf(a_id);
				if (!actor || !actor->Get3D()) {
					return false;
				}
				SetGhost(a_id, false);
				REX::INFO("WeaponFire: {} is no ghost again, loaded once more", Name(a_id));
				return true;
			});
			if (shields.Size() == 0) {
				return;
			}
			// A copy whose NPC is no longer another game's (ours now, or nobody's) is no ghost of ours a frame longer.
			const auto local = NpcSync::LocalPlayer();
			for (const auto id : shields.Ids()) {
				if (!BlastShield::Shields(NpcSync::Owner(id), local) && shields.End(id)) {
					Unshield(id, "no longer another game's");
				}
			}
			for (const auto id : shields.Due(BlastShield::Clock::now(), frames)) {
				Unshield(id, "the explosion is over");
			}
		}
	}

	void NoteDeath(std::uint32_t a_refId)
	{
		std::scoped_lock l{ deathLock };
		if (deathsSeen.size() < 256) {
			deathsSeen.push_back(a_refId);
		}
	}

	void EndShield(std::uint32_t a_refId)
	{
		if (shields.End(a_refId)) {
			Unshield(a_refId, "no longer mirrored");
		}
	}

	void EndAllShields()
	{
		for (const auto id : shields.EndAll()) {
			Unshield(id, "a save, a load or the session's end");
		}
	}

	std::vector<Protocol::Explosion> TakeExplosions()
	{
		std::scoped_lock l{ explosionLock };
		return std::exchange(explosions, {});
	}

	bool SetOffForFriend(RE::TESObjectREFR* a_explosion)
	{
		return a_explosion && setOffExplosions.Has(a_explosion);
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
		ShieldNear(at, radius, a_explosion.playerId);
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
			setOffExplosions.Add(ref.get());
		}
		++explosionsSetOff;
	}
}
