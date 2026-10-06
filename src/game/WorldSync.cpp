#include "game/WorldSync.h"

#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"

namespace WorldSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto APPLY_INTERVAL = 250ms;

		// Every actor known to be dead this session, and those we still have to kill locally
		// (they weren't loaded when we heard about them).
		std::unordered_set<std::uint32_t> dead;
		std::unordered_set<std::uint32_t> pending;

		// Game events can arrive on any thread, so sinks only queue IDs; Frame() does the work.
		std::mutex                 inboxLock;
		std::vector<std::uint32_t> deathInbox;
		std::vector<std::uint32_t> hitInbox;
		std::vector<std::vector<std::uint8_t>> lootInbox;  // already-encoded reports
		std::vector<std::uint32_t>             pickupInbox;  // base forms picked up from the world
		std::vector<std::pair<std::uint32_t, float>> standInHits;  // (stand-in actor, damage)

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

		// Remote loot changes for containers/items that weren't loaded yet, in arrival order.
		std::vector<Protocol::ContainerChange> pendingContainer;
		std::unordered_set<std::uint32_t>      pendingPickups;

		// Actors we hit last frame; their resulting health is reported this frame, once the
		// damage has been applied.
		std::unordered_set<std::uint32_t> hitLastFrame;
		// Health values from other players for actors that weren't loaded yet.
		std::unordered_map<std::uint32_t, float> pendingHealth;

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextApply{};
		std::uint32_t                          applied = 0;
		std::uint32_t                          reported = 0;

		bool IsShareable(std::uint32_t a_refId)
		{
			return a_refId != 0 && (a_refId >> 24) != 0xFF;
		}

		// Kills an actor that died in another player's world. Returns true once it is dead here
		// (or can never be), false if it isn't loaded yet.
		bool TryKill(std::uint32_t a_refId)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
			if (!actor || actor->IsDeleted()) {
				return false;  // not in memory yet; try again when its cell loads
			}
			if (actor->IsDead(false)) {
				return true;
			}
			if (!actor->Get3D() || !actor->GetParentCell()) {
				return false;
			}

			// The console kill goes through the engine's normal death path (death animation,
			// ragdoll, quest/script death events), exactly as if it happened here.
			const auto command = std::format("{:08X}.kill", a_refId);
			RE::Console::ExecuteCommand(command.c_str());
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
				if (!IsShareable(id) || ref->IsPlayerRef() || (actor && Puppets::IsPuppet(actor))) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				deathInbox.push_back(id);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		DeathWatcher deathWatcher;

		const RE::ActorValueInfo* HealthInfo()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->health : nullptr;
		}

		// Sets a loaded actor's current health to a fraction of its maximum. Returns false if it isn't loaded.
		bool TrySetHealth(std::uint32_t a_refId, float a_fraction)
		{
			const auto info = HealthInfo();
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
			if (!info || !actor || actor->IsDeleted() || !actor->Get3D()) {
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
				// An NPC in our world hit another player's stand-in: that player takes the damage.
				if (!cause->IsPlayerRef()) {
					const auto victim = target->As<RE::Actor>();
					if (victim && Puppets::IsPuppet(victim) && a_event.usesHitData) {
						const float damage = a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage;
						if (damage > 0.0f) {
							std::scoped_lock l{ inboxLock };
							standInHits.push_back({ victim->GetFormID(), damage });
						}
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				// Only damage we dealt; each player reports their own hits.
				const auto actor = target->As<RE::Actor>();
				if (actor && IsShareable(target->GetFormID()) && !Puppets::IsPuppet(actor)) {
					std::scoped_lock l{ inboxLock };
					hitInbox.push_back(target->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitWatcher hitWatcher;

		constexpr std::uint32_t PLAYER_REF = 0x14;

		class ContainerWatcher :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Only moves the local player makes. Changes we apply for other players go between a
				// container and nothing, so they never match and never echo back.
				std::optional<std::vector<std::uint8_t>> packet;
				const auto count = a_event.itemCount;
				if (a_event.newContainerFormID == PLAYER_REF && a_event.oldContainerFormID == 0) {
					// Picked up from the world. The event doesn't say which reference (referenceFormID is 0),
					// so Frame() pairs it with the item the player just activated.
					std::scoped_lock l{ inboxLock };
					pickupInbox.push_back(a_event.baseObjectFormID);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event.newContainerFormID == PLAYER_REF && IsShareable(a_event.oldContainerFormID) && count > 0) {
					packet = Protocol::Encode(Protocol::ContainerChange{ a_event.oldContainerFormID, a_event.baseObjectFormID, -count }, Protocol::MessageType::kReportContainer);
				} else if (a_event.oldContainerFormID == PLAYER_REF && IsShareable(a_event.newContainerFormID) && count > 0) {
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
				if (!target || !by || !by->IsPlayerRef() || !IsShareable(target->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto base = target->GetObjectReference();
				std::scoped_lock l{ inboxLock };
				activateInbox.push_back({ target->GetFormID(), base ? base->GetFormID() : 0 });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ActivateWatcher activateWatcher;

		// Applies another player's container change. Returns false if the container isn't loaded.
		bool TryApplyContainer(const Protocol::ContainerChange& a_change)
		{
			const auto container = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_change.container);
			if (!container || container->IsDeleted()) {
				return false;
			}
			if (!RE::TESForm::GetFormByID(a_change.item)) {
				return true;  // unknown item; nothing sensible to do
			}
			const auto command = a_change.count < 0 ?
				std::format("{:08X}.removeitem {:08X} {}", a_change.container, a_change.item, -a_change.count) :
				std::format("{:08X}.additem {:08X} {}", a_change.container, a_change.item, a_change.count);
			RE::Console::ExecuteCommand(command.c_str());
			return true;
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

		// Applies another player's door/lock state. Returns false if the reference isn't loaded.
		bool TryApplyRefState(const Protocol::RefState& a_state)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_state.refId);
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

		bool TryApplyPickup(std::uint32_t a_refId)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_refId);
			if (!ref) {
				return false;
			}
			if (!ref->IsDisabled() && !ref->IsDeleted()) {
				ref->Disable();
			}
			return true;
		}
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

	void ApplyRemoteDeath(std::uint32_t a_refId)
	{
		if (!IsShareable(a_refId) || !dead.insert(a_refId).second) {
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
		for (const auto& change : a_state.containerChanges) {
			ApplyRemoteContainerChange(change);
		}
		for (const auto id : a_state.pickedUp) {
			ApplyRemotePickup(id);
		}
		for (const auto& state : a_state.refStates) {
			ApplyRemoteRefState(state);
		}
		REX::INFO("WorldSync: session state has {} dead actors", a_state.deadActors.size());
	}

	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health)
	{
		if (!IsShareable(a_refId) || dead.contains(a_refId)) {
			return;
		}
		if (TrySetHealth(a_refId, a_health)) {
			pendingHealth.erase(a_refId);
		} else {
			pendingHealth[a_refId] = a_health;
		}
	}

	void ApplyRemoteContainerChange(const Protocol::ContainerChange& a_change)
	{
		// Keep order: if earlier changes for anything are still waiting, queue behind them.
		if (!pendingContainer.empty() || !TryApplyContainer(a_change)) {
			pendingContainer.push_back(a_change);
		}
	}

	void ApplyRemotePickup(std::uint32_t a_refId)
	{
		if (IsShareable(a_refId) && !TryApplyPickup(a_refId)) {
			pendingPickups.insert(a_refId);
		}
	}

	void ApplyRemoteRefState(const Protocol::RefState& a_state)
	{
		if (!IsShareable(a_state.refId)) {
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
		std::vector<std::uint32_t> newHits;
		std::vector<std::uint32_t> newPickups;
		std::vector<std::pair<std::uint32_t, float>> newStandInHits;
		std::vector<Activation>    newActivations;
		{
			std::scoped_lock l{ inboxLock };
			newDeaths.swap(deathInbox);
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
				outgoing.push_back(Protocol::Encode(Protocol::ActorDeath{ id }, Protocol::MessageType::kReportDeath));
				++reported;
			}
			pending.erase(id);
			pendingHealth.erase(id);
		}

		// Report the health of actors we hit last frame (damage has been applied by now).
		if (const auto info = HealthInfo(); info && !hitLastFrame.empty()) {
			for (const auto id : hitLastFrame) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
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
		for (const auto id : newHits) {
			NpcSync::OnLocalHit(id);
		}
		for (const auto& [actorId, damage] : newStandInHits) {
			if (const auto playerId = RemotePlayers::PlayerIdFor(actorId)) {
				outgoing.push_back(Protocol::Encode(Protocol::PlayerHit{ playerId, damage }, Protocol::MessageType::kPlayerHit));
			}
		}

		const auto now = Clock::now();
		MatchPickups(now, newActivations, newPickups);
		WatchRefStates(now, newActivations);
		if ((pending.empty() && pendingHealth.empty() && pendingContainer.empty() && pendingPickups.empty() && pendingRefStates.empty()) || now < nextApply) {
			return;
		}
		nextApply = now + APPLY_INTERVAL;

		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return;
		}

		for (auto it = pending.begin(); it != pending.end();) {
			it = TryKill(*it) ? pending.erase(it) : std::next(it);
		}
		for (auto it = pendingHealth.begin(); it != pendingHealth.end();) {
			it = (dead.contains(it->first) || TrySetHealth(it->first, it->second)) ? pendingHealth.erase(it) : std::next(it);
		}
		std::erase_if(pendingContainer, [](const Protocol::ContainerChange& a_change) { return TryApplyContainer(a_change); });
		std::erase_if(pendingPickups, [](std::uint32_t a_id) { return TryApplyPickup(a_id); });
		std::erase_if(pendingRefStates, [](const auto& a_entry) { return TryApplyRefState(a_entry.second); });
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
		pendingContainer.clear();
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
		hitInbox.clear();
		lootInbox.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("dead={} pending={} reported={} applied={} pendingLoot={} pendingPickups={} watched={} pendingDoors={}", dead.size(), pending.size(), reported, applied, pendingContainer.size(), pendingPickups.size(), watched.size(), pendingRefStates.size());
	}
}
