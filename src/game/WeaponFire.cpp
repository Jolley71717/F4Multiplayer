#include "game/WeaponFire.h"

#include "game/BlastShield.h"
#include "game/CopyWatch.h"
#include "game/Destructibles.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/HandThrowable.h"
#include "game/InertShots.h"
#include "game/MuzzleFlash.h"
#include "game/AttackKind.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/ThrowReplays.h"
#include "game/Traps.h"

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
		std::atomic<std::uint32_t> pending{ 0 };        // our shots ("WeaponFire") not taken yet
		std::atomic<std::uint32_t> pendingSwings{ 0 };  // our swings, punches and bashes ("weaponSwing") not taken yet
		std::uint32_t              bashesKept = 0;      // gun bashes (ours and our NPCs') not sent as shots (AttackKind::ReportsAsShot)
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

		std::optional<AttackKind::Event> AttackOf(std::string_view a_tag)
		{
			if (a_tag == "WeaponFire") {
				return AttackKind::Event::kFire;
			}
			if (a_tag == "weaponSwing") {
				return AttackKind::Event::kSwing;
			}
			return std::nullopt;
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

		// A friend's shot replayed on their stand-in: the stand-in's own bullet is kept from the engine
		// (AttackKind::HoldsOwnFire). Armed on the main thread (PlayShot); the shot comes on the animation threads.
		std::mutex                                                               fireLock;
		std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> fireReplays;  // under fireLock
		constexpr std::size_t                                                    MAX_FIRE_REPLAYS = 64;
		std::atomic<std::uint32_t>                                               firesHeld{ 0 };
		std::atomic<std::uint32_t>                                               firesLogged{ 0 };
		std::uint32_t                                                            firesLetGo = 0;  // stand-ins' shots fired as before: a flame, a harpoon
		std::uint32_t                                                            firesInert = 0;  // stand-ins' and copies' shots launched inert: a missile, a shell, an explosive round

		static_assert(AttackKind::PROJECTILE_EXPLOSION == std::to_underlying(RE::BGSProjectile::BGSProjectileFlags::kExplosion));
		static_assert(AttackKind::PROJECTILE_MOTION_MISSILE == std::to_underlying(RE::BGSProjectile::BGSProjectileFlags::kMotionMissile));
		static_assert(AttackKind::PROJECTILE_MOTION_BEAM == std::to_underlying(RE::BGSProjectile::BGSProjectileFlags::kMotionBeam));
		static_assert(AttackKind::PROJECTILE_MOTION_TYPES == std::to_underlying(RE::BGSProjectile::BGSProjectileFlags::kMotionTypes));

		// Whether an actor's gun fires a shot that may go off: its projectile names an explosion (a missile, a mini nuke, a
		// grenade launcher's shell, a beam or a bullet with a blast), or the gun carries an enchantment (a legendary's
		// Explosive round). One test for both ends: the game that runs the shooter watches for its blast after such a shot
		// (CheckShots), and every other game keeps its copy's from the engine (PlayShot), so the blast is the report's alone.
		// Main thread.
		bool ShotGoesOff(RE::Actor* a_actor)
		{
			const auto projectile = MuzzleFlash::GunProjectile(a_actor);
			return (projectile && (projectile->data.explosionType || (projectile->data.flags & AttackKind::PROJECTILE_EXPLOSION) != 0)) ||
			       MuzzleFlash::GunEnchantmentExplodes(a_actor);
		}

		// What becomes of the stand-in's (a_standIn) or the copy's own shot of a replay (AttackKind::ShotReplay): held, the
		// engine's as before, or launched inert (InertShots: one that would go off flies with no blast, so friends see it).
		// Main thread (PlayShot).
		AttackKind::Fire ShotReplayOf(RE::Actor* a_actor, bool a_standIn)
		{
			const auto projectile = MuzzleFlash::GunProjectile(a_actor);
			const bool explodes = projectile && (projectile->data.explosionType || (projectile->data.flags & AttackKind::PROJECTILE_EXPLOSION) != 0);
			return AttackKind::ShotReplay(a_standIn, projectile != nullptr, projectile ? projectile->data.flags : 0, explodes, MuzzleFlash::GunEnchantmentExplodes(a_actor),
				InertShots::Installed());
		}

		// A stand-in has its player's gun and nothing to fire from it: its replayed shot of a gun whose shot flies here
		// (an inert missile, a shell) left the gun with nothing (seen on the rig, and with missiles given it after its
		// launcher was equipped too). Its gun is given a few rounds and loaded before the replay.
		constexpr std::uint32_t SHOT_AMMO = 3;
		std::uint32_t           ammoGiven = 0;
		std::uint32_t           inertReplaysLogged = 0;

		void GiveShotAmmo(RE::Actor* a_actor)
		{
			if (MuzzleFlash::LoadGun(a_actor, SHOT_AMMO)) {
				++ammoGiven;
			}
		}

		// A stand-in's graph refused "attackStart" while it says an animation drives it (bAnimationDriven), standing in no
		// furniture and no power armor frame (seen on the rig right after a join: every replayed shot of its friend's
		// sounded and flashed, none animated, so nothing left its gun). Out of furniture and a frame nothing of a
		// stand-in's is animation-driven (we place it every frame), so the flag is cleared, and the replay is asked once
		// more. A copy's (an NPC's) is left to the engine: its idles are its own (Puppets::ReleaseIdle).
		std::uint32_t animationDrivenCleared = 0;

		bool ReleaseAnimationDriven(RE::Actor* a_actor)
		{
			static const RE::BSFixedString animationDriven{ "bAnimationDriven" };
			if (Puppets::InFurniture(a_actor) || Equipment::InPowerArmor(a_actor)) {
				return false;
			}
			const auto graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			bool       driven = false;
			if (!graph->GetGraphVariableImplBool(animationDriven, driven) || !driven) {
				return false;
			}
			graph->SetGraphVariableBool(animationDriven, false);
			if (++animationDrivenCleared <= 10) {
				REX::INFO("WeaponFire: {:08X}'s graph refused a replayed shot while an animation drove it, in no furniture or frame; the flag cleared and the shot asked again ({} so far)",
					a_actor->GetFormID(), animationDrivenCleared);
			}
			return true;
		}

		void ForgetReplayedFire(std::uint32_t a_actorId)
		{
			std::scoped_lock l{ fireLock };
			fireReplays.erase(a_actorId);
		}

		void ArmReplayedFire(std::uint32_t a_actorId)
		{
			const auto       now = std::chrono::steady_clock::now();
			std::scoped_lock l{ fireLock };
			if (fireReplays.size() >= MAX_FIRE_REPLAYS) {
				std::erase_if(fireReplays, [&](const auto& a_entry) { return now - a_entry.second > AttackKind::HOLD_FIRE_FOR; });
			}
			if (fireReplays.size() < MAX_FIRE_REPLAYS || fireReplays.contains(a_actorId)) {
				fireReplays[a_actorId] = now;
			}
		}

		// Whether this "WeaponFire" is a stand-in's own, after a replayed shot, kept from the engine.
		bool HoldsReplayedFire(std::uint32_t a_actorId, std::string_view a_tag, std::string_view a_payload)
		{
			const auto attack = AttackOf(a_tag);
			if (attack != AttackKind::Event::kFire) {
				return false;
			}
			std::chrono::steady_clock::duration since;
			{
				std::scoped_lock l{ fireLock };
				const auto it = fireReplays.find(a_actorId);
				if (it == fireReplays.end()) {
					return false;
				}
				since = std::chrono::steady_clock::now() - it->second;
			}
			if (!AttackKind::HoldsOwnFire(*attack, IsThrowEvent(a_tag, a_payload), since)) {
				return false;
			}
			++firesHeld;
			if (firesLogged.fetch_add(1) < 20) {
				REX::INFO("WeaponFire: {:08X} fired {} ms after a replayed shot; kept from the engine, so no bullet of its own leaves the gun", a_actorId,
					std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
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
		// Our companions no other game runs (NpcSync's scan), one made here at runtime among them: in no list of the
		// session's, but their throws and shots are ours to report (OursSetItOff).
		std::unordered_set<std::uint32_t> ourCompanions;
		// Who of ours fired a gun lately (our player: PLAYER_REF_ID), for the main thread to arm the shot watch if the gun's
		// shot goes off (CheckShots): the walk the watch makes is no cost of every gun's every shot.
		std::vector<std::uint32_t> shotsToCheck;
		constexpr std::size_t      MAX_SHOTS_TO_CHECK = 64;
		std::vector<std::uint8_t>  playerActions;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> npcActions;
		constexpr std::size_t      MAX_PENDING_ACTIONS = 64;

		// Under actionLock.
		void QueueShotCheck(std::uint32_t a_shooter)
		{
			if (shotsToCheck.size() < MAX_SHOTS_TO_CHECK && std::ranges::find(shotsToCheck, a_shooter) == shotsToCheck.end()) {
				shotsToCheck.push_back(a_shooter);
			}
		}

		std::atomic<bool>        logging{ false };
		std::uint32_t            explosionsReported = 0;  // explosions seen after our throws and sent
		std::uint32_t            explosionsSetOff = 0;    // explosions from other players set off here
		std::uint32_t            explosionsForOurs = 0;   // ... of those, far from our player, beside an NPC our game runs
		std::uint32_t            explosionRefusedRadius = 0;  // ... not set off: no throw makes one that big (or it has no radius)
		std::uint32_t            copiesShielded = 0;  // our copies of NPCs other games run made ghosts against a friend's explosion (BlastShield)
		std::uint32_t            shieldsEnded = 0;    // ... and no ghosts again
		std::uint32_t            shieldsHurt = 0;     // ... that lost health meanwhile all the same
		std::uint32_t            shieldedDied = 0;    // ... that died meanwhile all the same
		std::uint32_t            ghostsStuck = 0;     // ... still ghosts five frames after we took the ghost off
		std::uint32_t            ghostsHiddenFromSaves = 0;  // ghosts of ours whose flag was turned off for a save (HideGhostsFromSave)
		std::uint32_t            ghostsRetried = 0;  // ... and setghost 0 sent again for them (ReadBackGhosts)
		std::uint32_t            explosionsLogged = 0;
		std::mutex               logLock;
		std::vector<std::string> logged;

		RE::BSEventNotifyControl OnPlayerAnimationEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			const std::string_view tag = a_event.tag.c_str() ? a_event.tag.c_str() : "";
			const std::string_view payload = a_event.payload.c_str() ? a_event.payload.c_str() : "";
			if (IsThrowEvent(tag, payload)) {
				{
					std::scoped_lock l{ actionLock };
					if (playerActions.size() < MAX_PENDING_ACTIONS) {
						playerActions.push_back(Protocol::ShotAction::kThrow);
					}
				}
				ArmExplosionWatch(Protocol::PLAYER_REF_ID);  // outside actionLock: the hooks nest no locks
			} else if (const auto attack = AttackOf(tag)) {
				// Which it was is kept for TakeShots: a swing with a gun in hand is a bash, never sent as a shot.
				++(*attack == AttackKind::Event::kFire ? pending : pendingSwings);
				++total;
				if (*attack == AttackKind::Event::kFire) {
					std::scoped_lock l{ actionLock };
					QueueShotCheck(Protocol::PLAYER_REF_ID);  // a missile or a shell goes off a little later, ours to report
				}
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
		struct NpcAttack
		{
			std::uint32_t     id = 0;
			AttackKind::Event event = AttackKind::Event::kFire;
		};
		std::vector<NpcAttack>     npcShots;
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
			// A friend's shot replayed here: the engine is not told the stand-in's gun fired, so it fires no bullet of its own.
			if (HoldsReplayedFire(id, npcTag, npcPayload)) {
				return RE::BSEventNotifyControl::kContinue;
			}
			// A plugin's NPC, or one our game made at runtime that we run under a spawn id (Spawns): a stand-in, a copy of a
			// friend's spawn or one not shared is in no other game to replay it.
			const bool shared = Protocol::IsShareableRef(id) || (Protocol::IsSpawnRef(id) && [&] {
				std::scoped_lock l{ actionLock };
				return ownedNpcs.contains(id);
			}());
			// Ours to report what it sets off: an NPC we run, or our companion no other game runs (one made here at runtime
			// too, in no other game). One mirrored from a friend's game threw or fired in theirs, and their game reports
			// what went off: watched here too, the same explosion could be reported from both games.
			const bool ours = [&] {
				std::scoped_lock l{ actionLock };
				return ownedNpcs.contains(id) || ourCompanions.contains(id);
			}();
			if ((shared || ours) && IsThrowEvent(npcTag, npcPayload)) {
				{
					std::scoped_lock l{ actionLock };
					if (ownedNpcs.contains(id) && npcActions.size() < MAX_PENDING_ACTIONS) {
						npcActions.push_back({ id, Protocol::ShotAction::kThrow });
					}
				}
				if (ours) {
					ArmExplosionWatch(id);
				}
			} else if (const auto attack = shared || ours ? AttackOf(npcTag) : std::nullopt) {
				if (*attack == AttackKind::Event::kFire && ours) {
					std::scoped_lock o{ actionLock };
					QueueShotCheck(id);  // an NPC of ours fired: its missile or shell is ours to report
				}
				std::scoped_lock l{ npcLock };
				if (shared && npcShots.size() < MAX_PENDING_NPC_SHOTS) {
					npcShots.push_back({ id, *attack });
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

	void SetOurCompanions(std::vector<std::uint32_t> a_ids)
	{
		std::scoped_lock l{ actionLock };
		ourCompanions = std::unordered_set<std::uint32_t>(a_ids.begin(), a_ids.end());
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
	std::string DescribeBlasts();
	namespace
	{
		void CheckShots();
	}
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
		CheckShots();
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
		bool       animated = false;
		// A stand-in's plain shot is held: what it hit comes as the shooter's game's report. A stand-in's or a copy's shot
		// that would go off flies, launched inert (InertShots: no blast, no damage of its own), so friends see the missile
		// or the explosive round in flight, and its blast comes as the shooter's game's report, once (docs/DESIGN.md,
		// "Who sets off an explosion"); with no inert launch to be had, it is held too.
		const bool puppet = Puppets::IsPuppet(a_actor);
		const auto fire = replay == AttackKind::Replay::kFire ? ShotReplayOf(a_actor, puppet) : AttackKind::Fire::kFired;
		const bool standIn = fire == AttackKind::Fire::kHeld;
		if (standIn) {
			ArmReplayedFire(a_actor->GetFormID());  // before the graph is told: its "WeaponFire" may come at once
		} else if (replay == AttackKind::Replay::kFire) {
			ForgetReplayedFire(a_actor->GetFormID());  // a pistol's shot just before is no reason to hold this one
			firesLetGo += puppet && fire == AttackKind::Fire::kFired;
		}
		if (fire == AttackKind::Fire::kInert) {
			InertShots::Prepare(MuzzleFlash::GunProjectile(a_actor));  // the inert copy, made here on the main thread
			if (puppet) {
				GiveShotAmmo(a_actor);
			}
			++firesInert;
		}
		if (replay == AttackKind::Replay::kSoundOnly) {
			++unseen;
		} else if (static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
			animated = true;
		} else if (puppet && ReleaseAnimationDriven(a_actor) && static_cast<RE::IAnimationGraphManagerHolder*>(a_actor)->NotifyAnimationGraphImpl(attackStart)) {
			++played;
			animated = true;
		}
		// The engine shows the gun's flash and light when the gun fires; a shot whose animation the graph refused gets
		// the flash from MuzzleFlash instead (ROADMAP #17), and so does a stand-in's plain shot, whose own fire is kept
		// from the engine (HoldsReplayedFire: with it, the engine made no flash either, seen on the rig).
		if (replay == AttackKind::Replay::kFire) {
			MuzzleFlash::OnShot(a_actor, animated && !standIn);
		}
		if (fire == AttackKind::Fire::kInert && inertReplaysLogged < 20) {
			++inertReplaysLogged;
			REX::INFO("WeaponFire: {:08X}'s shot replayed to fly inert: animated={} {}", a_actor->GetFormID(), animated, MuzzleFlash::DescribeLoad(a_actor));
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
		auto       shots = pending.exchange(0);
		const auto swings = pendingSwings.exchange(0);
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (swings == 0) {
			return shots;
		}
		if (player && AttackKind::ReportsAsShot(AttackKind::Event::kSwing, HoldsGun(player))) {
			shots += swings;
		} else {
			bashesKept += swings;
		}
		return shots;
	}

	std::vector<std::uint32_t> TakeNpcShots()
	{
		std::vector<NpcAttack> attacks;
		{
			std::scoped_lock l{ npcLock };
			attacks = std::exchange(npcShots, {});
		}
		std::vector<std::uint32_t> out;
		out.reserve(attacks.size());
		for (const auto& attack : attacks) {
			const auto actor = attack.event == AttackKind::Event::kSwing ? RE::TESForm::GetFormByID<RE::Actor>(attack.id) : nullptr;
			if (AttackKind::ReportsAsShot(attack.event, actor && HoldsGun(actor))) {
				out.push_back(attack.id);
			} else {
				++bashesKept;
			}
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("shots={} bashes={} replayed={}/{} swung={} sounded={} unseen={} theirs={} kssm={:08X}{} loops={}/{} long={} actions={} explosions={}/{} ghosts={} attaches={} loads={} explosionRefusedRadius={} shielded={}/{}/{}/{} ghostsStuck={}/{} ghostsHidden={} held={} firesHeld={} firesLetGo={} firesInert={} ammoGiven={} animDrivenCleared={} inhand={}",total.load(), bashesKept, played, replayed, swung, sounded, unseen, theirs, lastSound, lastLooped ? "(loop)" : "", loopingShots.Active(), loopsEnded, longCut, actionsPlayed, explosionsReported, explosionsSetOff, GhostsRemoved(), throwAttaches.load(), Equipment::AttachesAsked(), explosionRefusedRadius, copiesShielded, shieldsEnded, shieldsHurt, shieldedDied, ghostsStuck, ghostsRetried, ghostsHiddenFromSaves, releasesHeld.load(), firesHeld.load(), firesLetGo, firesInert, ammoGiven, animationDrivenCleared, DescribeHandThrowables()) + " " + DescribeBlasts() + " " + MuzzleFlash::Describe() + " " + InertShots::Describe();
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
		constexpr float FRIEND_REACH = RemoteEdits::MAX_EXPLOSION_RADIUS + 1000.0f;  // around a friend: a blast that can reach them, and their walk since they said where they were
		std::atomic<Clock::time_point>         watchUntil{};  // written from the animation hooks, read in Frame
		std::atomic<Clock::time_point>         shotWatchUntil{};  // after a shot of ours or of an NPC we run: its missile or shell goes off a little later
		std::mutex                             throwLock;
		RemoteEdits::ThrowWatches              throws;  // each thrower's watch, and whether its explosions were all reported (under throwLock)
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

	namespace
	{
		// What became of each explosion we saw (RemoteEdits::BlastLog): the hits on copies are judged by it (BlastAt).
		RemoteEdits::BlastLog    blasts;
		// Where a hit of ours on a destructible object watches for an explosion it sets off (ArmBlastWatch).
		RemoteEdits::AreaWatches areaWatches;
		// Whether the server is sure to pass a report of ours on (RemoteEdits::ReportGate): only then is it sent.
		RemoteEdits::ReportGate  reportGate;
		std::uint32_t            explosionsNotShared = 0;  // seen while a watch was on, but out of its reach: ours alone
		std::uint32_t            explosionsOfStandIns = 0; // a friend's stand-in's own: it fired one after all (it holds them)
		std::uint32_t            explosionsOfOthers = 0;   // an NPC's another game runs, or nobody does: never reported from here
		std::uint32_t            explosionsKept = 0;       // ours, but the server might not pass it on: not sent, ours alone
		std::uint32_t            explosionsNearFriends = 0;  // ours, found around a friend far from our player
		std::uint32_t            deathBlasts = 0;            // an actor's death blast: every game's copy sets its own off
		std::unordered_map<std::uint32_t, bool> deathBlastBases;  // RemoteEdits::DEATH_BLASTS by the IDs this game has them under: unowned only
		bool                              deathBlastsResolved = false;

		// Whether an explosion's base is a death blast's (RemoteEdits::DEATH_BLASTS). Main thread.
		bool DeathBlastBase(const RE::TESForm* a_base, bool a_owned)
		{
			if (!deathBlastsResolved) {
				if (const auto handler = RE::TESDataHandler::GetSingleton()) {
					for (const auto& base : RemoteEdits::DEATH_BLASTS) {
						if (const auto id = handler->LookupFormID(base.id, base.file)) {
							deathBlastBases.emplace(id, base.unownedOnly);
						}
					}
					deathBlastsResolved = !deathBlastBases.empty();  // else asked again: the game's data may not be loaded yet
				}
			}
			const auto found = a_base ? deathBlastBases.find(a_base->GetFormID()) : deathBlastBases.end();
			return found != deathBlastBases.end() && (!found->second || !a_owned);
		}

		std::uint32_t            blastsNamed = 0;          // blows on copies put down to the explosion their hit names
		std::uint32_t            blastsNearest = 0;        // ... to the nearest that reached them, the hit naming none we saw
		std::uint32_t            blastsAmbiguous = 0;      // ... where two reached them that fared differently: taken as ours

		RemoteEdits::Spot SpotOfRef(const RE::TESObjectREFR* a_ref)
		{
			const auto  cell = a_ref->GetParentCell();
			const auto& at = a_ref->data.location;
			if (!cell) {
				return { 0, 0, at.x, at.y, at.z };
			}
			return cell->IsInterior() ? RemoteEdits::Spot{ cell->GetFormID(), 0, at.x, at.y, at.z } :
			                            RemoteEdits::Spot{ 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0, at.x, at.y, at.z };
		}

		// An explosion reference not yet looked at: never one we set off for a friend, nor one going away (it lost its id
		// first: the last throw's, a 00000000 where it went off 6.5 s before, was reported again by the next throw's watch).
		bool Fresh(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
			return base && base->Is(RE::ENUM_FORM_ID::kEXPL) && a_ref->GetFormID() != 0 && !a_ref->IsDeleted() && !setOffExplosions.Has(a_ref) &&
			       !seenExplosions.Has(a_ref);
		}

		// Whether an actor that set off an explosion is ours: our player, or an NPC our game runs, our companion among them
		// (our game claims it, and one made here at runtime is in no list of the session's: seen on the rig) unless another
		// game runs it. Main thread.
		bool OursSetItOff(RE::Actor* a_by)
		{
			if (a_by->IsPlayerRef()) {
				return true;
			}
			{
				std::scoped_lock l{ actionLock };
				if (ownedNpcs.contains(a_by->GetFormID())) {
					return true;
				}
			}
			if (Puppets::IsPuppet(a_by) || !NpcSync::IsOurCompanion(a_by)) {
				return false;
			}
			const auto session = NpcSync::SessionId(a_by->GetFormID());
			const auto owner = session ? NpcSync::Owner(session) : 0;
			return owner == 0 || owner == NpcSync::LocalPlayer();
		}

		// How far an explosion is from the nearest player it could hurt: ours, or a friend whose game we know is in our
		// space (RemotePlayers: where they last said they were). An explosion of ours is reported wherever any player could
		// be hit by it, as only their own game deals their blows; the server passes on one near any player
		// (Protocol::REPORTED_EXPLOSION_RANGE), and ReportGate::RANGE is that less a margin.
		float NearestPlayer(const RE::NiPoint3& a_at, const RE::NiPoint3& a_player)
		{
			float nearest = a_at.GetDistance(a_player);
			for (const auto& info : RemotePlayers::List()) {
				if (const auto at = RemotePlayers::PositionOf(info.id)) {
					nearest = (std::min)(nearest, a_at.GetDistance(*at));
				}
			}
			return nearest;
		}

		// What becomes of a fresh explosion, decided once and kept (blasts), by who set it off (docs/DESIGN.md, "Who sets
		// off an explosion"). A destructible object's stage's own (by its base: a car we shot) is the object's: every game
		// sets its own off with the stage (Destructibles). A world trap's own (a mine, a grenade bouquet's grenades, near a
		// trap that went off and set off by no actor): every game sets its own off (Traps), so reported too it would go off
		// twice. A death blast (by its base: RemoteEdits::DEATH_BLASTS, a robot's or a turret's self-destruct and the limbs it
		// blows apart): every game's copy of its NPC sets its own off as it dies. Ours (our player's or an NPC's we run:
		// the explosion's owner, or, with none named, one while a throw's watch is on or within a destructible object's
		// watch) is reported for each friend's game to set off, when the
		// server is sure to pass it on (ReportGate, from the nearest player); else it is ours alone, and so are its blows on
		// copies. A friend's stand-in's own, or a mirrored NPC's, should never go off here (their shots that may go off
		// are held, and so are their throws): their own game reports it.
		void Decide(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_player, Clock::time_point a_now)
		{
			seenExplosions.Add(a_ref);
			const auto base = a_ref->GetObjectReference();
			const auto record = base->As<RE::BGSExplosion>();
			RemoteEdits::Seen seen{ SpotOfRef(a_ref), record ? record->data.outerRadius : 0.0f, RemoteEdits::Fate::kOurs, a_now, a_ref->GetFormID() };
			const auto        explosion = RE::fallout_cast<RE::Explosion*>(a_ref);
			const auto        by = explosion ? explosion->GetActorOwner() : nullptr;
			const bool        thrown = a_now <= watchUntil.load();
			const bool        watched = thrown || areaWatches.Covers(seen.at, a_now);
			const char*       what = "ours alone";
			if (by && Puppets::IsPuppet(by)) {
				++explosionsOfStandIns;
				what = "a stand-in's own: its player's game reports it";
			} else if (Destructibles::ClaimsExplosion(a_ref)) {
				seen.fate = RemoteEdits::Fate::kEachOwn;  // by its base: the stage's own explosion
				what = "a destructible object's own: every game sets its own off with its stage";
			} else if (Traps::Claims(a_ref->data.location, by != nullptr)) {
				seen.fate = RemoteEdits::Fate::kEachOwn;  // near a trap that went off, and no actor's
				what = "a trap's: every game sets its own off";
			} else if (DeathBlastBase(base, by != nullptr)) {
				seen.fate = RemoteEdits::Fate::kEachOwn;  // by its base: a robot's or a turret's self-destruct, or a limb it blows apart
				what = "a death blast: every game's copy of its NPC sets its own off";
				++deathBlasts;
			} else if (by ? OursSetItOff(by) : watched) {
				if (reportGate.Pass(NearestPlayer(a_ref->data.location, a_player), seen.radius, a_now)) {
					seen.fate = RemoteEdits::Fate::kShared;
					what = "reported";
					std::scoped_lock e{ explosionLock };
					explosions.push_back({ 0, base->GetFormID(), seen.at.x, seen.at.y, seen.at.z, seen.at.cell, seen.at.worldspace });
					++explosionsReported;
				} else {
					++explosionsKept;
					what = "ours, kept: the server might not pass it on (too far, too big, or past the budget)";
					if (thrown) {
						std::scoped_lock t{ throwLock };
						throws.Unshare(by ? by->GetFormID() : 0, a_now);  // a Molotov's fire, if it was one, burns here alone
					}
				}
			} else if (by) {
				++explosionsOfOthers;
				what = "an NPC's our game doesn't run: never reported from here";
			} else if (a_now <= watchUntil.load() || areaWatches.Any(a_now)) {
				++explosionsNotShared;
			}
			blasts.Note(seen);
			if (reportsLogged < 30) {
				++reportsLogged;
				REX::INFO("WeaponFire: our explosion {:08X} ({:08X}) at ({:.0f},{:.0f},{:.0f}) radius {:.0f}, set off by {:08X}: {}", a_ref->GetFormID(), base->GetFormID(), seen.at.x,
					seen.at.y, seen.at.z, seen.radius, by ? by->GetFormID() : 0, what);
			}
		}
	}

	void ArmExplosionWatch(std::uint32_t a_thrower)
	{
		const auto until = Clock::now() + EXPLOSION_WATCH;
		{
			std::scoped_lock l{ throwLock };
			throws.Arm(a_thrower, until);
		}
		watchUntil = until;
	}

	void ArmShotWatch()
	{
		shotWatchUntil = Clock::now() + EXPLOSION_WATCH;
	}

	namespace
	{
		std::uint32_t shotsChecked = 0, shotsWatched = 0;

		// The guns of ours that fired since the last frame (the animation hooks queue them): the shot watch is armed for
		// one whose shot may go off (a missile, a shell, an explosive or enchanted round), not for every gun's every shot.
		// Main thread.
		void CheckShots()
		{
			std::vector<std::uint32_t> shooters;
			{
				std::scoped_lock l{ actionLock };
				shooters.swap(shotsToCheck);
			}
			for (const auto id : shooters) {
				++shotsChecked;
				const auto actor = id == Protocol::PLAYER_REF_ID ? RE::PlayerCharacter::GetSingleton() : RE::TESForm::GetFormByID<RE::Actor>(id);
				if (actor && ShotGoesOff(actor)) {
					++shotsWatched;
					ArmShotWatch();
				}
			}
		}
	}

	void ArmBlastWatch(RE::TESObjectREFR* a_object, float a_radius)
	{
		if (a_object) {
			areaWatches.Arm(SpotOfRef(a_object), a_radius, Clock::now() + EXPLOSION_WATCH);
		}
	}

	bool ExplosionsShared(std::uint32_t a_thrower)
	{
		std::scoped_lock l{ throwLock };
		return throws.Shared(a_thrower, Clock::now());
	}

	bool ThrewLately(std::uint32_t a_thrower)
	{
		std::scoped_lock l{ throwLock };
		return throws.Shared(a_thrower, Clock::now(), THREW_LATELY);
	}

	std::optional<RemoteEdits::Seen> BlastAt(RE::TESObjectREFR* a_target, std::uint32_t a_source)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto tes = RE::TES::GetSingleton();
		if (!a_target || !player || !tes) {
			return std::nullopt;
		}
		const auto now = Clock::now();
		// The explosion may not have been looked at yet (the scan comes every 100 ms, and only while a watch is on): it is
		// decided now, as the scan would have, so the hit and the report agree.
		std::vector<RE::TESObjectREFR*> fresh;
		tes->ForEachReferenceInRange(a_target->data.location, RemoteEdits::MAX_EXPLOSION_RADIUS + RemoteEdits::BlastLog::REACH, [&](RE::TESObjectREFR* a_ref) {
			if (Fresh(a_ref)) {
				fresh.push_back(a_ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		for (const auto ref : fresh) {
			Decide(ref, player->data.location, now);  // after the walk: a destructible's claim walks the references around it
		}
		bool       ambiguous = false;
		const auto seen = blasts.At(SpotOfRef(a_target), now, a_source, &ambiguous);
		if (seen) {
			++(a_source != 0 && seen->source == a_source ? blastsNamed : ambiguous ? blastsAmbiguous : blastsNearest);
		}
		return seen;
	}

	void ScanExplosions()
	{
		const auto now = Clock::now();
		blasts.Prune(now);
		areaWatches.Prune(now);
		if (now > watchUntil.load() && now > shotWatchUntil.load() && !areaWatches.Any(now)) {
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
		std::vector<RE::TESObjectREFR*> fresh;
		RE::TES::GetSingleton()->ForEachReferenceInRange(player->data.location, EXPLOSION_RANGE, [&](RE::TESObjectREFR* a_ref) {
			if (Fresh(a_ref)) {
				fresh.push_back(a_ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		for (const auto ref : fresh) {
			Decide(ref, player->data.location, now);
		}
		// An NPC of ours can set one off far from our player but beside a friend's (it fights them there, and the copies
		// in their game hold its explosive shots): looked for around each friend in our space too. Only one an actor of
		// ours set off is taken from there; any other there is the friend's game's to see.
		for (const auto& info : RemotePlayers::List()) {
			const auto at = RemotePlayers::PositionOf(info.id);
			if (!at || at->GetDistance(player->data.location) + FRIEND_REACH <= EXPLOSION_RANGE) {
				continue;  // within our own walk
			}
			std::vector<RE::TESObjectREFR*> near;
			RE::TES::GetSingleton()->ForEachReferenceInRange(*at, FRIEND_REACH, [&](RE::TESObjectREFR* a_ref) {
				const auto explosion = Fresh(a_ref) ? RE::fallout_cast<RE::Explosion*>(a_ref) : nullptr;
				const auto by = explosion ? explosion->GetActorOwner() : nullptr;
				if (by && !Puppets::IsPuppet(by) && OursSetItOff(by)) {
					near.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto ref : near) {
				Decide(ref, player->data.location, now);
				++explosionsNearFriends;
			}
		}
	}

	std::string DescribeBlasts()
	{
		return std::format("blasts={} areaWatches={} explosionsNotShared={} standInExplosions={} othersExplosions={} explosionsKept={} explosionsForOurs={} blastsNamed={}/{}/{} shotsWatched={}/{} nearFriends={} deathBlasts={}", blasts.Size(),
			areaWatches.Size(), explosionsNotShared, explosionsOfStandIns, explosionsOfOthers, explosionsKept, explosionsForOurs, blastsNamed, blastsNearest, blastsAmbiguous, shotsWatched,
			shotsChecked, explosionsNearFriends, deathBlasts);
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
		std::vector<std::uint32_t>               hiddenFromSave;  // ghosts whose flag HideGhostsFromSave turned off for the save (AfterSave)

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

		// Sets the ghost flag in the actor's extra data itself, which a save writes (the console's setghost takes a frame,
		// and keeps its own count of the ghost: see AfterSave). Whether there was one to change.
		bool SetGhostData(RE::Actor* a_actor, bool a_on)
		{
			const auto extra = a_actor->extraList ? a_actor->extraList->GetByType(RE::EXTRA_DATA_TYPE::kGhost) : nullptr;
			if (!extra) {
				return false;
			}
			auto& flag = *reinterpret_cast<bool*>(reinterpret_cast<std::uintptr_t>(extra) + sizeof(RE::BSExtraData));
			return std::exchange(flag, a_on) != a_on;
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

		// Copies still ghosts five frames after their setghost 0, sent it again (how many times, up to GHOST_RETRIES).
		constexpr std::uint8_t                           GHOST_RETRIES = 3;
		std::unordered_map<std::uint32_t, std::uint8_t> ghostRetries;

		void ReadBackGhosts()
		{
			std::vector<std::uint32_t> again;
			std::erase_if(ghostReadbacks, [&](const GhostReadback& a_readback) {
				const auto after = frames - a_readback.frame;
				if (after != 1 && after != READBACK_LAST) {
					return after > READBACK_LAST;
				}
				const auto actor = CopyOf(a_readback.id);
				const bool still = actor && a_readback.on != actor->GetGhost();
				if (after == READBACK_LAST && !a_readback.on) {
					if (!still) {
						ghostRetries.erase(a_readback.id);
					} else {
						++ghostsStuck;
						// A setghost 1 sent in the frame a save began landed after the setghost 0 that followed it
						// (the rig: both sent before either landed, the copy a ghost for good): the ghost is taken off
						// again, unless a new explosion made it one meanwhile.
						if (!shields.Shielded(a_readback.id) && ghostRetries[a_readback.id]++ < GHOST_RETRIES) {
							again.push_back(a_readback.id);
						}
					}
				}
				if (readbacksLogged < 120 || (after == READBACK_LAST && still)) {
					++readbacksLogged;
					REX::INFO("WeaponFire: {}, {} frame{} after setghost {}: the ghost reads {}{}", Name(a_readback.id), after, after == 1 ? "" : "s", a_readback.on ? 1 : 0,
						CopyWatch::GhostState(actor), still ? ", not yet as set" : "");
				}
				return after == READBACK_LAST;
			});
			for (const auto id : again) {
				// A flag that reads off on a copy the engine still counts a ghost is put back on first: the console's
				// setghost 0 leaves a flag that reads off alone (AfterSave).
				if (const auto actor = CopyOf(id)) {
					SetGhostData(actor, true);
				}
				++ghostsRetried;
				REX::INFO("WeaponFire: {} still a ghost five frames after its setghost 0, sent again", Name(id));
				SetGhost(id, false);
			}
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
			if (!hiddenFromSave.empty()) {
				AfterSave();  // the save's own message had it done already, unless it came off the main thread
			}
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

	void HideGhostsFromSave()
	{
		// The console's setghost 0 lands a frame after it is sent, and the save is written in this frame: the ghost went
		// into the save, and whoever loaded it (a player who died loads their last save) found those enemies ghosts for
		// good, their shots passing through; as ghosts already, they were never ours to clear again. So the flag the save
		// writes is turned off for it: on the copies whose setghost 0 went in the last frames and hasn't landed
		// (EndAllShields and NpcSync::ReleaseMirrors end every shield just before this), and on those whose ghost was to
		// come off once they loaded again. AfterSave puts it back on.
		const auto hide = [](std::uint32_t a_id) {
			if (const auto actor = CopyOf(a_id); actor && SetGhostData(actor, false) && std::ranges::find(hiddenFromSave, a_id) == hiddenFromSave.end()) {
				hiddenFromSave.push_back(a_id);
			}
		};
		for (const auto& readback : ghostReadbacks) {
			if (!readback.on) {
				hide(readback.id);
			}
		}
		std::ranges::for_each(unghostLater, hide);
		if (!hiddenFromSave.empty()) {
			ghostsHiddenFromSaves += static_cast<std::uint32_t>(hiddenFromSave.size());
			REX::INFO("WeaponFire: {} ghost{} against friends' explosions kept out of the save", hiddenFromSave.size(), hiddenFromSave.size() == 1 ? "" : "s");
		}
	}

	void AfterSave()
	{
		// The console's setghost 0 finds a ghost whose flag reads off already and leaves it: the engine still counted the
		// copy a ghost (the rig: GetGhost on, the flag off, five frames on). So the flag goes back on and the command goes
		// again, to take the ghost off as it does. One whose command landed meanwhile has no flag left to put back; one
		// not loaded is unghosted as it loads again (unghostLater).
		std::uint32_t again = 0;
		for (const auto id : std::exchange(hiddenFromSave, {})) {
			const auto actor = CopyOf(id);
			if (!actor || !SetGhostData(actor, true)) {
				continue;
			}
			if (!unghostLater.contains(id)) {
				SetGhost(id, false);
				++again;
			}
		}
		if (again > 0) {
			REX::INFO("WeaponFire: after the save, setghost 0 sent again to {} cop{}", again, again == 1 ? "y" : "ies");
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

	namespace
	{
		// Whether an NPC our game runs (our companion among them) stands within a_reach of a_at, in the cells loaded here: a
		// friend's explosion there is ours to set off for it, wherever our player is (RemoteEdits::SetOff).
		bool OursNear(const RE::NiPoint3& a_at, float a_reach)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes || !std::isfinite(a_at.x) || !std::isfinite(a_at.y) || !std::isfinite(a_at.z)) {
				return false;
			}
			std::vector<std::uint32_t> actors;
			bool                       companion = false;
			tes->ForEachReferenceInRange(a_at, a_reach, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && !actor->IsPlayerRef() && !Puppets::IsPuppet(actor) && !actor->IsDead(false)) {
					actors.push_back(actor->GetFormID());
					companion = companion || NpcSync::IsOurCompanion(actor);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			if (companion) {
				return true;
			}
			std::scoped_lock l{ actionLock };
			return std::ranges::any_of(actors, [](std::uint32_t a_id) { return ownedNpcs.contains(a_id); });
		}
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
		// Near us, or beside an NPC our game runs (the reporter left its blow to us), and only what a throw makes (a grenade,
		// a mine, a Fat Man's shell), not a nuke or an artillery strike (RemoteEdits::SetOff). Near us or our companion it
		// goes off all the same: it is what kills the enemies there in our game.
		const float radius = base->data.outerRadius;
		const auto  spot = RemoteEdits::SpotOf(a_explosion);
		const bool  far = !RemoteEdits::Within(RemoteEdits::Distance(spot, here), RemoteEdits::EXPLOSION_RANGE);
		const bool  oursNear = far && RemoteEdits::SameSpace(spot, here) && RemoteEdits::Throwable(radius) &&
		                      OursNear({ a_explosion.x, a_explosion.y, a_explosion.z }, radius + RemoteEdits::OURS_NEAR_MARGIN);
		const auto  blast = RemoteEdits::SetOff(spot, here, radius, oursNear);
		if (oursNear) {
			++explosionsForOurs;
		}
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
		// One whose record spawns projectiles (a MIRV pack's children, a Sentry Bot's cluster) spawns them here too, as the
		// reference is created, with no shooter; the shooter's game reports their blasts, so the ones spawned here fly
		// inert (InertShots): told before the creation, as the engine launches them inside it (seen on the rig: a MIRV
		// pack's five children launched in the same millisecond, before the placing returned, and went off twice).
		InertShots::ExpectSpawned(base->data.spawnProjectile);
		const auto handle = EngineRefs::CreateReferenceAtLocation(data);
		if (const auto ref = handle.get()) {
			// A friend's explosion we just placed is in range of our own scan after our next throw: it
			// was reported back as ours, and went off a second time for everyone.
			setOffExplosions.Add(ref.get());
		}
		++explosionsSetOff;
	}

	void SetOffTrapExplosion(RE::BGSExplosion* a_explosion, const RE::NiPoint3& a_at, std::uint32_t a_from)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell || !a_explosion) {
			return;
		}
		ShieldNear(a_at, a_explosion->data.outerRadius, a_from);
		RE::NEW_REFR_DATA data;
		data.location = a_at;
		data.object = a_explosion;
		data.interior = cell->IsInterior() ? cell : nullptr;
		data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
		data.clearStillLoadingFlag = true;
		const auto handle = EngineRefs::CreateReferenceAtLocation(data);
		if (const auto ref = handle.get()) {
			setOffExplosions.Add(ref.get());
		}
		++explosionsSetOff;
	}

	void ShieldCopiesNear(const RE::NiPoint3& a_at, float a_radius, std::uint32_t a_from)
	{
		ShieldNear(a_at, a_radius, a_from);
	}
}
