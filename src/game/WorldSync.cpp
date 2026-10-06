#include "game/WorldSync.h"

#include "game/NpcSync.h"
#include "game/QuestSync.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"

namespace WorldSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Protocol::IsShareableRef;

		constexpr auto APPLY_INTERVAL = 250ms;

		// Every actor known to be dead this session, and those we still have to kill locally
		// (they weren't loaded when we heard about them).
		std::unordered_set<std::uint32_t> dead;
		std::unordered_set<std::uint32_t> pending;

		// Game events can arrive on any thread, so sinks only queue IDs; Frame() does the work.
		std::mutex                 inboxLock;
		std::vector<std::uint32_t> deathInbox;
		std::unordered_set<std::uint32_t> killInbox;  // deaths in deathInbox the player caused
		std::atomic<std::uint32_t>        playerKills{ 0 };  // anything the player killed, shared or not
		std::vector<std::uint32_t> hitInbox;
		std::vector<std::vector<std::uint8_t>> lootInbox;  // already-encoded reports
		std::vector<std::uint32_t>             pickupInbox;  // base forms picked up from the world
		struct StandInHit
		{
			std::uint32_t actor;
			float         damage;
			bool          byPlayer;  // we hit them, rather than an NPC in our world
		};
		std::vector<StandInHit> standInHits;
		std::atomic<bool>       friendlyFire{ false };

		struct Activation
		{
			std::uint32_t     ref;
			std::uint32_t     base;
			Clock::time_point time{};
		};
		std::vector<Activation> activateInbox;

		// Recent local activations and world pickups, matched up in Frame() (either can arrive first).
		constexpr auto          MATCH_WINDOW = 2s;
		std::vector<Activation> recentActivations;
		std::vector<Activation> unmatchedPickups;  // ref unused

		// Doors and lockable things the player used recently. Their open/lock state is reported
		// whenever it changes while watched (lockpicking can take a while, hence the long window).
		constexpr auto WATCH_TIME = 60s;
		struct Watched
		{
			Protocol::RefState lastSent;
			Clock::time_point  until;
		};
		std::unordered_map<std::uint32_t, Watched>           watched;
		std::unordered_map<std::uint32_t, Protocol::RefState> pendingRefStates;

		// Container changes are numbered by the server. Everything before containerNext is in our
		// world already or waiting in pendingContainer (its container isn't loaded). This belongs
		// to the world rather than the connection, so it is kept in the save (see SaveResyncPoint).
		struct PendingChange
		{
			std::uint32_t             index;
			Protocol::ContainerChange change;
		};
		std::uint64_t              session = 0;
		std::uint32_t              containerNext = 0;
		std::vector<PendingChange> pendingContainer;
		std::uint32_t              localPlayer = 0;

		std::unordered_set<std::uint32_t> pendingPickups;

		// Actors we hit last frame; their resulting health is reported this frame, once the
		// damage has been applied.
		std::unordered_set<std::uint32_t> hitLastFrame;
		// Health values from other players for actors that weren't loaded yet.
		std::unordered_map<std::uint32_t, float> pendingHealth;

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextApply{};
		std::uint32_t                          applied = 0;
		std::uint32_t                          reported = 0;

		// A loaded, real (not a stand-in) actor, or nullptr.
		RE::Actor* LoadedActor(std::uint32_t a_refId)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
			if (!actor || actor->IsDeleted() || actor->IsPlayerRef() || Puppets::IsPuppet(actor) || !actor->Get3D() || !actor->GetParentCell()) {
				return nullptr;
			}
			return actor;
		}

		// Kills an actor that died in another player's world. Returns true once it is dead here
		// (or can never be), false to try again later.
		bool TryKill(std::uint32_t a_refId)
		{
			if (!InWorld()) {
				return false;
			}
			const auto form = RE::TESForm::GetFormByID(a_refId);
			if (form && !form->As<RE::Actor>()) {
				return true;  // not an actor: nothing to kill
			}
			const auto actor = form ? form->As<RE::Actor>() : nullptr;
			if (actor && (actor->IsPlayerRef() || Puppets::IsPuppet(actor))) {
				return true;
			}
			if (actor && actor->IsDead(false)) {
				return true;
			}
			if (!LoadedActor(a_refId)) {
				return false;  // not loaded yet; try again when its cell loads
			}

			// The console kill goes through the engine's normal death path (death animation,
			// ragdoll, quest/script death events), exactly as if it happened here.
			RE::Console::ExecuteCommand(std::format("{:08X}.kill", a_refId).c_str());
			++applied;
			REX::INFO("WorldSync: killed {:08X} (died in another player's world)", a_refId);
			return true;
		}

		class DeathWatcher :
			public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				const auto ref = a_event.actorDying.get();
				if (!a_event.dead || !ref) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto id = ref->GetFormID();
				const auto actor = ref->As<RE::Actor>();
				const auto killer = a_event.actorKiller.get();
				const bool byPlayer = killer && killer->IsPlayerRef() && !ref->IsPlayerRef();
				if (actor && Puppets::IsPuppet(actor)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (byPlayer) {
					++playerKills;
				}
				if (!IsShareableRef(id)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				deathInbox.push_back(id);
				if (byPlayer) {
					killInbox.insert(id);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		DeathWatcher deathWatcher;

		const RE::ActorValueInfo* HealthInfo()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->health : nullptr;
		}

		// Sets a loaded actor's current health to a fraction of its maximum. Returns false to try again later.
		bool TrySetHealth(std::uint32_t a_refId, float a_fraction)
		{
			const auto info = HealthInfo();
			const auto actor = InWorld() ? LoadedActor(a_refId) : nullptr;
			if (!info || !actor) {
				return false;
			}
			if (actor->IsDead(false)) {
				return true;
			}
			auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
			const float maximum = values.GetPermanentActorValue(*info);
			const float target = std::clamp(a_fraction, 0.0f, 1.0f) * maximum;
			const float delta = target - values.GetActorValue(*info);
			if (delta < 0.0f) {
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, delta);
			} else if (delta > 0.0f) {
				values.RestoreActorValue(*info, delta);
			}
			return true;
		}

		class HitWatcher :
			public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				const auto target = a_event.target.get();
				const auto cause = a_event.cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// An NPC in our world (or we, with friendly fire on) hit another player's stand-in:
				// that player takes the damage. Hits on mirrored NPCs are dropped in Frame().
				const bool byPlayer = cause->IsPlayerRef();
				const auto victim = target->As<RE::Actor>();
				if (victim && Puppets::IsPuppet(victim)) {
					if ((!byPlayer || friendlyFire) && a_event.usesHitData) {
						const float damage = a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage;
						if (damage > 0.0f) {
							std::scoped_lock l{ inboxLock };
							standInHits.push_back({ victim->GetFormID(), damage, byPlayer });
						}
					}
				}
				if (!byPlayer) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Only damage we dealt to others (not ourselves); each player reports their own hits.
				const auto actor = target->As<RE::Actor>();
				if (actor && IsShareableRef(target->GetFormID()) && !Puppets::IsPuppet(actor)) {
					std::scoped_lock l{ inboxLock };
					hitInbox.push_back(target->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitWatcher hitWatcher;

		class ContainerWatcher :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Only moves the local player makes. Changes we apply for other players go between a
				// container and nothing, so they never match and never echo back.
				using Protocol::PLAYER_REF_ID;
				std::optional<std::vector<std::uint8_t>> packet;
				const auto count = a_event.itemCount;
				if (a_event.newContainerFormID == PLAYER_REF_ID && a_event.oldContainerFormID == 0) {
					// Picked up from the world. The event doesn't say which reference (referenceFormID is 0),
					// so Frame() pairs it with the item the player just activated.
					std::scoped_lock l{ inboxLock };
					pickupInbox.push_back(a_event.baseObjectFormID);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (count <= 0 || count > Protocol::MAX_ITEM_COUNT) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event.newContainerFormID == PLAYER_REF_ID && IsShareableRef(a_event.oldContainerFormID)) {
					packet = Protocol::Encode(Protocol::ContainerChange{ a_event.oldContainerFormID, a_event.baseObjectFormID, -count }, Protocol::MessageType::kReportContainer);
				} else if (a_event.oldContainerFormID == PLAYER_REF_ID && IsShareableRef(a_event.newContainerFormID)) {
					packet = Protocol::Encode(Protocol::ContainerChange{ a_event.newContainerFormID, a_event.baseObjectFormID, count }, Protocol::MessageType::kReportContainer);
				}
				if (packet) {
					std::scoped_lock l{ inboxLock };
					lootInbox.push_back(std::move(*packet));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ContainerWatcher containerWatcher;

		class ActivateWatcher :
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				const auto& target = a_event.objectActivated;
				const auto& by = a_event.actionRef;
				if (!target || !by || !by->IsPlayerRef() || !IsShareableRef(target->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto base = target->GetObjectReference();
				std::scoped_lock l{ inboxLock };
				activateInbox.push_back({ target->GetFormID(), base ? base->GetFormID() : 0 });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ActivateWatcher activateWatcher;

		// Things that can sit in an inventory.
		bool IsInventoryForm(const RE::TESForm* a_form)
		{
			using Type = RE::ENUM_FORM_ID;
			if (!a_form) {
				return false;
			}
			switch (a_form->GetFormType()) {
			case Type::kARMO:
			case Type::kBOOK:
			case Type::kMISC:
			case Type::kWEAP:
			case Type::kAMMO:
			case Type::kKEYM:
			case Type::kALCH:
			case Type::kNOTE:
			case Type::kINGR:
			case Type::kCMPO:
			case Type::kOMOD:
				return true;
			default:
				return false;
			}
		}

		// Containers, and NPCs (corpses, merchants): what another player can take from or put into.
		bool IsItemHolder(RE::TESObjectREFR* a_ref)
		{
			if (const auto actor = a_ref->As<RE::Actor>()) {
				return !actor->IsPlayerRef() && !Puppets::IsPuppet(actor);
			}
			const auto base = a_ref->GetObjectReference();
			return base && base->GetFormType() == RE::ENUM_FORM_ID::kCONT;
		}

		// Applies another player's container change. Returns false to try again later.
		bool TryApplyContainer(const Protocol::ContainerChange& a_change)
		{
			if (!InWorld()) {
				return false;
			}
			const auto container = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_change.container);
			if (!container || container->IsDeleted()) {
				return false;  // not loaded yet
			}
			if (!IsItemHolder(container) || !IsInventoryForm(RE::TESForm::GetFormByID(a_change.item))) {
				return true;  // nothing sensible to do
			}
			const auto command = a_change.count < 0 ?
				std::format("{:08X}.removeitem {:08X} {}", a_change.container, a_change.item, -static_cast<std::int64_t>(a_change.count)) :
				std::format("{:08X}.additem {:08X} {}", a_change.container, a_change.item, a_change.count);
			RE::Console::ExecuteCommand(command.c_str());
			return true;
		}

		// A session container change reached us (live or in a WorldState).
		void OfferContainerChange(std::uint32_t a_index, const Protocol::ContainerChange& a_change, bool a_alreadyInWorld)
		{
			if (a_index < containerNext) {
				return;  // already have it
			}
			containerNext = a_index + 1;
			if (a_alreadyInWorld || !Protocol::IsValid(a_change)) {
				return;
			}
			// Changes to one container stay in order; different containers don't wait for each other.
			const bool waiting = std::ranges::any_of(pendingContainer, [&](const PendingChange& a_pending) { return a_pending.change.container == a_change.container; });
			if (waiting || !TryApplyContainer(a_change)) {
				pendingContainer.push_back({ a_index, a_change });
			}
		}

		void RetryPendingContainers()
		{
			std::unordered_set<std::uint32_t> blocked;
			std::erase_if(pendingContainer, [&](const PendingChange& a_pending) {
				if (blocked.contains(a_pending.change.container)) {
					return false;
				}
				if (TryApplyContainer(a_pending.change)) {
					return true;
				}
				blocked.insert(a_pending.change.container);
				return false;
			});
		}

		// Pairs world pickups with the reference the player activated, and reports them.
		void MatchPickups(Clock::time_point a_now, std::vector<Activation>& a_activations, const std::vector<std::uint32_t>& a_pickups)
		{
			for (auto& activation : a_activations) {
				activation.time = a_now;
				recentActivations.push_back(activation);
			}
			for (const auto base : a_pickups) {
				unmatchedPickups.push_back({ 0, base, a_now });
			}
			std::erase_if(unmatchedPickups, [&](const Activation& a_pickup) {
				const auto match = std::ranges::find(recentActivations, a_pickup.base, &Activation::base);
				if (match == recentActivations.end()) {
					return false;
				}
				outgoing.push_back(Protocol::Encode(Protocol::RefPickedUp{ match->ref }, Protocol::MessageType::kReportPickup));
				recentActivations.erase(match);
				return true;
			});
			const auto expired = [&](const Activation& a_entry) { return a_now - a_entry.time > MATCH_WINDOW; };
			std::erase_if(recentActivations, expired);
			std::erase_if(unmatchedPickups, expired);
		}

		// Current open/lock state of a loaded reference, or nullopt if it isn't loaded or has neither.
		std::optional<Protocol::RefState> ReadRefState(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsDeleted() || !a_ref->Get3D()) {
				return std::nullopt;
			}
			using Open = RE::BGSOpenCloseForm::OPEN_STATE;
			Protocol::RefState state{ a_ref->GetFormID() };
			switch (RE::BGSOpenCloseForm::GetOpenState(a_ref)) {
			case Open::kOpen:
			case Open::kOpening:
				state.open = Protocol::RefState::kYes;
				break;
			case Open::kClosed:
			case Open::kClosing:
				state.open = Protocol::RefState::kNo;
				break;
			default:
				break;
			}
			if (const auto lock = a_ref->GetLock()) {
				state.locked = (lock->flags & std::to_underlying(RE::REFR_LOCK::Flags::kLocked)) ? Protocol::RefState::kYes : Protocol::RefState::kNo;
			}
			if (state.open == Protocol::RefState::kUnknown && state.locked == Protocol::RefState::kUnknown) {
				return std::nullopt;
			}
			return state;
		}

		// Applies another player's door/lock state. Returns false to try again later.
		bool TryApplyRefState(const Protocol::RefState& a_state)
		{
			if (!InWorld()) {
				return false;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_state.refId);
			if (ref && ref->As<RE::Actor>()) {
				return true;
			}
			const auto current = ReadRefState(ref);
			if (!current) {
				return ref && ref->IsDeleted();  // gone for good: drop it
			}
			if (a_state.locked != Protocol::RefState::kUnknown && current->locked != Protocol::RefState::kUnknown && a_state.locked != current->locked) {
				ref->GetLock()->SetLocked(a_state.locked == Protocol::RefState::kYes);
				ref->AddLockChange();
			}
			if (a_state.open != Protocol::RefState::kUnknown && current->open != Protocol::RefState::kUnknown && a_state.open != current->open) {
				RE::BGSOpenCloseForm::SetOpenState(ref, a_state.open == Protocol::RefState::kYes, false);
			}
			return true;
		}

		// Starts watching references the player activated, and reports changes to watched ones.
		void WatchRefStates(Clock::time_point a_now, const std::vector<Activation>& a_activations)
		{
			for (const auto& activation : a_activations) {
				auto& entry = watched[activation.ref];
				entry.until = a_now + WATCH_TIME;
			}
			for (auto it = watched.begin(); it != watched.end();) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(it->first);
				const auto state = ReadRefState(ref);
				if (!state || a_now > it->second.until) {
					it = watched.erase(it);
					continue;
				}
				if (*state != it->second.lastSent) {
					it->second.lastSent = *state;
					outgoing.push_back(Protocol::Encode(*state, Protocol::MessageType::kReportRefState));
				}
				++it;
			}
		}

		// Removes an item another player picked up. Returns false to try again later.
		bool TryApplyPickup(std::uint32_t a_refId)
		{
			if (!InWorld()) {
				return false;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_refId);
			if (!ref) {
				return false;
			}
			// Only loose items. Quest items stay so every player can still pick up their own copy.
			if (ref->As<RE::Actor>() || !IsInventoryForm(ref->GetObjectReference()) ||
				(ref->extraList && ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray))) {
				return true;
			}
			if (!ref->IsDisabled() && !ref->IsDeleted()) {
				ref->Disable();
			}
			return true;
		}

		constexpr std::uint32_t RESYNC_RECORD = 'CONT';
		constexpr std::uint32_t RESYNC_VERSION = 1;
	}

	bool InWorld()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return false;
		}
		const auto ui = RE::UI::GetSingleton();
		return !ui || !ui->GetMenuOpen("LoadingMenu"sv);
	}

	void Install()
	{
		if (const auto source = RE::TESDeathEvent::GetEventSource()) {
			source->RegisterSink(&deathWatcher);
			REX::INFO("WorldSync: watching deaths");
		} else {
			REX::ERROR("WorldSync: death event source unavailable");
		}
		if (const auto source = RE::TESHitEvent::GetEventSource()) {
			source->RegisterSink(&hitWatcher);
		}
		if (const auto source = RE::TESContainerChangedEvent::GetEventSource()) {
			source->RegisterSink(&containerWatcher);
		}
		if (const auto source = RE::TESActivateEvent::GetEventSource()) {
			source->RegisterSink(&activateWatcher);
		}
	}

	void OnWelcome(std::uint64_t a_sessionId, std::uint32_t a_localPlayerId, bool a_friendlyFire)
	{
		localPlayer = a_localPlayerId;
		friendlyFire = a_friendlyFire;
		if (a_sessionId != session) {
			// A different session: none of its changes are in our world yet.
			session = a_sessionId;
			containerNext = 0;
			pendingContainer.clear();
		}
	}

	Protocol::WorldRequest ResyncPoint()
	{
		return { session, containerNext };
	}

	void ApplyRemoteDeath(std::uint32_t a_refId)
	{
		if (!IsShareableRef(a_refId) || !dead.insert(a_refId).second) {
			return;
		}
		if (!TryKill(a_refId)) {
			pending.insert(a_refId);
		}
	}

	void ApplyWorldState(const Protocol::WorldState& a_state)
	{
		for (const auto id : a_state.deadActors) {
			ApplyRemoteDeath(id);
		}
		// Our own changes too: the server only sends those our world doesn't have.
		for (std::size_t i = 0; i < a_state.containerChanges.size(); ++i) {
			OfferContainerChange(a_state.containerFirstIndex + static_cast<std::uint32_t>(i), a_state.containerChanges[i], false);
		}
		for (const auto id : a_state.pickedUp) {
			ApplyRemotePickup(id);
		}
		for (const auto& state : a_state.refStates) {
			ApplyRemoteRefState(state);
		}
		for (const auto& stage : a_state.questStages) {
			QuestSync::Apply(stage);
		}
		REX::INFO("WorldSync: session state: {} dead, {} container changes from #{}, {} pickups, {} doors, {} quest stages",
			a_state.deadActors.size(), a_state.containerChanges.size(), a_state.containerFirstIndex, a_state.pickedUp.size(),
			a_state.refStates.size(), a_state.questStages.size());
	}

	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health)
	{
		if (!IsShareableRef(a_refId) || dead.contains(a_refId)) {
			return;
		}
		if (TrySetHealth(a_refId, a_health)) {
			pendingHealth.erase(a_refId);
		} else {
			pendingHealth[a_refId] = a_health;
		}
	}

	void ApplyContainerChange(const Protocol::IndexedContainerChange& a_change)
	{
		OfferContainerChange(a_change.index, a_change.change, a_change.playerId == localPlayer);
	}

	void ApplyRemotePickup(std::uint32_t a_refId)
	{
		if (IsShareableRef(a_refId) && !TryApplyPickup(a_refId)) {
			pendingPickups.insert(a_refId);
		}
	}

	void ApplyRemoteRefState(const Protocol::RefState& a_state)
	{
		if (!IsShareableRef(a_state.refId)) {
			return;
		}
		// So our own watcher doesn't report this change back.
		if (const auto it = watched.find(a_state.refId); it != watched.end()) {
			it->second.lastSent = a_state;
		}
		if (TryApplyRefState(a_state)) {
			pendingRefStates.erase(a_state.refId);
		} else {
			pendingRefStates[a_state.refId] = a_state;
		}
	}

	void Frame()
	{
		std::vector<std::uint32_t> newDeaths;
		std::unordered_set<std::uint32_t> killedByUs;
		std::vector<std::uint32_t> newHits;
		std::vector<std::uint32_t> newPickups;
		std::vector<StandInHit> newStandInHits;
		std::vector<Activation>    newActivations;
		{
			std::scoped_lock l{ inboxLock };
			newDeaths.swap(deathInbox);
			killedByUs.swap(killInbox);
			killInbox.clear();
			newHits.swap(hitInbox);
			newPickups.swap(pickupInbox);
			newStandInHits.swap(standInHits);
			newActivations.swap(activateInbox);
			for (auto& packet : lootInbox) {
				outgoing.push_back(std::move(packet));
			}
			lootInbox.clear();
		}

		// Deaths we caused because someone else reported them are already known and don't go back out.
		for (const auto id : newDeaths) {
			if (dead.insert(id).second) {
				outgoing.push_back(Protocol::Encode(Protocol::ActorDeath{ id, 0, killedByUs.contains(id) }, Protocol::MessageType::kReportDeath));
				++reported;
			}
			pending.erase(id);
			pendingHealth.erase(id);
		}

		// Report the health of actors we hit last frame (damage has been applied by now).
		if (const auto info = HealthInfo(); info && !hitLastFrame.empty()) {
			for (const auto id : hitLastFrame) {
				const auto actor = LoadedActor(id);
				if (actor && !actor->IsDead(false) && !dead.contains(id)) {
					const auto& values = static_cast<RE::ActorValueOwner&>(*actor);
					const float maximum = values.GetPermanentActorValue(*info);
					if (maximum > 0.0f) {
						const float fraction = values.GetActorValue(*info) / maximum;
						outgoing.push_back(Protocol::Encode(Protocol::ActorHealth{ id, fraction }, Protocol::MessageType::kReportHealth));
					}
				}
			}
		}
		hitLastFrame.clear();
		hitLastFrame.insert(newHits.begin(), newHits.end());
		// Fighting or talking to an NPC another player's game runs brings it over to ours.
		for (const auto id : newHits) {
			NpcSync::OnLocalInteraction(id);
		}
		for (const auto& activation : newActivations) {
			NpcSync::OnLocalInteraction(activation.ref);
			NpcSync::OnActivated(activation.ref);
		}
		for (const auto& hit : newStandInHits) {
			if (const auto playerId = RemotePlayers::PlayerIdFor(hit.actor)) {
				outgoing.push_back(Protocol::Encode(Protocol::PlayerHit{ playerId, hit.damage, hit.byPlayer }, Protocol::MessageType::kPlayerHit));
			}
		}

		const auto now = Clock::now();
		MatchPickups(now, newActivations, newPickups);
		WatchRefStates(now, newActivations);
		if ((pending.empty() && pendingHealth.empty() && pendingContainer.empty() && pendingPickups.empty() && pendingRefStates.empty()) || now < nextApply) {
			return;
		}
		nextApply = now + APPLY_INTERVAL;
		if (!InWorld()) {
			return;
		}

		for (auto it = pending.begin(); it != pending.end();) {
			it = TryKill(*it) ? pending.erase(it) : std::next(it);
		}
		for (auto it = pendingHealth.begin(); it != pendingHealth.end();) {
			it = (dead.contains(it->first) || TrySetHealth(it->first, it->second)) ? pendingHealth.erase(it) : std::next(it);
		}
		RetryPendingContainers();
		std::erase_if(pendingPickups, [](std::uint32_t a_id) { return TryApplyPickup(a_id); });
		std::erase_if(pendingRefStates, [](const auto& a_entry) { return TryApplyRefState(a_entry.second); });
	}

	std::uint32_t TakePlayerKills()
	{
		return playerKills.exchange(0);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Reset()
	{
		dead.clear();
		pending.clear();
		pendingHealth.clear();
		pendingPickups.clear();
		hitLastFrame.clear();
		recentActivations.clear();
		watched.clear();
		pendingRefStates.clear();
		unmatchedPickups.clear();
		std::scoped_lock l{ inboxLock };
		pickupInbox.clear();
		standInHits.clear();
		activateInbox.clear();
		deathInbox.clear();
		killInbox.clear();
		hitInbox.clear();
		lootInbox.clear();
		outgoing.clear();
	}

	void SaveResyncPoint(const F4SE::SerializationInterface* a_intfc)
	{
		if (session == 0) {
			return;
		}
		const auto count = static_cast<std::uint32_t>(pendingContainer.size());
		if (!a_intfc->OpenRecord(RESYNC_RECORD, RESYNC_VERSION) ||
			!a_intfc->WriteRecordData(session) || !a_intfc->WriteRecordData(containerNext) || !a_intfc->WriteRecordData(count)) {
			REX::ERROR("WorldSync: failed to save the session record");
			return;
		}
		for (const auto& entry : pendingContainer) {
			a_intfc->WriteRecordData(entry.index);
			a_intfc->WriteRecordData(entry.change.container);
			a_intfc->WriteRecordData(entry.change.item);
			a_intfc->WriteRecordData(entry.change.count);
		}
	}

	void LoadResyncPoint(const F4SE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		RevertResyncPoint();
		std::uint64_t savedSession = 0;
		std::uint32_t savedNext = 0;
		std::uint32_t count = 0;
		constexpr std::uint32_t HEADER = sizeof(savedSession) + sizeof(savedNext) + sizeof(count);
		constexpr std::uint32_t ENTRY = 4 * sizeof(std::uint32_t);
		if (a_version != RESYNC_VERSION || a_length < HEADER ||
			a_intfc->ReadRecordData(savedSession) != sizeof(savedSession) ||
			a_intfc->ReadRecordData(savedNext) != sizeof(savedNext) ||
			a_intfc->ReadRecordData(count) != sizeof(count) || count > (a_length - HEADER) / ENTRY) {
			REX::WARN("WorldSync: ignoring an unreadable session record");
			return;
		}
		std::vector<PendingChange> saved;
		for (std::uint32_t i = 0; i < count; ++i) {
			PendingChange entry{};
			a_intfc->ReadRecordData(entry.index);
			a_intfc->ReadRecordData(entry.change.container);
			a_intfc->ReadRecordData(entry.change.item);
			a_intfc->ReadRecordData(entry.change.count);
			saved.push_back(entry);
		}
		session = savedSession;
		containerNext = savedNext;
		pendingContainer = std::move(saved);
		REX::INFO("WorldSync: save has session {:016X} container changes up to #{} ({} waiting)", session, containerNext, pendingContainer.size());
	}

	void RevertResyncPoint()
	{
		session = 0;
		containerNext = 0;
		pendingContainer.clear();
	}

	std::string Describe()
	{
		return std::format("dead={} pending={} reported={} applied={} containerNext={} pendingLoot={} pendingPickups={} watched={} pendingDoors={}",
			dead.size(), pending.size(), reported, applied, containerNext, pendingContainer.size(), pendingPickups.size(), watched.size(), pendingRefStates.size());
	}
}
