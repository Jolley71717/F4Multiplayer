#include "game/WorldSync.h"

#include "game/Puppets.h"

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
				// Only damage we dealt; each player reports their own hits.
				if (!target || !cause || !cause->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
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
				if (a_event.newContainerFormID == PLAYER_REF && a_event.oldContainerFormID == 0 && IsShareable(a_event.referenceFormID)) {
					packet = Protocol::Encode(Protocol::RefPickedUp{ a_event.referenceFormID }, Protocol::MessageType::kReportPickup);
				} else if (a_event.newContainerFormID == PLAYER_REF && IsShareable(a_event.oldContainerFormID) && count > 0) {
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

	void Frame()
	{
		std::vector<std::uint32_t> newDeaths;
		std::vector<std::uint32_t> newHits;
		{
			std::scoped_lock l{ inboxLock };
			newDeaths.swap(deathInbox);
			newHits.swap(hitInbox);
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

		const auto now = Clock::now();
		if ((pending.empty() && pendingHealth.empty() && pendingContainer.empty() && pendingPickups.empty()) || now < nextApply) {
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
		std::scoped_lock l{ inboxLock };
		deathInbox.clear();
		hitInbox.clear();
		lootInbox.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("dead={} pending={} reported={} applied={} pendingLoot={} pendingPickups={}", dead.size(), pending.size(), reported, applied, pendingContainer.size(), pendingPickups.size());
	}
}
