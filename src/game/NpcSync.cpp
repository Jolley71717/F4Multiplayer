#include "game/NpcSync.h"

#include "game/AimPoint.h"
#include "game/CommandMeter.h"
#include "game/CreatureGait.h"
#include "game/Dialogue.h"
#include "game/Equipment.h"
#include "game/FrameShare.h"
#include "game/Gait.h"
#include "game/Hud.h"
#include "game/MirrorGait.h"
#include "game/NpcClaims.h"
#include "game/NpcHold.h"
#include "game/NpcPlace.h"
#include "game/OpenMenus.h"
#include "game/Prediction.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/RetryBudget.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"
#include "game/StandInRange.h"
#include "game/Story.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"

namespace NpcSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto SCAN_INTERVAL = 250ms;
		// NPCs fighting or near a player send 20 states a second, the rest 10 (every other tick).
		constexpr auto  STATE_INTERVAL = 50ms;
		constexpr float FAST_RANGE = 3000.0f;  // game units from any player
		constexpr auto TAKEOVER_COOLDOWN = 5s;  // between takeover requests for the same NPC
		// While we talk to an NPC we run, we claim it again this often so nobody takes it over
		// mid-conversation (the server refuses takeovers within 5 s of a claim).
		constexpr auto TALK_CLAIM_INTERVAL = 2s;
		// Render far enough in the past to have two states to blend: less for NPCs sending 20 a
		// second than for those sending 10. Changes are eased in so the NPC doesn't jump.
		constexpr auto  FAST_DELAY = 100ms;
		constexpr auto  SLOW_DELAY = 150ms;
		constexpr auto  FAST_GAP = 70ms;  // average time between states for FAST_DELAY
		constexpr float DELAY_SLEW_MS = 2.0f;  // per frame
		// A slow game reads the network once a frame: the delay also covers one of our frames.
		constexpr float DELAY_SLACK_MS = 20.0f;
		constexpr float MAX_DELAY_MS = 250.0f;
		float           frameMs = 1000.0f / 60.0f;  // ours, smoothed
		Clock::time_point lastFrame{};
		constexpr auto STALE_AFTER = 1s;
		// No states for this long, or the owner's copy this far from ours: ours runs its own AI (NpcPlace::Follows).
		constexpr auto  LOST_AFTER = NpcPlace::LOST_AFTER;
		constexpr float MAX_FOLLOW_DISTANCE = NpcPlace::MAX_FOLLOW_DISTANCE;
		// An NPC we run that is this far from us, and much nearer a friend, is given to their game:
		// our game's AI only reacts to our player, so it would ignore the friend standing next to it.
		constexpr float HANDOFF_FROM = 6000.0f;
		constexpr float HANDOFF_TO = 4000.0f;  // the friend must be this close (and at most half as far)
		constexpr auto  HANDOFF_HOLD = 10s;    // we don't claim it back meanwhile
		constexpr auto  HANDOFF_RETRY = 30s;   // between hand-offs of the same NPC
		constexpr std::size_t MAX_SNAPSHOTS = 16;

		constexpr std::uint32_t CURRENT_COMPANION_FACTION = 0x00023C01;

		struct Snapshot
		{
			Clock::time_point  received;
			Protocol::ActorState state;
		};

		// An NPC run by another player that we copy.
		struct Mirror
		{
			std::deque<Snapshot> snapshots;
			Prediction::ArrivalClock arrivals{ .gap = 0.05f };  // even stamps for states read in bursts
			RE::Actor*           registered = nullptr;  // as registered with Puppets
			std::uint32_t        registeredId = 0;      // its form ID then (a copy of a friend's spawn has its own, not the spawn's)
			float                delayMs = std::chrono::duration<float, std::milli>(SLOW_DELAY).count();
		};

		// An NPC we run.
		struct Owned
		{
			RE::NiPoint3       lastPosition;
			Clock::time_point  lastTime{};
			Puppets::GaitGraph graph;  // which locomotion graph it has (Puppets::GaitGraphOf), asked again while it had none
		};

		std::uint32_t localId = 0;

		std::unordered_map<std::uint32_t, std::uint32_t>     owners;     // NPC -> player (absent = nobody)
		NpcClaims::Book                                      claimBook;  // claims awaiting an answer, and companion claims refused
		NpcHold::Book                                        hold;       // NPCs our player fights, and ours that are dying
		std::unordered_map<std::uint32_t, Clock::time_point> lastTakeover;
		std::uint32_t                                        talkingTo = 0;  // the NPC in our conversation
		std::vector<std::uint32_t>                           holding;        // loaded NPCs holding our player (Dialogue::Holds), as of the last scan
		Clock::time_point                                    nextTalkClaim{};
		std::uint32_t                                        conversations = 0;
		std::uint32_t                                        localFallbacks = 0;  // mirrors handed to local AI (owner far, quiet or elsewhere)
		std::uint32_t                                        broughtOver = 0;     // mirrors teleported after their owner's copy outdoors
		std::uint32_t                                        broughtIn = 0;       // copies moved to where their owner's stands: from an unloaded cell, or from another space ours was left in
		std::uint32_t                                        switchedOn = 0;      // copies that were off here while their owner's ran (Enable)
		// Left alone, each counted once: characters of our own story off here (not switched on: MaySwitchOn), and
		// copies not moved to their owner's because the owner's stand-in is not beside it (OwnerNear), by
		// BringIn and by the bring-over of a far mirror.
		std::uint32_t                                        keptOffStory = 0, notBroughtOwnerFar = 0, notBroughtOverOwnerFar = 0;
		std::unordered_set<std::uint32_t>                    keptOff, notBrought, notBroughtOver;
		RetryBudget::Budget                                  switchOns{ 5s };  // SwitchOn, per NPC: three tries at most
		RetryBudget::Budget                                  bringIns{ 10s };  // BringIn, the same
		CommandMeter::Meter                                  meter;  // console commands a frame, and copies taken over a frame
		std::unordered_map<std::uint32_t, Clock::time_point> handedOffAt;
		std::uint32_t                                        handOffs = 0;
		std::uint32_t                                        stateTick = 0;
		std::uint32_t                                        fastNpcs = 0;  // owned NPCs sent at 20 Hz on the last tick
		// Gear: each game rolls NPC gear on its own, so owners report what their NPCs wear and the
		// others dress their copies the same.
		constexpr auto                                                GEAR_INTERVAL = 1s;
		Clock::time_point                                             nextGear{};
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> sentGear;   // owned NPCs: what was last reported
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> knownGear;  // mirrored NPCs: what their owner reported
		std::uint32_t                                                 gearSent = 0;
		std::uint32_t                                                 gearApplied = 0;
		std::uint32_t                                        lastActivated = 0;
		Clock::time_point                                    lastActivatedAt{};
		constexpr auto                                       ACTIVATION_TO_TALK = 5s;
		constexpr float                                      TALK_RANGE = 400.0f;  // game units

		RE::Actor* LoadedActor(std::uint32_t a_id);
		// Power armor frames (defined with the rest of their kind, after OursOf).
		RE::TESObjectREFR*                   FrameRef(std::uint32_t a_id);
		void                                 ScanFrames(Clock::time_point a_now, std::unordered_set<std::uint32_t>& a_loaded, std::vector<Protocol::ActorClaim>& a_claims);
		std::optional<Protocol::ActorState> FrameState(std::uint32_t a_id, Clock::time_point a_now);
		void                                 ApplyFrameState(RE::TESObjectREFR* a_frame, const Protocol::ActorState& a_state, std::uint32_t a_owner, Clock::time_point a_now);

		// Who the player is talking to: the NPC they activated, else the nearest NPC (one who
		// started the conversation). (MenuTopicManager::speaker isn't reliable in this version.)
		std::uint32_t FindPartner(Clock::time_point a_now)
		{
			if (lastActivated && a_now - lastActivatedAt < ACTIVATION_TO_TALK && LoadedActor(lastActivated)) {
				return lastActivated;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::uint32_t nearest = 0;
			float         best = TALK_RANGE;
			for (const auto& [id, entry] : owners) {
				const auto actor = LoadedActor(id);
				if (actor && !actor->IsDead(false)) {
					const float distance = actor->data.location.GetDistance(player->data.location);
					if (distance < best) {
						best = distance;
						nearest = id;
					}
				}
			}
			return nearest;
		}
		std::unordered_map<std::uint32_t, Mirror>            mirrors;
		std::unordered_map<std::uint32_t, Owned>             owned;

		std::vector<Outgoing> outgoing;
		Clock::time_point     nextScan{};
		Clock::time_point     nextStates{};

		// Power armor frames standing in the world, shared as NPCs are (FrameShare.h): furniture references, found by a
		// walk of the loaded cells now and then rather than among the actors, claimed and released through the same
		// book, their states sent among the NPCs' (once a second, and at once when one moved), and never mirrored:
		// ours is moved to where its owner's game has it, and left there.
		constexpr auto                               FRAME_WALK_INTERVAL = 2s;
		Clock::time_point                            nextFrameWalk{};
		bool                                         hosting = false;  // our server is the session's (Frame)
		std::unordered_set<std::uint32_t>            standingFrames;   // frames found standing, loaded, by the last walk
		std::unordered_set<std::uint32_t>            usedFrames;       // frames our player got into or out of, or had left last: ours to run
		// Frames our game made (0xFF......), shared as spawns (FrameShare::SharesMade, Spawns::ShareFrame), by their ids here.
		constexpr std::size_t                        MAX_MADE_SEEN = 64;  // standing ones looked at, and our player's own kept, at most
		std::unordered_set<std::uint32_t>            standingMade;        // found standing, loaded, by the last walk
		std::unordered_set<std::uint32_t>            usedMade;            // our player got into or out of, or had left last
		std::uint32_t                                wornFrame = 0;    // the frame our player was in at the last scan (0: none)
		struct SentFrame
		{
			RemoteEdits::Spot at;
			float             heading = 0.0f;
			Clock::time_point sentAt{};
		};
		std::unordered_map<std::uint32_t, SentFrame> sentFrames;  // owned frames: where each was last reported
		RetryBudget::Budget                          frameBrings{ 10s };  // a frame moved through the console: three tries
		std::unordered_map<std::uint32_t, int>       frameChoices;       // the last FrameShare::Move logged for each
		std::uint32_t                                framesNudged = 0;   // moved in place to where their owner's game has them
		std::uint32_t                                framesBrought = 0;  // moved there through the console (from an unloaded cell, or far)
		std::uint32_t                                frameStates = 0;    // states sent for frames we run


		// Our game's NPC under an id NpcSync and the wire use: a plugin's reference is the same in every game; under a
		// spawn id (Spawns) is our own NPC shared so, or our copy of a friend's. 0: none.
		std::uint32_t LocalId(std::uint32_t a_id)
		{
			if (Protocol::IsSpawnRef(a_id)) {
				return Spawns::LocalOf(a_id);
			}
			return Protocol::IsShareableRef(a_id) ? a_id : 0;  // the player or a stand-in: never something to mirror
		}

		RE::Actor* LoadedActor(std::uint32_t a_id)
		{
			const auto local = LocalId(a_id);
			if (local == 0) {
				return nullptr;
			}
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(local);
			return actor && !actor->IsDeleted() && !actor->IsDisabled() && actor->Get3D() && !actor->IsPlayerRef() && !Puppets::IsPuppet(actor) ? actor : nullptr;
		}

		// NPCs we can share: loaded, alive, from a plugin file, and not a player or a stand-in.
		bool IsCandidate(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsPlayerRef() && Protocol::IsShareableRef(a_actor->GetFormID()) && !Puppets::IsPuppet(a_actor) &&
			       !a_actor->IsDead(false) && !a_actor->IsDisabled() && a_actor->Get3D();
		}

		// Where a reference is, as sent in states: (interior cell, 0) or (0, worldspace).
		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::TESObjectREFR* a_actor)
		{
			const auto cell = a_actor->GetParentCell();
			if (!cell) {
				return { 0, 0 };
			}
			if (cell->IsInterior()) {
				return { cell->GetFormID(), 0 };
			}
			return { 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0 };
		}

		void StopMirroring(std::uint32_t a_id, Mirror& a_mirror)
		{
			WeaponFire::EndShield(a_id);  // a ghost made against a friend's explosion, if it is one now
			if (!a_mirror.registered) {
				return;
			}
			WeaponFire::ReleaseSoundsOf(a_mirror.registeredId);  // a replayed burst may still follow the actor's 3D
			WeaponFire::DropHandThrowable(a_mirror.registeredId);
			if (RE::TESForm::GetFormByID<RE::Actor>(a_mirror.registeredId) == a_mirror.registered) {
				Puppets::ReleaseNpc(a_mirror.registered);
				meter.Released();  // and its evp
			} else {
				Puppets::Unregister(a_mirror.registered, a_mirror.registeredId);  // the actor is gone; just forget it
			}
			a_mirror.registered = nullptr;
			a_mirror.registeredId = 0;
		}

		void StopMirroring(std::uint32_t a_id)
		{
			if (const auto it = mirrors.find(a_id); it != mirrors.end()) {
				StopMirroring(a_id, it->second);
				mirrors.erase(it);
			}
		}

		std::uint32_t OwnerOf(std::uint32_t a_id)
		{
			const auto it = owners.find(a_id);
			return it != owners.end() ? it->second : 0;
		}

		template <class T>
		void SendList(const std::vector<T>& a_items, auto a_encode, bool a_reliable)
		{
			for (std::size_t i = 0; i < a_items.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
				const auto end = (std::min)(a_items.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
				outgoing.push_back({ a_encode(std::vector<T>(a_items.begin() + i, a_items.begin() + end)), a_reliable });
			}
		}

		// See HANDOFF_FROM. Not our companion, the NPC we're talking to or one holding us (NpcPlace::StaysWithUs), one
		// fighting us, nor one our player hit lately (NpcHold): a long shot's target stays ours, so our killing blow lands
		// where it runs.
		bool ShouldHandOff(RE::Actor* a_actor, std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (const auto last = handedOffAt.find(a_id); last != handedOffAt.end() && a_now - last->second < HANDOFF_RETRY) {
				return false;
			}
			if (a_id == talkingTo || Dialogue::Holds(a_actor) || IsOurCompanion(a_actor) || a_actor->currentCombatTarget.get().get() == player || hold.FoughtLately(a_id, a_now)) {
				return false;
			}
			const auto& at = a_actor->data.location;
			const float ours = at.GetDistance(player->data.location);
			if (ours < HANDOFF_FROM) {
				return false;
			}
			const auto space = SpaceOf(a_actor);
			for (const auto& info : RemotePlayers::List()) {
				if (!info.state || std::pair{ info.state->cell, info.state->worldspace } != space) {
					continue;
				}
				const float theirs = at.GetDistance(RE::NiPoint3{ info.state->x, info.state->y, info.state->z });
				if (theirs < HANDOFF_TO && theirs * 2.0f < ours) {
					return true;
				}
			}
			return false;
		}

		void Scan(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> handOff;
			std::unordered_set<std::uint32_t> loaded;
			std::vector<Protocol::ActorClaim> claims;
			holding.clear();

			const auto lists = RE::ProcessLists::GetSingleton();
			if (lists) {
				for (const auto& handle : lists->highActorHandles) {
					const auto ptr = handle.get();
					const auto actor = ptr.get();
					// One our game made at runtime is shared under a spawn id (Spawns) and run by our game for good: never
					// claimed, nor handed off. Loaded while dead too, so it isn't let go before the server has its death.
					if (actor && Protocol::IsSpawnRef(actor->GetFormID())) {
						if (const auto spawn = Spawns::Share(actor)) {
							WeaponFire::InstallForNpcs(actor);
							loaded.insert(spawn);
						}
						continue;
					}
					if (!IsCandidate(actor)) {
						continue;
					}
					WeaponFire::InstallForNpcs(actor);  // no-op once hooked
					const auto id = actor->GetFormID();
					loaded.insert(id);
					if (Dialogue::Holds(actor)) {
						holding.push_back(id);
					}

					const auto owner = OwnerOf(id);
					if (owner == localId) {
						if (ShouldHandOff(actor, id, a_now)) {
							handOff.push_back(id);
						}
						continue;
					}
					// Just handed to a nearer friend: give their game time to claim it.
					if (const auto handed = handedOffAt.find(id); owner == 0 && handed != handedOffAt.end() && a_now - handed->second < HANDOFF_HOLD) {
						continue;
					}
					// Companions follow their own player, so that player's game must run them. Nothing the session has dead,
					// and nothing at all until its dead list is in after a load: the server would take a copy alive only in
					// an older save for one that came back (WorldSync::MayClaim). Not again while a claim awaits its answer, nor for a
					// while after another player who has it as a companion too kept it (NpcClaims::Book::Offer). One our game
					// keeps essential is offered marked so, even with the session having it dead: its death waits here, and from
					// the host's game the claim undoes one an older build saved by mistake (Dogmeat, taken for dead as he went down).
					const bool essential = WorldSync::IsEssential(actor);
					if (const auto reason = claimBook.Offer(id, owner, localId, IsOurCompanion(actor), WorldSync::MayClaim(id, essential), a_now)) {
						claims.push_back({ id, *reason, essential });
					}
				}
			}
			ScanFrames(a_now, loaded, claims);
			claimBook.Expire(a_now);
			hold.Expire(a_now);
			std::erase_if(lastTakeover, [&](const auto& a_entry) { return a_now - a_entry.second >= TAKEOVER_COOLDOWN; });
			std::erase_if(handedOffAt, [&](const auto& a_entry) { return a_now - a_entry.second >= HANDOFF_RETRY; });

			// NPCs we run that are no longer loaded go back to the pool, and so do the ones a friend is
			// much nearer to (their game claims them). One that died here is kept until our report of its
			// death ends it (NpcHold): released as it fell, a friend's game where it still lived took it
			// over, and our kill was not believed. What we said they wear is forgotten with them, as the
			// server forgets it: when we run one again we report it afresh.
			std::vector<std::uint32_t> released;
			for (const auto id : handOff) {
				owners.erase(id);
				owned.erase(id);
				sentGear.erase(id);
				hold.Forget(id);
				handedOffAt[id] = a_now;
				released.push_back(id);
				++handOffs;
				REX::INFO("NpcSync: handing {:08X} to a nearer player", id);
			}
			for (auto it = owned.begin(); it != owned.end();) {
				if (!loaded.contains(it->first)) {
					const auto actor = LoadedActor(it->first);
					if (hold.Keep(it->first, actor && actor->IsDead(false), a_now)) {
						++it;
						continue;
					}
					hold.Forget(it->first);
					released.push_back(it->first);
					owners.erase(it->first);
					sentGear.erase(it->first);
					it = owned.erase(it);
				} else {
					++it;
				}
			}

			SendList(claims, [](const std::vector<Protocol::ActorClaim>& a) { return Protocol::Encode(a); }, true);
			SendList(released, [](const std::vector<std::uint32_t>& a) { return Protocol::EncodeRelease(a); }, true);
		}

		// A menu stops our game (pause menu, Pip-Boy): our NPCs stand still, so their states stop
		// too, and the others' copies run their own AI rather than freezing with ours.
		bool GamePaused()
		{
			const auto ui = RE::UI::GetSingleton();
			return ui && ui->menuMode > 0;
		}

		// Fighting, or near enough to a player to be watched closely.
		bool IsFast(RE::Actor* a_actor, const RE::NiPoint3& a_pos, RE::PlayerCharacter* a_player, const std::vector<RemotePlayers::Info>& a_friends)
		{
			if (a_actor->IsInCombat()) {
				return true;
			}
			if (a_player && a_pos.GetDistance(a_player->data.location) < FAST_RANGE) {
				return true;
			}
			const auto space = SpaceOf(a_actor);
			return std::ranges::any_of(a_friends, [&](const RemotePlayers::Info& a_info) {
				return a_info.state && std::pair{ a_info.state->cell, a_info.state->worldspace } == space &&
				       a_pos.GetDistance(RE::NiPoint3{ a_info.state->x, a_info.state->y, a_info.state->z }) < FAST_RANGE;
			});
		}

		// Gives back every NPC we run and lets go of the ones we copy.
		void LeaveAll()
		{
			std::vector<std::uint32_t> released;
			for (const auto& [id, track] : owned) {
				owners.erase(id);
				released.push_back(id);
			}
			owned.clear();
			claimBook.Clear();
			hold.Clear();
			switchOns.Clear();
			bringIns.Clear();
			for (auto& [id, mirror] : mirrors) {
				StopMirroring(id, mirror);
			}
			mirrors.clear();
			talkingTo = 0;
			holding.clear();
			sentFrames.clear();
			frameBrings.Clear();
			SendList(released, [](const std::vector<std::uint32_t>& a) { return Protocol::EncodeRelease(a); }, true);
		}

		// Whether the NPC's graph stands (MirrorGait::Reported).
		bool GraphStands(const RE::Actor* a_actor)
		{
			static const RE::BSFixedString idleVar{ MirrorGait::IDLE_VARIABLE };
			std::int32_t                   locomotion = 0;
			return static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor)->GetGraphVariableImplInt(idleVar, locomotion) && locomotion == 0;
		}

		// The gait the NPC's own graph plays and the rate its clip plays at, for its copies (MirrorGait.h; a dog's,
		// CreatureGait.h). Left out when its graph does not say.
		void ReadGait(const RE::Actor* a_actor, const Puppets::GaitGraph& a_kind, Protocol::ActorState& a_state)
		{
			const auto   graph = static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor);
			std::int32_t state = 0;
			float        rate = 0.0f;
			switch (a_kind.family) {
			case MirrorGait::Family::kPerson:
				{
					static const RE::BSFixedString gaitVar{ MirrorGait::GAIT_VARIABLE };
					static const RE::BSFixedString weaponGaitVar{ MirrorGait::WEAPON_GAIT_VARIABLE };
					const bool drawn = a_actor->GetWeaponMagicDrawn();
					const auto gait = graph->GetGraphVariableImplInt(drawn ? weaponGaitVar : gaitVar, state) ? MirrorGait::PersonGait(drawn, state) : std::nullopt;
					if (!gait) {
						return;
					}
					a_state.gait = static_cast<std::uint8_t>(*gait);
					if (const auto rateVar = MirrorGait::PersonRate(drawn, *gait); rateVar && graph->GetGraphVariableImplFloat(Puppets::VariableName(rateVar), rate)) {
						a_state.gaitRate = CreatureGait::PackRate(rate);
					}
					break;
				}
			case MirrorGait::Family::kDog:
				{
					static const RE::BSFixedString rangeVar{ CreatureGait::RANGE_VARIABLE };
					const auto                     gait = graph->GetGraphVariableImplInt(rangeVar, state) ? CreatureGait::Find(state) : nullptr;
					if (!gait || !graph->GetGraphVariableImplFloat(Puppets::VariableName(gait->rateVariable), rate)) {
						return;
					}
					a_state.gait = gait->id;
					a_state.gaitRate = CreatureGait::PackRate(rate);
					break;
				}
			case MirrorGait::Family::kCreature:
				{
					const auto own = graph->GetGraphVariableImplInt(Puppets::VariableName(a_kind.stateVariable), state) ? MirrorGait::CreatureState(state) : std::nullopt;
					if (!own) {
						return;
					}
					a_state.gait = *own;
					break;
				}
			default:
				return;
			}
			a_state.flags |= Protocol::kOwnGait;
		}

		void SendOwnedStates(Clock::time_point a_now)
		{
			if (GamePaused()) {
				return;
			}
			const bool slowTick = ++stateTick % 2 == 0;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto friends = RemotePlayers::List();
			std::uint32_t fast = 0;
			std::vector<Protocol::ActorState> states;
			for (auto& [id, track] : owned) {
				const auto actor = LoadedActor(id);
				if (!actor) {
					// A power armor frame we run: its state goes among the NPCs' when it is due (FrameShare::Due).
					if (auto state = FrameState(id, a_now)) {
						states.push_back(*state);
					}
					continue;
				}
				if (actor->IsDead(false)) {
					continue;
				}
				const auto& pos = actor->data.location;
				if (!IsFast(actor, pos, player, friends)) {
					if (!slowTick) {
						continue;
					}
				} else {
					++fast;
				}
				float       speed = 0.0f;
				if (track.lastTime != Clock::time_point{}) {
					const float dt = std::chrono::duration<float>(a_now - track.lastTime).count();
					if (dt > 0.0f) {
						const float dx = pos.x - track.lastPosition.x;
						const float dy = pos.y - track.lastPosition.y;
						speed = std::sqrt(dx * dx + dy * dy) / dt;
					}
				}
				track.lastPosition = pos;
				track.lastTime = a_now;

				Protocol::ActorState state;
				state.refId = id;
				std::tie(state.cell, state.worldspace) = SpaceOf(actor);
				state.x = pos.x;
				state.y = pos.y;
				state.z = pos.z;
				state.heading = actor->data.angle.z;
				const auto process = actor->currentProcess;
				const auto middle = process ? process->middleHigh : nullptr;
				// Which graph it has, told once and kept with the track; asked again while it had none (a graph loads late).
				if (track.graph.family == MirrorGait::Family::kNone) {
					track.graph = Puppets::GaitGraphOf(actor);
				}
				const auto& kind = track.graph;
				state.speed = MirrorGait::Reported(middle ? middle->animationSpeed : 0.0f, speed, kind.family, middle && !(middle->animationSpeed > 0.0f) && GraphStands(actor));
				state.moveMode = static_cast<std::uint16_t>(static_cast<const RE::ActorState&>(*actor).moveMode);
				// The way its own graph has it go, rather than guessed from its path by the copies.
				{
					static const RE::BSFixedString directionVar{ "Direction" };
					float                          direction = 0.0f;
					if (static_cast<const RE::IAnimationGraphManagerHolder*>(actor)->GetGraphVariableImplFloat(directionVar, direction)) {
						state.direction = Protocol::PackDirection(direction);
						state.flags |= Protocol::kOwnDirection;
					}
				}
				ReadGait(actor, kind, state);
				// The pitch it aims its gun at, from the point its combat AI gave its weapon aim; the copies have no AI
				// to give them one (AimPoint::ForMirror).
				if (const auto point = Puppets::AimPointOf(actor)) {
					state.aimPitch = Protocol::PackAimPitch(AimPoint::OwnerPitch({ pos.x, pos.y, pos.z }, AimPoint::Point{ point->x, point->y, point->z }));
				}
				if (actor->GetWeaponMagicDrawn()) {
					state.flags |= Protocol::kWeaponDrawn;
				}
				if (actor->IsSneaking()) {
					state.flags |= Protocol::kSneaking;
				}
				states.push_back(state);
			}
			fastNpcs = fast;
			SendList(states, [](const std::vector<Protocol::ActorState>& a) { return Protocol::Encode(a, Protocol::MessageType::kActorStates); }, false);
		}

		// Reports the gear of the NPCs we run whenever it changes (and once when we start running one).
		void SendOwnedGear()
		{
			if (GamePaused()) {
				return;
			}
			for (const auto& [id, track] : owned) {
				const auto actor = LoadedActor(id);
				// A dead one is reported once (it may have died before its first report), then left alone.
				if (!actor || (actor->IsDead(false) && sentGear.contains(id))) {
					continue;
				}
				auto items = Equipment::Read(actor);
				if (const auto last = sentGear.find(id); last != sentGear.end() && last->second == items) {
					continue;
				}
				outgoing.push_back({ Protocol::Encode(Protocol::NpcEquipment{ 0, id, items }, Protocol::MessageType::kReportNpcEquipment), true });
				sentGear[id] = std::move(items);
				++gearSent;
			}
		}

		float WrapAngle(float a_angle)
		{
			constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
			a_angle = std::fmod(a_angle, twoPi);
			return a_angle < 0.0f ? a_angle + twoPi : a_angle;
		}

		float LerpAngle(float a_from, float a_to, float a_t)
		{
			float delta = WrapAngle(a_to - a_from);
			if (delta > std::numbers::pi_v<float>) {
				delta -= std::numbers::pi_v<float> * 2.0f;
			}
			return a_from + delta * a_t;
		}

		// Ease the mirror's delay toward what its owner's send rate needs.
		void AdjustDelay(Mirror& a_mirror)
		{
			const auto& snaps = a_mirror.snapshots;
			constexpr std::size_t GAPS = 4;
			if (snaps.size() <= GAPS) {
				return;
			}
			const auto gap = (snaps.back().received - snaps[snaps.size() - 1 - GAPS].received) / GAPS;
			const float usual = std::chrono::duration<float, std::milli>(gap <= FAST_GAP ? FAST_DELAY : SLOW_DELAY).count();
			const float target = std::clamp(std::chrono::duration<float, std::milli>(gap).count() + frameMs + DELAY_SLACK_MS, usual, MAX_DELAY_MS);
			a_mirror.delayMs += std::clamp(target - a_mirror.delayMs, -DELAY_SLEW_MS, DELAY_SLEW_MS);
		}

		std::optional<Puppets::Motion> Sample(const Mirror& a_mirror, Clock::time_point a_now)
		{
			if (a_mirror.snapshots.empty()) {
				return std::nullopt;
			}
			const auto behind = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float, std::milli>(a_mirror.delayMs));
			const auto renderTime = a_now - behind;
			const auto& snaps = a_mirror.snapshots;

			Protocol::ActorState state = snaps.back().state;
			float                motionAngle = state.heading;
			std::size_t          next = snaps.size();  // the first state the path goes on through (MotionPath.h)
			// The pitch its owner's copy aims at: blended between the two states it is drawn between, as its place and
			// heading are, so its gun sweeps up a target rather than stepping (AimPoint::BlendAim).
			auto ownerAim = Protocol::UnpackAimPitch(state.aimPitch);
			for (std::size_t i = 1; i < snaps.size(); ++i) {
				const auto& a = snaps[i - 1];
				const auto& b = snaps[i];
				if (b.received < renderTime) {
					continue;
				}
				const float span = std::chrono::duration<float>(b.received - a.received).count();
				const float t = span > 0.0f ? std::clamp(std::chrono::duration<float>(renderTime - a.received).count() / span, 0.0f, 1.0f) : 1.0f;
				state = b.state;
				state.x = std::lerp(a.state.x, b.state.x, t);
				state.y = std::lerp(a.state.y, b.state.y, t);
				state.z = std::lerp(a.state.z, b.state.z, t);
				state.heading = LerpAngle(a.state.heading, b.state.heading, t);
				ownerAim = AimPoint::BlendAim(Protocol::UnpackAimPitch(a.state.aimPitch), Protocol::UnpackAimPitch(b.state.aimPitch), t);
				const float dx = b.state.x - a.state.x;
				const float dy = b.state.y - a.state.y;
				if (dx * dx + dy * dy > 1.0f) {
					motionAngle = std::atan2(dx, dy);
				}
				next = i - 1;
				break;
			}
			if (a_now - snaps.back().received > STALE_AFTER) {
				state.speed = 0.0f;
			}
			return Puppets::Motion{
				.position = { state.x, state.y, state.z },
				.heading = state.heading,
				.speed = state.speed,
				.direction = (state.flags & Protocol::kOwnDirection) ? Protocol::UnpackDirection(state.direction) : WrapAngle(motionAngle - state.heading) / (std::numbers::pi_v<float> * 2.0f),
				.moveMode = state.moveMode,
				.flags = state.flags,
				.gait = state.gait,
				.gaitRate = state.gaitRate,
				.ownerAim = ownerAim,
				.path = MotionPath::Ahead({ a_now, state.x, state.y, state.z, state.heading }, state.cell, state.worldspace, snaps, next, behind, &Snapshot::received),
			};
		}

		// Whether the owner's stand-in stands beside where their game has the NPC (RemoteEdits::OwnerNear): what
		// follows its player, as a companion does. Their game saying it is somewhere far from them moves nothing here.
		bool OwnerBeside(std::uint32_t a_owner, const Protocol::ActorState& a_state)
		{
			return RemoteEdits::OwnerBeside(RemotePlayers::LastStateOf, a_owner, a_state);
		}

		// Where a reference is: its space (SpaceOf) and position.
		NpcPlace::Spot SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto [cell, worldspace] = SpaceOf(a_ref);
			return { cell, worldspace, a_ref->data.location.x, a_ref->data.location.y, a_ref->data.location.z };
		}

		// Our copy of the NPC as NpcPlace::Decide takes it.
		NpcPlace::Ours OursOf(RE::Actor* a_actor, std::uint32_t a_id)
		{
			if (!a_actor) {
				return {};
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			return {
				.exists = true,
				.off = a_actor->IsDisabled(),
				.unusable = a_actor->IsDeleted() || a_actor->IsDead(false) || a_actor->IsPlayerRef(),
				.loaded = a_actor->Get3D() != nullptr,
				.at = SpotOf(a_actor),
				.companion = IsOurCompanion(a_actor),
				.inScene = a_actor->GetCurrentScene() != nullptr,
				.fightingUs = player && a_actor->currentCombatTarget.get().get() == player,
				.talkingToUs = a_id == talkingTo,
				.holdsUs = Dialogue::Holds(a_actor),
			};
		}

		// ---- Power armor frames (FrameShare.h) ------------------------------------------------------------------

		// Our reference of a shared frame: a plugin's power armor frame by that ID, whatever its state (disabled while
		// our player wears it, or in a cell no longer loaded). Never one made here, nor an NPC.
		RE::TESObjectREFR* FrameRef(std::uint32_t a_id)
		{
			if (!Protocol::IsShareableRef(a_id)) {
				return nullptr;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			return ref && !ref->IsDeleted() && Equipment::IsPowerArmorFrame(ref->GetObjectReference()) ? ref : nullptr;
		}

		// A power armor frame our game made at runtime, by its id here (Spawns::LocalOf turns a spawn id of ours into it),
		// whatever its state.
		RE::TESObjectREFR* MadeFrameRef(std::uint32_t a_local)
		{
			if (!Protocol::IsSpawnRef(a_local)) {
				return nullptr;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_local);
			return ref && !ref->IsDeleted() && Equipment::IsPowerArmorFrame(ref->GetObjectReference()) ? ref : nullptr;
		}

		// An actor of ours is in this frame (a stand-in gets into a copy of its own, never a reference of our world).
		bool FrameInUse(const RE::TESObjectREFR* a_frame)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				const auto middle = actor && actor->currentProcess ? actor->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				if (occupied && occupied.get() == a_frame) {
					return true;
				}
			}
			return false;
		}

		FrameShare::Ours FrameOursOf(RE::TESObjectREFR* a_frame)
		{
			FrameShare::Ours ours;
			if (!a_frame) {
				return ours;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			ours.found = true;
			ours.created = a_frame->IsCreated();
			ours.frame = true;  // FrameRef looked
			ours.off = a_frame->IsDisabled();
			ours.loaded = a_frame->Get3D() != nullptr;
			ours.quest = a_frame->extraList && a_frame->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
			ours.inUse = FrameInUse(a_frame);
			ours.wornHere = player && Equipment::PowerArmorFrameOf(player).get() == a_frame;
			ours.copy = SaveGuard::Placed(a_frame) || Spawns::CopyIdOf(a_frame->GetFormID()) != 0;
			ours.at = SpotOf(a_frame);
			ours.heading = a_frame->data.angle.z;
			return ours;
		}

		// The frames standing in the loaded cells, found by a walk of their references now and then: furniture is in no
		// process list. Loaded and enabled only; the one our player wears, and ours in cells since unloaded, are known
		// by other means (ScanFrames). Pre-placed ones by their own ids, and the ones our game made (shared as spawns)
		// apart: the session's copies among those are told from our own by FrameOursOf.
		void WalkFrames(Clock::time_point a_now)
		{
			if (a_now < nextFrameWalk) {
				return;
			}
			nextFrameWalk = a_now + FRAME_WALK_INTERVAL;
			standingFrames.clear();
			standingMade.clear();
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			tes->ForEachReference([](RE::TESObjectREFR* a_ref) {
				const auto id = a_ref ? a_ref->GetFormID() : 0;
				const bool placed = Protocol::IsShareableRef(id);
				if ((placed || Protocol::IsSpawnRef(id)) && Equipment::IsPowerArmorFrame(a_ref->GetObjectReference()) && !a_ref->IsDeleted() && !a_ref->IsDisabled() && a_ref->Get3D()) {
					if (placed) {
						standingFrames.insert(id);
					} else if (standingMade.size() < MAX_MADE_SEEN) {
						standingMade.insert(id);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// Scan's part for frames our game made: each one ours to share (FrameShare::SharesMade) goes to the server under a
		// spawn id, and counts as loaded once granted (kept); one that no longer is (worn, gone, or a stranger's left in a
		// cell since unloaded) is not, and is let go of, which ends the copies in the other games. Our player's own first,
		// then the ones we share already, then the ones standing about (FrameShare::MAX_MADE).
		void ShareMade(std::unordered_set<std::uint32_t>& a_loaded)
		{
			std::erase_if(usedMade, [](std::uint32_t a_local) { return !MadeFrameRef(a_local); });
			std::vector<std::uint32_t> order(usedMade.begin(), usedMade.end());
			for (const auto& [id, track] : owned) {
				if (const auto local = Protocol::IsSpawnRef(id) ? Spawns::LocalOf(id) : 0; MadeFrameRef(local)) {
					order.push_back(local);
				}
			}
			order.insert(order.end(), standingMade.begin(), standingMade.end());
			std::unordered_set<std::uint32_t> seen;
			for (const auto local : order) {
				if (!seen.insert(local).second) {
					continue;
				}
				const auto frame = MadeFrameRef(local);
				const bool share = frame && FrameShare::SharesMade(FrameOursOf(frame), hosting, usedMade.contains(local));
				if (const auto spawn = Spawns::ShareFrame(frame, share)) {
					a_loaded.insert(spawn);
				}
			}
		}

		// Scan's part for frames: which count as loaded (kept while we run them), and which to offer to run. Our player
		// getting into or out of one takes it over from whoever runs it (FrameShare::Used), as a hit or a press takes
		// over an NPC.
		void ScanFrames(Clock::time_point a_now, std::unordered_set<std::uint32_t>& a_loaded, std::vector<Protocol::ActorClaim>& a_claims)
		{
			WalkFrames(a_now);
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto worn = player ? Equipment::PowerArmorFrameOf(player) : RE::NiPointer<RE::TESObjectREFR>{};
			const auto last = player ? player->lastUsedPowerArmor.get() : RE::NiPointer<RE::TESObjectREFR>{};
			const auto wornId = worn && !worn->IsDeleted() ? worn->GetFormID() : 0;
			const auto lastId = last && !last->IsDeleted() ? last->GetFormID() : 0;
			if (const auto used = FrameShare::Used(wornFrame, wornId, lastId); FrameRef(used)) {
				const bool fresh = usedFrames.insert(used).second;
				if (fresh || wornId != wornFrame) {
					REX::INFO("NpcSync: our player {} frame {:08X}; it is ours to run (owner {})", wornId != wornFrame ? (wornId ? "got into" : "got out of") : "last used", used, OwnerOf(used));
					OnLocalInteraction(used);  // taken over when another game runs it; offered below when nobody does
				}
			} else if (const auto made = MadeFrameRef(used); made && !SaveGuard::Placed(made) && usedMade.size() < MAX_MADE_SEEN) {
				// One our game made: ours, shared as a spawn wherever our world keeps it (ShareMade).
				if (usedMade.insert(used).second || wornId != wornFrame) {
					REX::INFO("NpcSync: our player {} frame {:08X}, made in our game; it is shared as a spawn while it stands", wornId != wornFrame ? (wornId ? "got into" : "got out of") : "last used", used);
				}
			}
			wornFrame = wornId;
			ShareMade(a_loaded);

			std::unordered_set<std::uint32_t> candidates = standingFrames;
			candidates.insert(usedFrames.begin(), usedFrames.end());
			for (const auto& [id, track] : owned) {
				if (FrameRef(id)) {
					candidates.insert(id);  // kept while it is still here, loaded or not
				}
			}
			for (const auto id : candidates) {
				const auto share = FrameShare::ChooseShare(FrameOursOf(FrameRef(id)));
				if (!FrameShare::Keeps(share)) {
					continue;
				}
				a_loaded.insert(id);
				const auto owner = OwnerOf(id);
				if (owner == localId || !FrameShare::Offers(share, hosting, usedFrames.contains(id))) {
					continue;
				}
				if (const auto reason = claimBook.Offer(id, owner, localId, false, WorldSync::MayClaim(id), a_now)) {
					a_claims.push_back({ id, *reason, false });
				}
			}
		}

		// The state of a frame we run, when one is due (FrameShare::Due): where it stands, nothing of movement. A pre-placed
		// one by its own id; one our game made under the spawn id it is shared as.
		std::optional<Protocol::ActorState> FrameState(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto placed = FrameRef(a_id);
			const auto made = placed ? nullptr : MadeFrameRef(Protocol::IsSpawnRef(a_id) ? Spawns::LocalOf(a_id) : 0);
			const auto frame = placed ? placed : made;
			if (!frame) {
				return std::nullopt;
			}
			const auto ours = FrameOursOf(frame);
			if (made ? !FrameShare::SharesMade(ours, hosting, usedMade.contains(made->GetFormID())) : !FrameShare::Keeps(FrameShare::ChooseShare(ours))) {
				return std::nullopt;
			}
			const auto sent = sentFrames.find(a_id);
			const bool moved = FrameShare::Moved(sent != sentFrames.end(), sent != sentFrames.end() ? sent->second.at : RemoteEdits::Spot{}, sent != sentFrames.end() ? sent->second.heading : 0.0f, ours.at, ours.heading);
			if (!FrameShare::Due(moved, sent != sentFrames.end() ? a_now - sent->second.sentAt : Clock::duration{})) {
				return std::nullopt;
			}
			Protocol::ActorState state;
			state.refId = a_id;
			state.cell = ours.at.cell;
			state.worldspace = ours.at.worldspace;
			state.x = ours.at.x;
			state.y = ours.at.y;
			state.z = ours.at.z;
			state.heading = ours.heading;
			sentFrames[a_id] = { ours.at, ours.heading, a_now };
			++frameStates;
			return state;
		}

		// Whether a spot is in the area our engine draws around our player: our interior, or outside within the cells
		// it attaches around ours (StandInRange::Visible). Not during a loading screen, when nothing is.
		bool InView(const RemoteEdits::Spot& a_spot, const RE::PlayerCharacter* a_player)
		{
			const auto cell = a_player ? a_player->GetParentCell() : nullptr;
			if (!cell || Hud::LoadingScreenUp()) {
				return false;
			}
			if (cell->IsInterior()) {
				return a_spot.cell == cell->GetFormID();
			}
			return a_spot.cell == 0 && cell->worldSpace && a_spot.worldspace == cell->worldSpace->GetFormID() &&
			       StandInRange::Visible(a_spot.x, a_spot.y, a_player->data.location.x, a_player->data.location.y, RemotePlayers::GridsToLoad());
		}

		const char* MoveText(FrameShare::Move a_move)
		{
			switch (a_move) {
			case FrameShare::Move::kThere:
				return "it stands there";
			case FrameShare::Move::kMissing:
				return "no such frame here";
			case FrameShare::Move::kNotFrame:
				return "not a power armor frame here";
			case FrameShare::Move::kOff:
				return "ours is disabled (worn here), left alone";
			case FrameShare::Move::kQuest:
				return "a quest of ours holds it, left alone";
			case FrameShare::Move::kInUse:
				return "someone here is in it, left alone";
			case FrameShare::Move::kOutOfView:
				return "where theirs stands is out of our sight; ours stays where it is";
			case FrameShare::Move::kNudge:
				return "moved there";
			case FrameShare::Move::kBring:
				return "brought there";
			}
			return "-";
		}

		// A frame another game runs stands somewhere in their world: ours is moved there, in place when it is loaded
		// near, else through the console as an NPC's copy is brought in (BringIn), a few tries at most (RetryBudget).
		void ApplyFrameState(RE::TESObjectREFR* a_frame, const Protocol::ActorState& a_state, std::uint32_t a_owner, Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto ours = FrameOursOf(a_frame);
			const auto theirs = RemoteEdits::SpotOf(a_state);
			const auto move = FrameShare::Decide(ours, theirs, a_state.heading, InView(theirs, player));
			const auto id = a_state.refId;
			// Each choice but the quiet ones once, as it changes: the states come every second.
			if (auto& logged = frameChoices[id]; logged != static_cast<int>(move) && move != FrameShare::Move::kNudge && move != FrameShare::Move::kBring) {
				logged = static_cast<int>(move);
				REX::INFO("NpcSync: player {}'s game has frame {:08X} at ({:.0f},{:.0f},{:.0f}) in {:08X}; ours is {} (cell {:08X} ws {:08X}): {}", a_owner, id, theirs.x, theirs.y, theirs.z, theirs.cell ? theirs.cell : theirs.worldspace,
					ours.loaded ? "loaded" : "not loaded", ours.at.cell, ours.at.worldspace, MoveText(move));
			}
			switch (move) {
			case FrameShare::Move::kThere:
				frameBrings.Forget(id);
				break;
			case FrameShare::Move::kNudge:
				{
					const float distance = RemoteEdits::Distance(ours.at, theirs);
					if (distance > 10.0f) {
						REX::INFO("NpcSync: frame {:08X} is moved {:.0f} units to ({:.0f},{:.0f},{:.0f}), where player {}'s game has it", id, distance, theirs.x, theirs.y, theirs.z, a_owner);
					}
					a_frame->data.location = { theirs.x, theirs.y, theirs.z };
					a_frame->data.angle = { 0.0f, 0.0f, a_state.heading };
					a_frame->Update3DPosition(true);
					++framesNudged;
					frameChoices[id] = static_cast<int>(move);
				}
				break;
			case FrameShare::Move::kBring:
				{
					const auto verdict = frameBrings.Ask(id, a_now);
					if (!RetryBudget::Go(verdict)) {
						break;
					}
					if (verdict == RetryBudget::Verdict::kFirst) {
						REX::INFO("NpcSync: frame {:08X} is brought to ({:.0f},{:.0f},{:.0f}) in {:08X}, where player {}'s game has it; ours was {} (cell {:08X} ws {:08X})", id, theirs.x, theirs.y, theirs.z, theirs.cell ? theirs.cell : theirs.worldspace, a_owner,
							ours.loaded ? "loaded elsewhere" : "in an unloaded cell", ours.at.cell, ours.at.worldspace);
					} else if (verdict == RetryBudget::Verdict::kLast) {
						REX::INFO("NpcSync: bringing frame {:08X} for the last time; after {} tries it stays where it is here", id, RetryBudget::TRIES);
					}
					RE::Console::ExecuteCommand(std::format("{:08X}.moveto player", id).c_str());
					RE::Console::ExecuteCommand(std::format("{:08X}.setpos x {:.0f}", id, theirs.x).c_str());
					RE::Console::ExecuteCommand(std::format("{:08X}.setpos y {:.0f}", id, theirs.y).c_str());
					RE::Console::ExecuteCommand(std::format("{:08X}.setpos z {:.0f}", id, theirs.z).c_str());
					RE::Console::ExecuteCommand(std::format("{:08X}.setangle z {:.1f}", id, a_state.heading * 180.0f / std::numbers::pi_v<float>).c_str());
					meter.Count(5);
					++framesBrought;
					frameChoices[id] = static_cast<int>(move);
				}
				break;
			default:
				break;
			}
		}

		// Our copy of the NPC as one of its owner's states finds it: what RetryBudget::Plan decides the fix-ups on.
		// The owner's copy stands near us outdoors, but ours is somewhere else: in a cell we have not loaded (a
		// companion that walked here with its player from across the map, say), or loaded in an interior or another
		// world its own quest left it in (the host finished Concord, so its Preston stands in Sanctuary; ours still
		// waits in the museum). Ours is then moved to where theirs is (BringIn) and mirrored from there; left where it
		// was, UpdateMirrors would only hand it to its own AI, and the friend's Preston would never appear here.
		// NpcPlace::Decide says when (never out of a cell our player is in with it, nor one busy with us).
		RetryBudget::Copy CopyFor(const Protocol::ActorState& a_state, std::uint32_t a_owner)
		{
			const auto actor = Protocol::IsShareableRef(a_state.refId) ? RE::TESForm::GetFormByID<RE::Actor>(a_state.refId) : nullptr;
			if (!actor) {
				return {};
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto ours = OursOf(actor, a_state.refId);
			const auto why = NpcPlace::Decide(RemoteEdits::SpotOf(a_state), ours, player && player->GetParentCell() ? SpotOf(player) : NpcPlace::Spot{}, OwnerBeside(a_owner, a_state));
			auto       copy = NpcPlace::CopyOf(ours, why);
			// A character of our own story stays as our story has it: a companion or quest character a friend's
			// story has brought in, ours has not yet. It is never switched on here.
			if (copy.off && !copy.unusable && !RemoteEdits::MaySwitchOn(Story::Shared(), WorldSync::DescribeStory(actor))) {
				copy.unusable = true;
				if (keptOff.insert(a_state.refId).second) {
					++keptOffStory;
					REX::INFO("NpcSync: leaving {:08X} off: its owner's copy runs, but it is a character of our own story", a_state.refId);
				}
			}
			// One whose owner's stand-in is not beside it is not moved: a companion walks with its player, and their
			// game saying some NPC stands next to us is otherwise only their word for it.
			if (why == NpcPlace::Why::kOwnerAway) {
				if (notBrought.insert(a_state.refId).second) {
					++notBroughtOwnerFar;
					REX::INFO("NpcSync: not bringing {:08X} in: player {}'s stand-in is not beside where their game has it", a_state.refId, a_owner);
				}
			} else if (copy.movable) {
				notBrought.erase(a_state.refId);
			}
			return copy;
		}

		// Moves our copy to where the owner's stands (RetryBudget::Plan said to).
		void BringIn(const Protocol::ActorState& a_state, RetryBudget::Verdict a_verdict)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_state.refId);
			if (!actor) {
				return;
			}
			const RE::NiPoint3 at{ a_state.x, a_state.y, a_state.z };
			const auto         ours = SpaceOf(actor);
			++broughtIn;
			if (a_verdict == RetryBudget::Verdict::kLast) {
				REX::INFO("NpcSync: bringing {:08X} in for the last time; after {} tries it stays where it is here", a_state.refId, RetryBudget::TRIES);
			} else if (a_verdict == RetryBudget::Verdict::kFirst) {
				REX::INFO("NpcSync: bringing {:08X} in to ({:.0f},{:.0f},{:.0f}) where its owner's copy is; ours was {} (cell {:08X} ws {:08X})", a_state.refId, at.x, at.y, at.z,
					actor->Get3D() ? "loaded elsewhere" : "in an unloaded cell", ours.first, ours.second);
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.moveto player", a_state.refId).c_str());
			RE::Console::ExecuteCommand(std::format("{:08X}.setpos x {:.0f}", a_state.refId, at.x).c_str());
			RE::Console::ExecuteCommand(std::format("{:08X}.setpos y {:.0f}", a_state.refId, at.y).c_str());
			RE::Console::ExecuteCommand(std::format("{:08X}.setpos z {:.0f}", a_state.refId, at.z).c_str());
			meter.Count(4);
		}

		// The owner's game switched the NPC on (a script enabled it: the mole rats that burrow up at Red
		// Rocket, an ambush), ours is still off: it is switched on here too, through the console's enable
		// like the kills, else the friend fights something we never see. It is mirrored once it loads.
		// RetryBudget::Plan said to (never the player, the dead or a deleted one).
		void SwitchOn(const Protocol::ActorState& a_state, RetryBudget::Verdict a_verdict)
		{
			++switchedOn;
			if (a_verdict == RetryBudget::Verdict::kFirst) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_state.refId);
				REX::INFO("NpcSync: switching {:08X} on: it is off here but its owner's copy is running{}", a_state.refId,
					actor && actor->extraList && actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent) ? " (an enable parent switches it here)" : "");
			} else if (a_verdict == RetryBudget::Verdict::kLast) {
				REX::INFO("NpcSync: switching {:08X} on for the last time; after {} tries it stays off here", a_state.refId, RetryBudget::TRIES);
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.enable", a_state.refId).c_str());
			meter.Count(1);
		}

		void UpdateMirrors(Clock::time_point a_now)
		{
			for (auto it = mirrors.begin(); it != mirrors.end();) {
				const auto id = it->first;
				auto&      mirror = it->second;
				const auto owner = OwnerOf(id);
				const auto actor = LoadedActor(id);
				if (owner == 0 || owner == localId || !actor || actor->IsDead(false)) {
					StopMirroring(id, mirror);
					it = mirrors.erase(it);
					continue;
				}
				// We're talking to it, or it is in the scene we're in: our own copy talks to us (dialogue and scenes are
				// each game's own), so it runs its own AI here until they end. Our own companion follows us here (two
				// players can have the same companion). Whoever runs it elsewhere (NpcPlace::StaysWithUs).
				if (NpcPlace::StaysWithUs({ .companion = IsOurCompanion(actor), .talkingToUs = id == talkingTo, .holdsUs = Dialogue::Holds(actor) })) {
					StopMirroring(id, mirror);
					++it;
					continue;
				}
				// The owner's copy is somewhere else (e.g. a companion that followed its player through
				// a door), far away, or hasn't been heard from: ours runs its own AI meanwhile. (Theirs
				// outdoors next to us while ours is elsewhere is BringIn's case, from ApplyStates.)
				const auto* latest = mirror.snapshots.empty() ? nullptr : &mirror.snapshots.back();
				// Outdoors in the same world, the owner's copy went far (a companion ran after its player, or
				// was moved): ours is brought over and mirrored from there, instead of being left behind; as
				// long as that is beside its owner's stand-in (OwnerBeside), else ours runs its own AI here. That
				// goes for any NPC their game runs, not only companions: a settler their game moved far from them
				// stays here until they are beside it.
				if (latest && a_now - latest->received <= LOST_AFTER && latest->state.cell == 0 && SpaceOf(actor) == std::pair{ 0u, latest->state.worldspace } &&
					actor->data.location.GetDistance({ latest->state.x, latest->state.y, latest->state.z }) > MAX_FOLLOW_DISTANCE) {
					if (OwnerBeside(owner, latest->state)) {
						actor->SetPosition({ latest->state.x, latest->state.y, latest->state.z }, true);
						++broughtOver;
						notBroughtOver.erase(id);
					} else if (notBroughtOver.insert(id).second) {
						++notBroughtOverOwnerFar;
						REX::INFO("NpcSync: not bringing {:08X} over: player {}'s stand-in is not beside where their game has it", id, owner);
					}
				}
				if (!latest || !NpcPlace::Follows(SpotOf(actor), RemoteEdits::SpotOf(latest->state), a_now - latest->received)) {
					if (mirror.registered) {
						++localFallbacks;
					}
					StopMirroring(id, mirror);
					++it;
					continue;
				}
				if (mirror.registered != actor) {
					// A few a frame (after a pause menu, every copy at a busy settlement is taken over at once): the rest
					// keep their snapshots and are taken over in the next frames.
					if (!meter.TryRegister()) {
						++it;
						continue;
					}
					StopMirroring(id, mirror);
					Puppets::Register(actor, Puppets::Kind::kNpc);
					mirror.registered = actor;
					mirror.registeredId = actor->GetFormID();
					if (const auto gear = knownGear.find(id); gear != knownGear.end()) {
						meter.Count(Equipment::Wear(actor, gear->second));
						++gearApplied;
					}
				}
				AdjustDelay(mirror);
				if (const auto motion = Sample(mirror, a_now)) {
					Puppets::SetTarget(actor, *motion);
				}
				++it;
			}
		}
	}

	void SetLocalPlayer(std::uint32_t a_id)
	{
		localId = a_id;
	}

	std::uint32_t LocalPlayer()
	{
		return localId;
	}

	void ApplyOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		const auto now = Clock::now();
		for (const auto& entry : a_owners) {
			claimBook.Answered(entry.refId, entry.playerId, localId, now);
			if (entry.playerId != localId) {
				hold.Forget(entry.refId);  // no longer ours to keep while it dies
			}
			if (entry.playerId == 0) {
				owners.erase(entry.refId);
				owned.erase(entry.refId);
				sentGear.erase(entry.refId);  // the server forgot its gear with its runner: report it afresh when we run it
				StopMirroring(entry.refId);
				continue;
			}
			owners[entry.refId] = entry.playerId;
			if (entry.playerId == localId) {
				owned.try_emplace(entry.refId);
				StopMirroring(entry.refId);
			} else {
				owned.erase(entry.refId);
				sentGear.erase(entry.refId);  // when we run it again, report its gear afresh
			}
		}
	}

	void ApplyStates(const std::vector<Protocol::ActorState>& a_states)
	{
		if (Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		for (const auto& state : a_states) {
			const auto owner = OwnerOf(state.refId);
			if (owner == 0 || owner == localId) {
				continue;
			}
			// A friend's spawn: our copy is placed where their game has it (Spawns), never switched on or brought in.
			const bool spawn = Protocol::IsSpawnRef(state.refId);
			if (spawn) {
				Spawns::OnState(state);
			}
			// A power armor frame their game runs: ours is moved to where theirs stands (FrameShare), nothing mirrored.
			if (const auto frame = spawn ? nullptr : FrameRef(state.refId)) {
				ApplyFrameState(frame, state, owner, now);
				continue;
			}
			// Off here: switched on. Not loaded (a friend's companion that came from somewhere we have not loaded), or
			// loaded but not where the owner's copy is (ours waits in an interior its own quest left it in): brought
			// over, and mirrored once it is here; the snapshot is kept meanwhile. A few tries each (RetryBudget).
			const auto fixes = spawn ? RetryBudget::Fixes{} : RetryBudget::Plan(switchOns, bringIns, state.refId, CopyFor(state, owner), now);
			if (RetryBudget::Go(fixes.switchOn)) {
				SwitchOn(state, fixes.switchOn);
			}
			if (RetryBudget::Go(fixes.bringIn)) {
				BringIn(state, fixes.bringIn);
			}
			const auto actor = LoadedActor(state.refId);
			if (!actor) {
				continue;
			}
			auto& mirror = mirrors[state.refId];
			auto& snaps = mirror.snapshots;
			snaps.push_back({ mirror.arrivals.Next(now), state });
			while (snaps.size() > MAX_SNAPSHOTS) {
				snaps.pop_front();
			}
		}
	}

	bool IsOurCompanion(RE::Actor* a_actor)
	{
		static const auto faction = RE::TESForm::GetFormByID<RE::TESFaction>(CURRENT_COMPANION_FACTION);
		return a_actor && faction && a_actor->IsInFaction(faction);
	}

	bool RunsLocally(std::uint32_t a_refId)
	{
		return owned.contains(a_refId);
	}

	std::uint32_t TalkingTo()
	{
		return talkingTo;
	}

	void ApplyShot(std::uint32_t a_refId, std::uint32_t a_sound)
	{
		const auto it = mirrors.find(a_refId);
		const auto actor = it != mirrors.end() ? LoadedActor(a_refId) : nullptr;
		if (actor && it->second.registered == actor) {
			WeaponFire::PlayShot(actor, a_sound);
		}
	}

	void ApplyAction(std::uint32_t a_refId, std::uint8_t a_action)
	{
		const auto it = mirrors.find(a_refId);
		const auto actor = it != mirrors.end() ? LoadedActor(a_refId) : nullptr;
		if (actor && it->second.registered == actor) {
			// A mirror's replayed throw shows and throws the grenade its owner's copy holds (if the owner reported one).
			if (const auto gear = a_action == Protocol::ShotAction::kThrow ? knownGear.find(a_refId) : knownGear.end(); gear != knownGear.end()) {
				Equipment::HoldThrowable(actor, gear->second);
			}
			WeaponFire::PlayAction(actor, a_action);
		}
	}

	void OnLocalInteraction(std::uint32_t a_refId)
	{
		hold.Fought(a_refId, Clock::now());  // ours or not: one we run is not handed away mid-fight
		const auto owner = OwnerOf(a_refId);
		if (localId == 0 || owner == 0 || owner == localId || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		const auto last = lastTakeover.find(a_refId);
		if (last != lastTakeover.end() && now - last->second < TAKEOVER_COOLDOWN) {
			return;
		}
		lastTakeover[a_refId] = now;
		claimBook.Claimed(a_refId, Protocol::ClaimReason::kInteract, now);
		outgoing.push_back({ Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_refId, Protocol::ClaimReason::kInteract } }), true });
	}

	bool FoughtLately(std::uint32_t a_refId)
	{
		return hold.FoughtLately(a_refId, Clock::now());
	}

	void OnActivated(std::uint32_t a_refId)
	{
		lastActivated = a_refId;
		lastActivatedAt = Clock::now();
	}

	void Frame(bool a_hosting)
	{
		hosting = a_hosting;
		// The last frame's console commands, wherever they ran (WorldSync's kills count here too).
		if (meter.EndFrame(Clock::now())) {
			REX::INFO("NpcSync: {} console commands for NPCs in one frame (the most so far {}, busy frames {})", meter.Last(), meter.Most(), meter.BusyFrames());
		}
		if (localId == 0) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return;
		}
		const auto now = Clock::now();
		if (lastFrame != Clock::time_point{} && now - lastFrame < 500ms) {  // a load or a pause is not the frame rate
			frameMs = frameMs * 0.95f + std::chrono::duration<float, std::milli>(now - lastFrame).count() * 0.05f;
		}
		lastFrame = now;
		// In the opening our NPCs are nobody else's business, and nobody else's run ours: its
		// scenes need every NPC on its own AI.
		if (Story::InOpening()) {
			LeaveAll();
			return;
		}
		if (now >= nextScan) {
			nextScan = now + SCAN_INTERVAL;
			Scan(now);
		}
		if (now >= nextStates) {
			// On schedule, not an interval after this frame (see Session's SEND_INTERVAL).
			nextStates += STATE_INTERVAL;
			if (nextStates <= now) {
				nextStates = now + STATE_INTERVAL;
			}
			SendOwnedStates(now);
		}
		if (now >= nextGear) {
			nextGear = now + GEAR_INTERVAL;
			SendOwnedGear();
			std::vector<std::uint32_t> ids;
			for (const auto& [id, track] : owned) {
				if (const auto local = LocalId(id)) {
					ids.push_back(local);  // WeaponFire's animation events name our game's NPC
				}
			}
			WeaponFire::SetOwnedNpcs(std::move(ids));
		}
		// The partner is found when the conversation starts and kept until it ends.
		const bool inConversation = OpenMenus::Open(OpenMenus::Menu::kDialogue);
		if (!inConversation) {
			talkingTo = 0;
		} else if (talkingTo == 0) {
			talkingTo = FindPartner(now);
			nextTalkClaim = {};
			conversations += talkingTo != 0;
			if (talkingTo) {
				REX::INFO("NpcSync: talking to {:08X} (owner {})", talkingTo, OwnerOf(talkingTo));
			}
		}
		// The NPC we talk to, and those holding us (the conversation's speaker, the others in our scene), are claimed
		// again while that lasts when we run them, so nobody takes them over mid-scene; else they're taken over.
		if ((talkingTo || !holding.empty()) && now >= nextTalkClaim) {
			nextTalkClaim = now + TALK_CLAIM_INTERVAL;
			auto keep = holding;
			if (talkingTo && !std::ranges::contains(keep, talkingTo)) {
				keep.push_back(talkingTo);
			}
			std::vector<Protocol::ActorClaim> claims;
			for (const auto id : keep) {
				if (OwnerOf(id) == localId) {
					claims.push_back({ id, Protocol::ClaimReason::kInteract });
				} else {
					OnLocalInteraction(id);
				}
			}
			SendList(claims, [](const std::vector<Protocol::ActorClaim>& a) { return Protocol::Encode(a); }, true);
		}
		UpdateMirrors(now);
	}

	void CountCommands(std::uint32_t a_commands)
	{
		meter.Count(a_commands);
	}

	std::vector<Outgoing> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void DropMirror(std::uint32_t a_refId)
	{
		StopMirroring(a_refId);
	}

	void ReleaseMirrors()
	{
		for (auto& [id, mirror] : mirrors) {
			StopMirroring(id, mirror);
		}
		mirrors.clear();
	}

	void ApplyEquipment(const Protocol::NpcEquipment& a_equipment)
	{
		if (a_equipment.playerId == localId) {
			return;
		}
		knownGear[a_equipment.refId] = a_equipment.items;
		// The mirror's actor is resolved afresh: its cell may have unloaded since the last frame, and the
		// stored pointer is only compared, never followed.
		const auto actor = LoadedActor(a_equipment.refId);
		if (const auto it = mirrors.find(a_equipment.refId); actor && it != mirrors.end() && it->second.registered == actor) {
			meter.Count(Equipment::Wear(actor, a_equipment.items));
			++gearApplied;
		}
	}

	void Reset()
	{
		ReleaseMirrors();
		keptOff.clear();
		notBrought.clear();
		notBroughtOver.clear();
		localId = 0;
		sentGear.clear();
		knownGear.clear();
		owners.clear();
		claimBook.Clear();
		hold.Clear();
		lastTakeover.clear();
		handedOffAt.clear();
		talkingTo = 0;
		holding.clear();
		owned.clear();
		switchOns.Clear();
		bringIns.Clear();
		outgoing.clear();
		standingFrames.clear();
		usedFrames.clear();
		standingMade.clear();
		usedMade.clear();
		wornFrame = 0;
		sentFrames.clear();
		frameBrings.Clear();
		frameChoices.clear();
		nextFrameWalk = {};
	}

	bool InSight(const RemoteEdits::Spot& a_spot)
	{
		return InView(a_spot, RE::PlayerCharacter::GetSingleton());
	}

	bool InFrame(const RE::TESObjectREFR* a_frame)
	{
		return FrameInUse(a_frame);
	}

	std::string DescribeMirror(std::uint32_t a_refId)
	{
		if (const auto frame = FrameRef(a_refId)) {
			const auto space = SpaceOf(frame);
			const auto sent = sentFrames.find(a_refId);
			const auto choice = frameChoices.find(a_refId);
			return std::format("frame owner={} owned={} standing={} used={} loaded={} off={} created={} ours=({:.0f},{:.0f},{:.0f}) heading={:.2f} cell={:08X} ws={:08X} sent={} lastMove={}", OwnerOf(a_refId), owned.contains(a_refId),
				standingFrames.contains(a_refId), usedFrames.contains(a_refId), frame->Get3D() != nullptr, frame->IsDisabled(), frame->IsCreated(), frame->data.location.x, frame->data.location.y, frame->data.location.z, frame->data.angle.z,
				space.first, space.second, sent != sentFrames.end(), choice != frameChoices.end() ? choice->second : -1);
		}
		const auto it = mirrors.find(a_refId);
		const auto actor = LoadedActor(a_refId);
		std::string out = std::format("owner={} owned={} mirror={} loaded={}", OwnerOf(a_refId), owned.contains(a_refId), it != mirrors.end(), actor != nullptr);
		if (it == mirrors.end()) {
			return out;
		}
		const auto& m = it->second;
		out += std::format(" registered={} snapshots={} delayMs={:.0f}", m.registered != nullptr, m.snapshots.size(), m.delayMs);
		if (!m.snapshots.empty()) {
			const auto& s = m.snapshots.back();
			const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.received).count();
			out += std::format(" lastAgeMs={} state=({:.0f},{:.0f},{:.0f}) cell={:08X} ws={:08X}", age, s.state.x, s.state.y, s.state.z, s.state.cell, s.state.worldspace);
			if (actor) {
				const auto space = SpaceOf(actor);
				out += std::format(" ours=({:.0f},{:.0f},{:.0f}) ourCell={:08X} ourWs={:08X} dist={:.0f} dead={}", actor->data.location.x, actor->data.location.y, actor->data.location.z, space.first, space.second, actor->data.location.GetDistance({ s.state.x, s.state.y, s.state.z }), actor->IsDead(false));
			}
		}
		if (actor && m.registered == actor) {
			out += " " + Puppets::Describe(actor);
		}
		return out;
	}

	std::uint32_t Owner(std::uint32_t a_refId)
	{
		return OwnerOf(a_refId);
	}

	std::uint32_t SessionId(std::uint32_t a_formId)
	{
		if (Protocol::IsSpawnRef(a_formId)) {
			return Spawns::IdOf(a_formId);
		}
		return Protocol::IsShareableRef(a_formId) ? a_formId : 0;
	}

	std::uint32_t LocalRef(std::uint32_t a_sessionId)
	{
		return LocalId(a_sessionId);
	}

	std::string Describe()
	{
		const auto puppeted = std::ranges::count_if(mirrors, [](const auto& a_entry) { return a_entry.second.registered != nullptr; });
		const auto delays = mirrors | std::views::transform([](const auto& a_entry) { return static_cast<int>(a_entry.second.delayMs); });
		const auto minDelay = mirrors.empty() ? 0 : std::ranges::min(delays);
		const auto maxDelay = mirrors.empty() ? 0 : std::ranges::max(delays);
		const auto ownedFrames = std::ranges::count_if(owned, [](const auto& a_entry) { return FrameRef(a_entry.first) != nullptr; });
		const auto sharedMade = std::ranges::count_if(owned, [](const auto& a_entry) { return Protocol::IsSpawnRef(a_entry.first) && MadeFrameRef(Spawns::LocalOf(a_entry.first)) != nullptr; });
		return std::format("npcs: owned={} fast={} mirrored={} puppeted={} known={} talkingTo={:08X} conversations={} fallbacks={} brought={}/{} switchedOn={} handOffs={} delayMs={}-{} gear={}/{} refusals={} gaveUp={}/{} cmds={}/{} busy={} rereg={} released={} keptOffStory={} notBroughtOwnerFar={} notBroughtOverOwnerFar={} frames={}/{} framesMade={}/{}/{} frameStates={} framesMoved={}/{} frameGaveUp={}", owned.size(),
			fastNpcs, mirrors.size(), puppeted, owners.size(), talkingTo, conversations, localFallbacks, broughtOver, broughtIn, switchedOn, handOffs, minDelay, maxDelay, gearSent, gearApplied, claimBook.Refusals(), switchOns.GaveUp(), bringIns.GaveUp(), meter.Last(), meter.Most(), meter.BusyFrames(), meter.Registrations(), meter.Releases(),
			keptOffStory, notBroughtOwnerFar, notBroughtOverOwnerFar, ownedFrames, standingFrames.size(), sharedMade, standingMade.size(), usedMade.size(), frameStates, framesNudged, framesBrought, frameBrings.GaveUp());
	}
}
