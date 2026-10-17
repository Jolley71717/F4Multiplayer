#include "game/Puppets.h"

#include "game/AimPoint.h"
#include "game/CreatureGait.h"
#include "game/Gait.h"
#include "game/IdleRelease.h"
#include "game/MoveSync.h"
#include "game/Papyrus.h"

#include "Protocol.h"

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
			Kind                                  kind = Kind::kPlayer;
			std::uint32_t                         formId = 0;  // to tell a freed actor's address apart from a new actor
			std::uint32_t                         originalFlags = 0;  // SUPPRESSED_BEHAVIOR bits the actor had before
			Motion                                target;
			bool                                  hasTarget = false;
			bool                                  moving = false;  // locomotion graph is in its moving state
			Gait::Kind                            gait = Gait::Kind::kWalk;  // the gait its graph was last told (Gait.h)
			float                                 gaitSpeed = 0.0f;           // the smoothed speed it goes by
			std::int32_t                          range = -1;  // the gait a dog's graph was last told (CreatureGait.h), -1 none
			std::chrono::steady_clock::time_point gaitAt{};
			std::uint8_t                          appliedFlags = 0;  // flags whose animation events were sent
			std::chrono::steady_clock::time_point aiUntil{};        // the engine's own update (with AI) runs until then: furniture use (power armor) needs it
			std::chrono::steady_clock::time_point nextIdleRefresh{};
			std::chrono::steady_clock::time_point nextDrawAttempt{};
			bool                                  redraw = false;  // holster (if drawn) and draw again
			IdleRelease::Gate                     idleRelease;     // a mirrored NPC held by an idle (see ReleaseIdle)
			std::uint32_t                         idleStops = 0;   // stops sent, and taken by the graph (dev: mirror)
			std::uint32_t                         idleStopsTaken = 0;
			std::uint32_t                         idleGiveUps = 0;   // stuck spells whose stops all went unheeded
			std::chrono::steady_clock::time_point nextResync{};      // DriveAnimation tells the graph again then whether it moves
			std::uint32_t                         resyncsTaken = 0;  // of those, taken: the graph was in the other state (dev: mirror)
			std::uint32_t                         seatExits = 0;     // stood up out of furniture (see ReleaseIdle)
			bool                                  held = false;      // held for furniture: neither placed nor driven (see Hold)
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

		// The point an NPC's weapon aim turns its arms toward (AimPoint.h). Actor::SetLookAtTarget leaves it in
		// the actor's movement state (+C9CB00; FLT_MIN takes the race's own turn rate, then 1 and 1, as
		// AIProcess::SetHeadTrackPathingFaceTarget passes), the aim modifier reads it back through +C9CC80, and
		// Actor::ClearLookAtTarget forgets it (+C9CF00).
		constexpr REL::ID SET_LOOK_AT{ 2230283 };
		constexpr REL::ID LOOK_AT_POINT{ 2230288 };
		constexpr REL::ID CLEAR_LOOK_AT{ 2230291 };

		// The engine reaches the point through the actor's mover and movement controller, which it does not
		// check for: an actor without them (no 3D yet) is left alone.
		bool CanAim(const RE::Actor* a_actor)
		{
			return a_actor->actorMover != nullptr && a_actor->movementController.get() != nullptr;
		}

		void SetAimPoint(RE::Actor* a_actor, const AimPoint::Point& a_point)
		{
			if (!CanAim(a_actor)) {
				return;
			}
			static REL::Relocation<void (*)(RE::Actor*, RE::NiPoint3&, float, float, float)> setLookAt{ SET_LOOK_AT };
			RE::NiPoint3 point{ a_point.x, a_point.y, a_point.z };
			setLookAt(a_actor, point, std::numeric_limits<float>::min(), 1.0f, 1.0f);
		}

		std::optional<AimPoint::Point> GetAimPoint(const RE::Actor* a_actor)
		{
			if (!CanAim(a_actor)) {
				return std::nullopt;
			}
			static REL::Relocation<bool (*)(const RE::Actor*, RE::NiPoint3&)> lookAtPoint{ LOOK_AT_POINT };
			RE::NiPoint3 point;
			if (!lookAtPoint(a_actor, point)) {
				return std::nullopt;
			}
			return AimPoint::Point{ point.x, point.y, point.z };
		}

		void ClearAimPoint(RE::Actor* a_actor)
		{
			if (!CanAim(a_actor)) {
				return;
			}
			static REL::Relocation<void (*)(RE::Actor*)> clearLookAt{ CLEAR_LOOK_AT };
			clearLookAt(a_actor);
		}

		// A stand-in aims where its owner aims (AimPoint.h), from where it stands now. A mirrored NPC aims at the
		// pitch its owner's copy aims at, and lowers its gun when that copy aims at nothing (AimPoint::ForMirror).
		void Aim(RE::Actor* a_actor, const Puppet& a_puppet)
		{
			const auto&           at = a_actor->data.location;
			const AimPoint::Point feet{ at.x, at.y, at.z };
			if (a_puppet.kind == Kind::kNpc) {
				const auto order = AimPoint::ForMirror(a_puppet.target.ownerAim, GetAimPoint(a_actor).has_value(), feet, a_puppet.target.heading);
				if (order.act == AimPoint::MirrorOrder::Act::kSet) {
					SetAimPoint(a_actor, order.point);
				} else if (order.act == AimPoint::MirrorOrder::Act::kClear) {
					ClearAimPoint(a_actor);
				}
				return;
			}
			if (const auto point = AimPoint::ForPuppet(true, feet, a_puppet.target.heading, a_puppet.target.pitch)) {
				SetAimPoint(a_actor, *point);
			}
		}

		// The furniture the NPC uses (sits or lies on, or still holds getting off it), or null.
		RE::NiPointer<RE::TESObjectREFR> OccupiedFurniture(const RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			return middle ? middle->occupiedFurniture.get() : RE::NiPointer<RE::TESObjectREFR>{};
		}

		// How often a mirror is stood up out of furniture before it is left to the idle stops below: once
		// does it, and one that will not let go would only have its graph reset again and again.
		constexpr std::uint32_t SEAT_EXIT_TRIES = 3;

		// A puppet's graph refused "moveStart": something holds it. Dogmeat's graph starts moving only from its
		// plain standing idle, and a dog left standing sits, lies down or turns on the spot after a few
		// seconds, so the mirror slid after its owner's copy sitting down. The AI ends the idle before walking
		// off; with the AI off, we do: "IdleStop" ends an idle, "turnStop" a turn on the spot, and
		// DriveAnimation's "moveStart" is taken once it stands. In Dogmeat's graph "turnStop" also ends a sit
		// or a lie-down at once (NonCombatStandingBehavior's TurnStop wildcard leaves SitLayState too), cutting
		// short the get-up "IdleStop" started: wanted, as its owner's copy already runs and a get-up would slide
		// after it. A friend's stand-in in its power armor frame gets the same stops: the graph the engine's
		// rebuild gives one switched into its frame answered its variables and played an idle, but took neither
		// "moveStart" nor "moveStop" (0.8.7-rc1 soak), and the stand-in glided after the friend standing; what
		// the AI would do for an NPC in a frame is end what holds it. Out of a frame a stand-in's graph was never
		// seen held, and a replayed throw or reload is an idle of ours the stops would cut short: left alone
		// there. How often is IdleRelease's. Called right after DriveAnimation asked it to move, in the engine's
		// update of the actor.
		void ReleaseIdle(RE::Actor* a_actor)
		{
			static const RE::BSFixedString idleStop{ "IdleStop" };
			static const RE::BSFixedString turnStop{ "turnStop" };
			static const RE::BSFixedString doNotInterrupt{ "bDoNotInterrupt" };

			const bool    inArmor = a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
			bool          due = false;
			bool          last = false;  // this stuck spell's last stop
			std::uint32_t seatExits = 0;
			Kind          kind = Kind::kNpc;
			{
				std::scoped_lock l{ lock };
				const auto entry = Find(a_actor);
				if (!entry || (entry->kind == Kind::kPlayer && !inArmor)) {
					return;
				}
				due = entry->idleRelease.Due(!entry->moving, std::chrono::steady_clock::now());
				last = entry->idleRelease.Spent();
				seatExits = entry->seatExits;
				kind = entry->kind;
			}
			if (!due) {
				return;
			}
			const char* mover = kind == Kind::kPlayer ? "its friend walks" : "its owner's copy moves";
			// Furniture holds a mirror: while an actor uses furniture its race swaps in that furniture's own graph,
			// which takes none of the stops below nor "moveStart" (Dogmeat's for its DogmeatBarJump markers, as
			// on Red Rocket's counter: up on its hind legs, front paws raised), and only the AI walks it out; the
			// do-nothing package we give a mirror leaves it there. A friend's Dogmeat slid after its owner's copy
			// standing upright. It is stood up at once, the way the engine's AI clears furniture: let go and the
			// graph back in its base state, so "moveStart" is taken next frame. Not moved to the furniture's exit,
			// which can be far off (we place it every frame). Not a seat on another actor (a vertibird's) nor a
			// power armor frame: letting go of that would take the armor's state off it. The engine makes the
			// same call from the same place, its own update of the actor, when a furniture animation times out.
			// Never a stand-in: the furniture it is in is its friend's seat (held, it is not driven at all), and
			// StandInSeat stands it up as they get up.
			const auto furniture = OccupiedFurniture(a_actor);
			if (kind == Kind::kNpc && furniture && !furniture->As<RE::Actor>() && !inArmor && seatExits < SEAT_EXIT_TRIES) {
				const auto sit = static_cast<int>(a_actor->DoGetSitSleepState());
				const auto base = furniture->GetObjectReference();
				a_actor->StopInteractingQuick(true, false, true);
				const bool letGo = !OccupiedFurniture(a_actor);
				{
					std::scoped_lock l{ lock };
					if (const auto entry = Find(a_actor)) {
						++entry->seatExits;
						// Its graph is back in its base state, standing and not sneaking, whatever was sent
						// before: DriveAnimation sends the sneak event again next frame if its owner's copy
						// sneaks, and tests its start at once.
						entry->appliedFlags = 0;
						entry->nextResync = {};
					}
				}
				REX::INFO("Puppets: {:08X} was in furniture {:08X} (base {:08X}, sit state {}) while its owner's copy moves; stood it up (let go={})", a_actor->GetFormID(), furniture->GetFormID(), base ? base->GetFormID() : 0, sit, letGo);
				return;
			}
			const auto graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			// What the engine's own idle stop checks: a paired or scripted animation that must play out.
			if (bool keep = false; graph->GetGraphVariableImplBool(doNotInterrupt, keep) && keep) {
				return;
			}
			const bool    stood = graph->NotifyAnimationGraphImpl(idleStop);
			const bool    turned = graph->NotifyAnimationGraphImpl(turnStop);
			const bool    gaveUp = last && !stood && !turned;
			std::uint32_t taken = 0;
			std::uint32_t giveUps = 0;
			{
				std::scoped_lock l{ lock };
				if (const auto entry = Find(a_actor)) {
					++entry->idleStops;
					entry->idleStopsTaken += stood || turned;
					entry->idleGiveUps += gaveUp;
					taken = entry->idleStopsTaken;
					giveUps = entry->idleGiveUps;
				}
			}
			if ((stood || turned) && MoveSync::Loggable(taken)) {
				REX::INFO("Puppets: {:08X} was held by an idle while {}; stopped it (idle={} turn={}, {} so far)", a_actor->GetFormID(), mover, stood, turned, taken);
			} else if (gaveUp && MoveSync::Loggable(giveUps)) {
				REX::INFO("Puppets: {:08X} still refuses to move after {} idle stops while {}; left alone until it moves or that stops ({} so far)", a_actor->GetFormID(), IdleRelease::TRIES, mover, giveUps);
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

		// Player stand-ins whose graph was found moving while they should stand, or the other way round, and
		// put back (see DriveAnimation): for the dev status line.
		std::atomic<std::uint64_t> standInResyncs{ 0 };

		// With AI off nothing feeds the animation graph, so we do: Speed/Direction every frame,
		// moveStart/moveStop when the remote player starts or stops, and jump events when they
		// leave or reach the ground.
		void DriveAnimation(RE::Actor* a_actor, const Puppet& a_puppet)
		{
			static const RE::BSFixedString speedVar{ "Speed" };
			static const RE::BSFixedString directionVar{ "Direction" };
			static const RE::BSFixedString pitchVar{ "Pitch" };
			static const RE::BSFixedString idleLocomotionVar{ "iSyncIdleLocomotion" };
			static const RE::BSFixedString moveStart{ "moveStart" };
			static const RE::BSFixedString moveStop{ "moveStop" };
			static const RE::BSFixedString jumpStart{ "jumpStart" };
			static const RE::BSFixedString jumpFall{ "jumpFall" };
			static const RE::BSFixedString jumpLand{ "jumpLand" };
			static const RE::BSFixedString sneakStart{ "sneakStart" };
			static const RE::BSFixedString sneakStop{ "sneakStop" };

			static const RE::BSFixedString walkMultVar{ "WalkSpeedMult" };
			static const RE::BSFixedString runMultVar{ "RunSpeedMult" };
			static const RE::BSFixedString weaponWalkMultVar{ "fLocomotionWalkPlaybackSpeed" };
			static const RE::BSFixedString weaponRunMultVar{ "fLocomotionRunPlaybackSpeed" };
			static const RE::BSFixedString gaitSyncVar{ "iSyncLocomotionSpeed" };
			static const RE::BSFixedString weaponGaitVar{ "iLocomotionSpeedState" };
			static const RE::BSFixedString rangeVar{ CreatureGait::RANGE_VARIABLE };
			static const RE::BSFixedString markerVar{ CreatureGait::MARKER_VARIABLE };

			const auto  graph = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			const auto& motion = a_puppet.target;
			const auto  now = std::chrono::steady_clock::now();

			// Walk or run (Gait.h): only a humanoid graph has the walk's playback rate. The sync
			// variables go first, so a moveStart below enters the right gait.
			float      walkMult = 0.0f;
			const bool humanoid = graph->GetGraphVariableImplFloat(walkMultVar, walkMult);
			const float seconds = a_puppet.gaitAt == std::chrono::steady_clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - a_puppet.gaitAt).count();
			const float gaitSpeed = Gait::Smooth(a_puppet.gaitSpeed, motion.speed, seconds);
			const auto  gait = humanoid ? Gait::Pick(gaitSpeed, a_puppet.gait, motion.moveMode) : a_puppet.gait;
			if (humanoid) {
				graph->SetGraphVariableInt(gaitSyncVar, Gait::SyncValue(gait));
				graph->SetGraphVariableInt(weaponGaitVar, Gait::WeaponState(gait));
			}
			// A dog's (CreatureGait.h): the gait and rate its owner's graph plays, its state set first for the same reason.
			std::int32_t range = 0;
			float        marker = 0.0f;
			const bool   dog = !humanoid && graph->GetGraphVariableImplInt(rangeVar, range) && graph->GetGraphVariableImplFloat(markerVar, marker);
			const auto   dogGait = CreatureGait::Choose((motion.flags & Protocol::kOwnGait) != 0, motion.gait, motion.gaitRate, motion.speed);
			if (dog) {
				graph->SetGraphVariableInt(rangeVar, dogGait.range->id);
			}

			// Start or stop, and every MoveSync::RESYNC tell the graph again in case it is in the other state.
			// A refused start is tested with a stop; a mirrored NPC's that the stop does not explain (an idle
			// holds it) is ReleaseIdle's, right after.
			const auto step = MoveSync::Drive(a_puppet.moving, a_puppet.nextResync, motion.speed, now, [&](MoveSync::Event a_event) {
				return graph->NotifyAnimationGraphImpl(a_event == MoveSync::Event::kStart ? moveStart : moveStop);
			});
			const bool moving = step.moving;
			if (humanoid) {
				graph->SetGraphVariableFloat(walkMultVar, Gait::Playback(gaitSpeed, Gait::WALK_CLIP));
				graph->SetGraphVariableFloat(runMultVar, Gait::Playback(gaitSpeed, Gait::RUN_CLIP));
				graph->SetGraphVariableFloat(weaponWalkMultVar, Gait::Playback(gaitSpeed, Gait::WEAPON_WALK_CLIP));
				graph->SetGraphVariableFloat(weaponRunMultVar, Gait::Playback(gaitSpeed, Gait::WEAPON_RUN_CLIP));
				if (Gait::Send(moving, a_puppet.moving, gait, a_puppet.gait, step.resent != nullptr)) {
					// The graph in use takes its own one and refuses the other.
					graph->NotifyAnimationGraphImpl(RE::BSFixedString{ Gait::Event(gait) });
					graph->NotifyAnimationGraphImpl(RE::BSFixedString{ Gait::WeaponEvent(gait) });
				}
			}
			if (dog) {
				graph->SetGraphVariableFloat(RE::BSFixedString{ dogGait.range->rateVariable }, dogGait.rate);
				if (CreatureGait::Send(moving, a_puppet.moving, dogGait.range->id, a_puppet.range, step.resent != nullptr)) {
					graph->NotifyAnimationGraphImpl(RE::BSFixedString{ dogGait.range->event });
				}
			}
			// What the engine's own move actions keep and our bare events do not: a jump's take-off and
			// landing, sneaking and standing up, the end of a stagger and the attacks pick their standing or
			// moving version by it. Set before the sneak and jump events below.
			graph->SetGraphVariableInt(idleLocomotionVar, moving ? 1 : 0);

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

			// The animation runs at the speed the owner reported (the speed their own legs play), and only while the
			// graph is in its moving state; it goes the way they go. Pitch moves nothing on an NPC (its blend is
			// weighted by IsPlayer); it is set so a reading shows what arrived. The arms follow the aim point (Aim).
			graph->SetGraphVariableFloat(speedVar, moving ? motion.speed : 0.0f);
			graph->SetGraphVariableFloat(directionVar, motion.direction);
			graph->SetGraphVariableFloat(pitchVar, motion.pitch);

			{
				std::uint32_t resyncs = 0;
				{
					std::scoped_lock l{ lock };
					if (const auto entry = Find(a_actor)) {
						entry->nextResync = step.nextResync;
						entry->resyncsTaken += step.resent != nullptr;
						resyncs = entry->resyncsTaken;
						entry->moving = moving;
						entry->gait = gait;
						entry->gaitSpeed = gaitSpeed;
						entry->range = dog && moving ? dogGait.range->id : -1;
						entry->gaitAt = now;
						entry->appliedFlags = motion.flags;
					}
				}
				if (step.resent) {
					if (a_puppet.kind == Kind::kPlayer) {
						standInResyncs.fetch_add(1, std::memory_order_relaxed);
					}
					if (MoveSync::Loggable(resyncs)) {
						REX::INFO("Puppets: {:08X}'s graph was out of step with its owner's copy (moving={}); sent {} again ({} so far)", a_actor->GetFormID(), moving, step.resent, resyncs);
					}
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
			const auto now = std::chrono::steady_clock::now();
			if (now < puppet->aiUntil) {
				originalUpdate(a_this, a_delta);
				return;
			}

			SuppressBehavior(a_this, puppet->kind);
			if (puppet->kind == Kind::kPlayer) {
				KeepHealthy(a_this);
			}
			// Held for furniture (Hold): in it, the furniture's own graph and marker place and animate it; on its way in, or
			// left beside it, it stands where it is with its legs stopped. Neither its target's spot nor its locomotion events
			// reach it: the spot is on the seat (followed, it stood on the couch), and "moveStart" and the rest would fight the
			// furniture's graph. Its desired movement is none, or the engine feeds the graph the walk it had.
			if (puppet->held) {
				if (const auto process = a_this->currentProcess) {
					if (const auto middle = process->middleHigh) {
						middle->desiredSpeed = 0.0f;
					}
					if (const auto high = process->high) {
						high->pathingDesiredMovementSpeed = {};
						high->pathingCurrentMovementSpeed = {};
					}
				}
				a_this->UpdateNoAI(a_delta);
				if (!OccupiedFurniture(a_this)) {
					auto still = *puppet;
					still.target.speed = 0.0f;
					still.target.flags = static_cast<std::uint8_t>(still.target.flags & ~(Protocol::kInAir | Protocol::kJumping));
					DriveAnimation(a_this, still);
				}
				return;
			}
			const float speed = puppet->moving ? puppet->target.speed : 0.0f;
			if (puppet->hasTarget) {
				// Where its path is now (MotionPath.h), not where it was when our frame's work last ran, which comes at
				// another moment of each frame than this update does.
				if (const auto at = puppet->target.path.At(now)) {
					puppet->target.position = { at->x, at->y, at->z };
					puppet->target.heading = at->heading;
				}
				a_this->SetPosition(puppet->target.position, true);
				a_this->SetHeading(puppet->target.heading);
				static_cast<RE::ActorState&>(*a_this).moveMode = puppet->target.moveMode;
				MatchWeaponDrawn(a_this, *puppet);
				// The engine feeds its locomotion graph from the actor's desired movement, which for an
				// actor with no AI is a stale walk; tell it what the stand-in really does so the run
				// cycle matches the ground covered.
				if (const auto process = a_this->currentProcess) {
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
			// The no-AI update only places the actor's 3D (Actor::UpdateActor3DPosition): it writes neither
			// the desired speed above nor the graph's variables, which DriveAnimation sets below.
			a_this->UpdateNoAI(a_delta);
			if (puppet->hasTarget) {
				DriveAnimation(a_this, *puppet);
				Aim(a_this, *puppet);
				// It has just asked a standing graph to move; a refusal means an idle holds it.
				if (!puppet->moving && puppet->target.speed > MoveSync::START) {
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

	bool IsRegistered(const RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		return Find(a_actor) != nullptr;
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

	void Hold(RE::Actor* a_actor, bool a_hold)
	{
		std::scoped_lock l{ lock };
		if (const auto entry = Find(a_actor)) {
			if (entry->held && !a_hold) {
				// Its graph is in its base state again, or still standing where it was held: what DriveAnimation sent before
				// is sent again (the sneak) and tested (the start) as it follows its target once more.
				entry->appliedFlags = 0;
				entry->nextResync = {};
			}
			entry->held = a_hold;
		}
	}

	void GraphRebuilt(RE::Actor* a_actor)
	{
		std::scoped_lock l{ lock };
		if (const auto entry = Find(a_actor)) {
			// What the old graph was told is nothing to the new one: it stands, not sneaking, in its base state. A stand-in
			// switched into its frame mid-walk kept moving=true, and the fresh graph was told "moveStart" only by the resync
			// half a second on (0.8.7-rc1 soak: moving=true, Speed=312, legs still).
			entry->moving = false;
			entry->appliedFlags = 0;
			entry->nextResync = {};
			entry->idleRelease = {};
		}
	}

	bool Enter(RE::Actor* a_actor, RE::TESObjectREFR* a_furniture, Entry a_way)
	{
		if (!a_actor || !a_furniture || !IsRegistered(a_actor)) {
			return false;
		}
		bool asked = false;
		if (a_way == Entry::kSnap) {
			// Both ends need a script object, and a stand-in or a plain chair is bound to none until one is made for it
			// (as LooksMenu does for a stand-in). The VM runs the call a frame or more later.
			const auto self = Papyrus::BoundObject(a_actor, "Actor", true);
			const auto target = Papyrus::BoundObject(a_furniture, "ObjectReference", true);
			asked = self && target && Papyrus::CallOn(self, "SnapIntoInteraction", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
				a_out[0] = target;
			});
		} else {
			// Only plain furniture (a seat, a bed): a workbench or a terminal activated by anyone but the player is left to the
			// snap, rather than risk the engine opening its menu.
			const auto base = a_furniture->GetObjectReference();
			const auto furniture = base && base->GetFormType() == RE::ENUM_FORM_ID::kFURN ? static_cast<const RE::TESFurniture*>(base) : nullptr;
			if (furniture && furniture->wbData.type == RE::WorkbenchData::Type::kNone) {
				RE::Console::ExecuteCommand(std::format("{:08X}.activate {:08X}", a_furniture->GetFormID(), a_actor->GetFormID()).c_str());
				asked = true;
			}
		}
		if (asked) {
			Hold(a_actor, true);
		}
		return asked;
	}

	bool StandUp(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const auto furniture = OccupiedFurniture(a_actor);
		const bool inArmor = a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
		// Never a power armor frame (letting go would take the armor's state off it) nor a seat on another actor, and
		// not one with no AI process left (its cell went): the engine works on that process to let go.
		const bool stood = furniture && !furniture->As<RE::Actor>() && !inArmor && a_actor->currentProcess;
		if (stood) {
			a_actor->StopInteractingQuick(true, false, true);
		}
		Hold(a_actor, false);
		return stood;
	}

	RE::NiPointer<RE::TESObjectREFR> FurnitureOf(const RE::Actor* a_actor)
	{
		return a_actor ? OccupiedFurniture(a_actor) : RE::NiPointer<RE::TESObjectREFR>{};
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
		const auto furniture = OccupiedFurniture(a_actor);
		auto       out = std::format("puppet: moving={} speed={:.0f} idleStops={} taken={} resyncs={} sit={} furniture={:08X} seatExits={} held={} ownerGait={} graph:", puppet.moving, puppet.target.speed, puppet.idleStops,
			puppet.idleStopsTaken, puppet.resyncsTaken, static_cast<int>(a_actor->DoGetSitSleepState()), furniture ? furniture->GetFormID() : 0, puppet.seatExits, puppet.held,
			(puppet.target.flags & Protocol::kOwnGait) ? std::format("{}@{:.2f}", puppet.target.gait, CreatureGait::UnpackRate(puppet.target.gaitRate)) : std::string{ "none" });
		// What the locomotion graph holds: a dog that refuses to move shows bForceIdleStop (sitting or
		// lying), and one that runs shows its gait in iSyncLocomotionRangeID (3 is the run, CreatureGait.h) and the rate its
		// walk and run play at.
		const auto graph = static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor);
		for (const auto name : { "Speed", "iSyncLocomotionRangeID", "fWalkClipMult", "fRunClipMult", "bAnimationDriven", "bForceIdleStop" }) {
			const RE::BSFixedString var{ name };
			float                   f = 0.0f;
			std::int32_t            n = 0;
			bool                    b = false;
			// The float getter copies any variable's raw bits, so an int or a bool read through it showed 0.
			if (name[0] == 'i' && graph->GetGraphVariableImplInt(var, n)) {
				out += std::format(" {}={}", name, n);
			} else if (name[0] == 'b' && graph->GetGraphVariableImplBool(var, b)) {
				out += std::format(" {}={}", name, b);
			} else if (name[0] == 'f' && graph->GetGraphVariableImplFloat(var, f)) {
				out += std::format(" {}={:.2f}", name, f);  // a clip's rate
			} else if (graph->GetGraphVariableImplFloat(var, f)) {
				out += std::format(" {}={:.0f}", name, f);
			}
		}
		return out + " " + DescribeAim(a_actor);
	}

	std::string DescribeAim(const RE::Actor* a_actor)
	{
		const auto&           at = a_actor->data.location;
		const AimPoint::Point from{ at.x, at.y, at.z };
		auto                  out = std::format("angleX={:.1f} angleZ={:.1f}", a_actor->data.angle.x * AimPoint::TO_DEGREES, a_actor->data.angle.z * AimPoint::TO_DEGREES);
		if (const auto point = GetAimPoint(a_actor)) {
			// shoulderPitch: the pitch from AimPoint::MIRROR_HEIGHT, which NpcSync sends for an NPC this game runs and a
			// mirrored copy is aimed from.
			out += std::format(" aimPoint=({:.0f},{:.0f},{:.0f}) aimPitch={:.1f} aimHeading={:.1f} shoulderPitch={:.1f}", point->x, point->y, point->z,
				AimPoint::PitchTo(from, *point), AimPoint::HeadingTo(from, *point) * AimPoint::TO_DEGREES, AimPoint::OwnerPitch(from, *point).value_or(0.0f));
		} else {
			out += CanAim(a_actor) ? " aimPoint=none" : " aimPoint=- (no mover)";
		}
		{
			std::scoped_lock l{ lock };
			if (const auto entry = Find(a_actor); entry && entry->kind == Kind::kPlayer) {
				out += std::format(" ownerPitch={:.0f}", entry->target.pitch);
			} else if (entry) {
				// A mirrored NPC: the pitch its owner's copy aims at, as received (none: it aims at nothing there).
				out += entry->target.ownerAim ? std::format(" ownerAim={:.0f}", *entry->target.ownerAim) : std::string{ " ownerAim=none" };
			}
		}
		// What the weapon behaviour's aim modifier makes of it: bAimActive while it turns the arms, its offsets
		// from the point, and its limits.
		const auto graph = static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor);
		for (const auto name : { "bAimActive", "bAimEnabled" }) {
			bool b = false;
			if (graph->GetGraphVariableImplBool(RE::BSFixedString{ name }, b)) {
				out += std::format(" {}={}", name, b);
			}
		}
		for (const auto name : { "IsPlayer", "Pitch", "AimPitchCurrent", "AimHeadingCurrent", "fDirectAtSavedGain", "AimPitchMaxUp", "AimPitchMaxDown" }) {
			float f = 0.0f;
			if (graph->GetGraphVariableImplFloat(RE::BSFixedString{ name }, f)) {
				out += std::format(" {}={:.2f}", name, f);
			}
		}
		return out;
	}

	std::string AimAt(RE::Actor* a_actor, std::optional<float> a_pitch)
	{
		if (!CanAim(a_actor)) {
			return "error: the actor has no mover yet";
		}
		if (a_pitch) {
			const auto& at = a_actor->data.location;
			SetAimPoint(a_actor, AimPoint::Along({ at.x, at.y, at.z }, a_actor->data.angle.z, *a_pitch));
		} else {
			ClearAimPoint(a_actor);
		}
		return DescribeAim(a_actor);
	}

	std::optional<RE::NiPoint3> AimPointOf(const RE::Actor* a_actor)
	{
		const auto point = GetAimPoint(a_actor);
		return point ? std::optional<RE::NiPoint3>{ RE::NiPoint3{ point->x, point->y, point->z } } : std::nullopt;
	}

	std::string Describe()
	{
		const auto resyncs = standInResyncs.load(std::memory_order_relaxed);
		if (resyncs == 0) {
			return {};
		}
		return std::format("graphResyncs={}", resyncs);
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();

		std::vector<std::pair<const RE::Actor*, std::uint32_t>> due;
		{
			std::scoped_lock l{ lock };
			for (auto& [actor, puppet] : puppets) {
				// Not while its own AI runs (a climb into or out of power armor): a new package mid-climb can
				// break off the furniture use the AI is in the middle of. It gets one as soon as that is over.
				// Nor while it is held for furniture (Hold): a new package can make the engine take it out of its seat.
				if (now >= puppet.nextIdleRefresh && now >= puppet.aiUntil && !puppet.held) {
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
