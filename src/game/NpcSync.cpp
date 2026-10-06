#include "game/NpcSync.h"

#include "game/Puppets.h"
#include "game/WeaponFire.h"

namespace NpcSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto SCAN_INTERVAL = 250ms;
		constexpr auto STATE_INTERVAL = 100ms;  // 10 Hz per owned NPC
		constexpr auto CLAIM_RETRY = 3s;
		constexpr auto TAKEOVER_COOLDOWN = 5s;  // between takeover requests for the same NPC
		// States arrive at 10 Hz; render far enough in the past to have two to blend.
		constexpr auto INTERPOLATION_DELAY = 150ms;
		constexpr auto STALE_AFTER = 1s;
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
			RE::Actor*           registered = nullptr;  // as registered with Puppets
		};

		// An NPC we run.
		struct Owned
		{
			RE::NiPoint3      lastPosition;
			Clock::time_point lastTime{};
		};

		std::uint32_t localId = 0;

		std::unordered_map<std::uint32_t, std::uint32_t>     owners;     // NPC -> player (absent = nobody)
		std::unordered_map<std::uint32_t, Clock::time_point> claimedAt;  // claims awaiting an answer
		std::unordered_map<std::uint32_t, Clock::time_point> lastTakeover;
		std::unordered_map<std::uint32_t, Mirror>            mirrors;
		std::unordered_map<std::uint32_t, Owned>             owned;

		std::vector<Outgoing> outgoing;
		Clock::time_point     nextScan{};
		Clock::time_point     nextStates{};


		RE::Actor* LoadedActor(std::uint32_t a_id)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
			return actor && !actor->IsDeleted() && !actor->IsDisabled() && actor->Get3D() ? actor : nullptr;
		}

		// NPCs we can share: loaded, alive, from a plugin file, and not a player or a stand-in.
		bool IsCandidate(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsPlayerRef() && Protocol::IsShareableRef(a_actor->GetFormID()) && !Puppets::IsPuppet(a_actor) &&
			       !a_actor->IsDead(false) && !a_actor->IsDisabled() && a_actor->Get3D();
		}

		bool IsOurCompanion(RE::Actor* a_actor)
		{
			static const auto faction = RE::TESForm::GetFormByID<RE::TESFaction>(CURRENT_COMPANION_FACTION);
			return faction && a_actor->IsInFaction(faction);
		}

		// Where an actor is, as sent in states: (interior cell, 0) or (0, worldspace).
		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::Actor* a_actor)
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
			if (!a_mirror.registered) {
				return;
			}
			if (RE::TESForm::GetFormByID<RE::Actor>(a_id) == a_mirror.registered) {
				Puppets::ReleaseNpc(a_mirror.registered);
			} else {
				Puppets::Unregister(a_mirror.registered);  // the actor is gone; just forget it
			}
			a_mirror.registered = nullptr;
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

		void Scan(Clock::time_point a_now)
		{
			std::unordered_set<std::uint32_t> loaded;
			std::vector<Protocol::ActorClaim> claims;

			const auto lists = RE::ProcessLists::GetSingleton();
			if (lists) {
				for (const auto& handle : lists->highActorHandles) {
					const auto ptr = handle.get();
					const auto actor = ptr.get();
					if (!IsCandidate(actor)) {
						continue;
					}
					WeaponFire::InstallForNpcs(actor);  // no-op once hooked
					const auto id = actor->GetFormID();
					loaded.insert(id);

					const auto owner = OwnerOf(id);
					if (owner == localId) {
						continue;
					}
					const auto claimed = claimedAt.find(id);
					if (claimed != claimedAt.end() && a_now - claimed->second < CLAIM_RETRY) {
						continue;
					}
					// Companions follow their own player, so that player's game must run them.
					const bool companion = IsOurCompanion(actor);
					if (owner == 0 || companion) {
						claims.push_back({ id, companion ? Protocol::ClaimReason::kCompanion : Protocol::ClaimReason::kUnowned });
						claimedAt[id] = a_now;
					}
				}
			}
			std::erase_if(claimedAt, [&](const auto& a_entry) { return a_now - a_entry.second >= CLAIM_RETRY; });
			std::erase_if(lastTakeover, [&](const auto& a_entry) { return a_now - a_entry.second >= TAKEOVER_COOLDOWN; });

			// NPCs we run that are no longer loaded (or died) go back to the pool.
			std::vector<std::uint32_t> released;
			for (auto it = owned.begin(); it != owned.end();) {
				if (!loaded.contains(it->first)) {
					released.push_back(it->first);
					owners.erase(it->first);
					it = owned.erase(it);
				} else {
					++it;
				}
			}

			SendList(claims, [](const std::vector<Protocol::ActorClaim>& a) { return Protocol::Encode(a); }, true);
			SendList(released, [](const std::vector<std::uint32_t>& a) { return Protocol::EncodeRelease(a); }, true);
		}

		void SendOwnedStates(Clock::time_point a_now)
		{
			std::vector<Protocol::ActorState> states;
			for (auto& [id, track] : owned) {
				const auto actor = LoadedActor(id);
				if (!actor || actor->IsDead(false)) {
					continue;
				}
				const auto& pos = actor->data.location;
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
				state.speed = speed;
				state.moveMode = static_cast<std::uint16_t>(static_cast<const RE::ActorState&>(*actor).moveMode);
				if (actor->GetWeaponMagicDrawn()) {
					state.flags |= Protocol::kWeaponDrawn;
				}
				if (actor->IsSneaking()) {
					state.flags |= Protocol::kSneaking;
				}
				states.push_back(state);
			}
			SendList(states, [](const std::vector<Protocol::ActorState>& a) { return Protocol::Encode(a, Protocol::MessageType::kActorStates); }, false);
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

		std::optional<Puppets::Motion> Sample(const Mirror& a_mirror, Clock::time_point a_now)
		{
			if (a_mirror.snapshots.empty()) {
				return std::nullopt;
			}
			const auto renderTime = a_now - INTERPOLATION_DELAY;
			const auto& snaps = a_mirror.snapshots;

			Protocol::ActorState state = snaps.back().state;
			float                motionAngle = state.heading;
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
				const float dx = b.state.x - a.state.x;
				const float dy = b.state.y - a.state.y;
				if (dx * dx + dy * dy > 1.0f) {
					motionAngle = std::atan2(dx, dy);
				}
				break;
			}
			if (a_now - snaps.back().received > STALE_AFTER) {
				state.speed = 0.0f;
			}
			return Puppets::Motion{
				.position = { state.x, state.y, state.z },
				.heading = state.heading,
				.speed = state.speed,
				.direction = WrapAngle(motionAngle - state.heading) / (std::numbers::pi_v<float> * 2.0f),
				.moveMode = state.moveMode,
				.flags = state.flags,
			};
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
				// The owner's copy is somewhere else (e.g. a companion that followed its player through
				// a door): ours can't follow it there, so it runs its own AI meanwhile.
				if (mirror.snapshots.empty() || SpaceOf(actor) != std::pair{ mirror.snapshots.back().state.cell, mirror.snapshots.back().state.worldspace }) {
					StopMirroring(id, mirror);
					++it;
					continue;
				}
				if (mirror.registered != actor) {
					StopMirroring(id, mirror);
					Puppets::Register(actor, Puppets::Kind::kNpc);
					mirror.registered = actor;
				}
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

	void ApplyOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		for (const auto& entry : a_owners) {
			claimedAt.erase(entry.refId);
			if (entry.playerId == 0) {
				owners.erase(entry.refId);
				owned.erase(entry.refId);
				StopMirroring(entry.refId);
				continue;
			}
			owners[entry.refId] = entry.playerId;
			if (entry.playerId == localId) {
				owned.try_emplace(entry.refId);
				StopMirroring(entry.refId);
			} else {
				owned.erase(entry.refId);
			}
		}
	}

	void ApplyStates(const std::vector<Protocol::ActorState>& a_states)
	{
		const auto now = Clock::now();
		for (const auto& state : a_states) {
			const auto owner = OwnerOf(state.refId);
			if (owner == 0 || owner == localId || !LoadedActor(state.refId)) {
				continue;
			}
			auto& snaps = mirrors[state.refId].snapshots;
			snaps.push_back({ now, state });
			while (snaps.size() > MAX_SNAPSHOTS) {
				snaps.pop_front();
			}
		}
	}

	bool RunsLocally(std::uint32_t a_refId)
	{
		return owned.contains(a_refId);
	}

	void ApplyShot(std::uint32_t a_refId)
	{
		const auto it = mirrors.find(a_refId);
		const auto actor = it != mirrors.end() ? LoadedActor(a_refId) : nullptr;
		if (actor && it->second.registered == actor) {
			WeaponFire::PlayShot(actor);
		}
	}

	void OnLocalInteraction(std::uint32_t a_refId)
	{
		const auto owner = OwnerOf(a_refId);
		if (localId == 0 || owner == 0 || owner == localId) {
			return;
		}
		const auto now = Clock::now();
		const auto last = lastTakeover.find(a_refId);
		if (last != lastTakeover.end() && now - last->second < TAKEOVER_COOLDOWN) {
			return;
		}
		lastTakeover[a_refId] = now;
		claimedAt[a_refId] = now;
		outgoing.push_back({ Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_refId, Protocol::ClaimReason::kInteract } }), true });
	}

	void Frame()
	{
		if (localId == 0) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return;
		}
		const auto now = Clock::now();
		if (now >= nextScan) {
			nextScan = now + SCAN_INTERVAL;
			Scan(now);
		}
		if (now >= nextStates) {
			nextStates = now + STATE_INTERVAL;
			SendOwnedStates(now);
		}
		UpdateMirrors(now);
	}

	std::vector<Outgoing> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void ReleaseMirrors()
	{
		for (auto& [id, mirror] : mirrors) {
			StopMirroring(id, mirror);
		}
		mirrors.clear();
	}

	void Reset()
	{
		ReleaseMirrors();
		localId = 0;
		owners.clear();
		claimedAt.clear();
		lastTakeover.clear();
		owned.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("npcs: owned={} mirrored={} known={}", owned.size(), mirrors.size(), owners.size());
	}
}
