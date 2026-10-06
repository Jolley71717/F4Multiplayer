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

				// Deaths we caused because someone else reported them don't go back out.
				if (dead.insert(id).second) {
					outgoing.push_back(Protocol::Encode(Protocol::ActorDeath{ id }, Protocol::MessageType::kReportDeath));
					++reported;
				}
				pending.erase(id);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		DeathWatcher deathWatcher;
	}

	void Install()
	{
		if (const auto source = RE::TESDeathEvent::GetEventSource()) {
			source->RegisterSink(&deathWatcher);
			REX::INFO("WorldSync: watching deaths");
		} else {
			REX::ERROR("WorldSync: death event source unavailable");
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
		REX::INFO("WorldSync: session state has {} dead actors", a_state.deadActors.size());
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (pending.empty() || now < nextApply) {
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
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Reset()
	{
		dead.clear();
		pending.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("dead={} pending={} reported={} applied={}", dead.size(), pending.size(), reported, applied);
	}
}
