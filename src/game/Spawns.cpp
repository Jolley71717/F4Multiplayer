#include "game/Spawns.h"

#include "game/EngineRefs.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/SpawnBook.h"
#include "game/Story.h"
#include "game/WorldSync.h"

namespace Spawns
{
	namespace
	{
		using Clock = SpawnBook::Clock;

		// Copies placed in one frame at most: a fight's worth arriving at once is placed over a few frames.
		constexpr int           PLACE_PER_FRAME = 2;
		constexpr std::size_t   MAX_KILLED_HERE = 256;
		constexpr std::uint32_t LOGGED = 20;  // lines about single spawns (the counters go on)

		SpawnBook::Ours   ours;
		SpawnBook::Theirs theirs;

		// Our copies of friends' spawns, by spawn id: the handle (its id alone may name something else once it is gone).
		struct Placed
		{
			RE::ObjectRefHandle handle;
			Clock::time_point   placedAt{};
		};
		std::unordered_map<std::uint32_t, Placed> placed;

		// Our NPCs killed for another game's report (a friend's kill of ours, or our copy's death in its owner's game):
		// their deaths here are not reported back.
		std::unordered_set<std::uint32_t> killedHere;

		// Our copies on their way out (in the stand-ins' graveyard until it is safe to delete them): a runtime NPC our
		// game has, hostile to our player, but never ours to share. Forgotten once the handle no longer finds it (its id
		// may then name a new NPC).
		constexpr std::size_t                                   MAX_LEAVING = 512;
		std::unordered_map<std::uint32_t, RE::ObjectRefHandle> leaving;

		bool Leaving(std::uint32_t a_local)
		{
			const auto it = leaving.find(a_local);
			if (it == leaving.end()) {
				return false;
			}
			if (const auto ref = it->second.get(); ref && ref->GetFormID() == a_local) {
				return true;
			}
			leaving.erase(it);
			return false;
		}

		std::vector<std::vector<std::uint8_t>> outgoing;

		std::uint32_t reported = 0, granted = 0, refused = 0, received = 0;
		std::uint32_t copiesPlaced = 0, placeFailed = 0, copiesDiscarded = 0, copiesLost = 0, copiesKilled = 0, oursKilled = 0;
		std::uint32_t logged = 0;

		std::uint32_t LocalPlayer()
		{
			return NpcSync::LocalPlayer();
		}

		RemoteEdits::Spot SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto  cell = a_ref->GetParentCell();
			const auto& at = a_ref->data.location;
			if (cell && cell->IsInterior()) {
				return { cell->GetFormID(), 0, at.x, at.y, at.z };
			}
			return { 0, cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0, at.x, at.y, at.z };
		}

		SpawnBook::Base BaseOf(RE::TESForm* a_form)
		{
			const auto npc = a_form ? a_form->As<RE::TESNPC>() : nullptr;
			if (!npc) {
				return { a_form ? a_form->GetFormID() : 0, false, false };
			}
			return { npc->GetFormID(), true, npc->UsesTemplate() };
		}

		// What every game places a copy of this NPC from (SpawnBook::PickBase), 0 if nothing.
		std::uint32_t BaseToPlace(RE::Actor* a_actor)
		{
			std::array<SpawnBook::Base, 13> templates{};
			std::size_t                     count = 0;
			SpawnBook::Base                 original{};
			if (a_actor->extraList) {
				if (const auto leveled = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>()) {
					original = BaseOf(leveled->originalBase);
					for (const auto base : leveled->templates) {
						if (base) {
							templates[count++] = BaseOf(base);
						}
					}
				}
			}
			return SpawnBook::PickBase(BaseOf(a_actor->GetNPC()), std::span{ templates.data(), count }, original);
		}

		// Ours to share: an NPC our game made that is fighting, or hostile to our player (a random encounter's raiders,
		// an ambush), and alive. Never our companion (nor a stand-in, a copy of ours or the player: Share).
		bool OursToShare(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			return !a_actor->IsDead(false) && !NpcSync::IsOurCompanion(a_actor) && (a_actor->IsInCombat() || a_actor->GetHostileToActor(a_player));
		}

		void Kill(std::uint32_t a_local)
		{
			if (killedHere.size() >= MAX_KILLED_HERE) {
				killedHere.clear();
			}
			killedHere.insert(a_local);
			RE::Console::ExecuteCommand(std::format("{:08X}.kill", a_local).c_str());
			NpcSync::CountCommands(1);
		}

		// Our copy of a friend's spawn, in our player's cell or worldspace (Theirs::Decide has them in the same space).
		RE::ObjectRefHandle PlaceCopy(const SpawnBook::Theirs::Copy& a_copy, RE::TESObjectCELL* a_cell)
		{
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_copy.spawn.base);
			if (!npc) {
				return {};
			}
			RE::NEW_REFR_DATA data;
			data.location = { a_copy.at.x, a_copy.at.y, a_copy.at.z };
			data.direction = { 0.0f, 0.0f, a_copy.heading };
			data.object = npc;
			data.interior = a_cell->IsInterior() ? a_cell : nullptr;
			data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
			data.clearStillLoadingFlag = true;
			const auto handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref || !ref->As<RE::Actor>()) {
				return {};
			}
			SaveGuard::Track(ref.get());
			return handle;
		}

		// Deletes our copy of a spawn (the record stays, unplaced, unless the spawn is over).
		void Discard(std::uint32_t a_id)
		{
			NpcSync::DropMirror(a_id);
			if (const auto it = placed.find(a_id); it != placed.end()) {
				if (const auto local = theirs.LocalOf(a_id)) {
					if (leaving.size() >= MAX_LEAVING) {
						std::erase_if(leaving, [](const auto& a_entry) { return !a_entry.second.get(); });
					}
					if (leaving.size() < MAX_LEAVING) {
						leaving[local] = it->second.handle;
					}
				}
				RemotePlayers::Discard(it->second.handle, it->second.placedAt);
				placed.erase(it);
				++copiesDiscarded;
			}
			theirs.Unplaced(a_id);
		}

		// A base we place copies from: an NPC, and not a unique one (a second of a named character) or a stand-in's.
		bool BaseOk(std::uint32_t a_base)
		{
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
			return npc && !npc->IsUnique() && !RemotePlayers::IsStandInBase(a_base);
		}
	}

	std::uint32_t Share(RE::Actor* a_actor)
	{
		if (!a_actor || a_actor->IsPlayerRef() || Puppets::IsRegistered(a_actor) || Puppets::IsPuppet(a_actor) || a_actor->IsDeleted() || a_actor->IsDisabled() ||
			!a_actor->Get3D() || !a_actor->GetParentCell()) {
			return 0;
		}
		const auto local = a_actor->GetFormID();
		if (!Protocol::IsSpawnRef(local) || theirs.IdOf(local) != 0 || LocalPlayer() == 0 || Leaving(local)) {
			return 0;
		}
		if (const auto id = ours.IdOf(local)) {
			return id;  // dead or alive: ours until the server has its death
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !OursToShare(a_actor, player)) {
			return 0;
		}
		const auto base = BaseToPlace(a_actor);
		const auto at = SpotOf(a_actor);
		if (base == 0 || (at.cell == 0) == (at.worldspace == 0)) {
			return 0;
		}
		const auto id = ours.Offer(local, Clock::now(), [](std::uint32_t a_id) { return theirs.Knows(a_id); });
		if (!id) {
			return 0;
		}
		outgoing.push_back(Protocol::Encode(Protocol::Spawn{ 0, *id, base, at.cell, at.worldspace, at.x, at.y, at.z, a_actor->data.angle.z }, Protocol::MessageType::kReportSpawn));
		++reported;
		if (logged < LOGGED) {
			++logged;
			REX::INFO("Spawns: sharing {:08X} (base {:08X}) as {:08X}", local, base, *id);
		}
		return 0;  // not until the server grants it
	}

	std::uint32_t LocalOf(std::uint32_t a_id)
	{
		if (const auto local = ours.LocalOf(a_id)) {
			return local;
		}
		return theirs.LocalOf(a_id);
	}

	std::uint32_t IdOf(std::uint32_t a_local)
	{
		if (const auto id = ours.IdOf(a_local)) {
			return id;
		}
		return theirs.IdOf(a_local);
	}

	std::uint32_t OwnIdOf(std::uint32_t a_local)
	{
		return ours.IdOf(a_local);
	}

	std::uint32_t CopyIdOf(std::uint32_t a_local)
	{
		return theirs.IdOf(a_local);
	}

	void Add(const Protocol::Spawn& a_spawn)
	{
		if (const auto known = theirs.Find(a_spawn.refId); known && known->local != 0) {
			Discard(a_spawn.refId);  // an old one under the same id: it is over
		}
		if (theirs.Add(a_spawn, LocalPlayer())) {
			++received;
			if (logged < LOGGED) {
				++logged;
				REX::INFO("Spawns: player {} shares {:08X} (base {:08X}) at ({:.0f},{:.0f},{:.0f}) cell {:08X} ws {:08X}", a_spawn.playerId, a_spawn.refId, a_spawn.base, a_spawn.x,
					a_spawn.y, a_spawn.z, a_spawn.cell, a_spawn.worldspace);
			}
		}
	}

	void OnOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		const auto now = Clock::now();
		for (const auto& owner : a_owners) {
			if (!Protocol::IsSpawnRef(owner.refId)) {
				continue;
			}
			if (ours.Holds(owner.refId)) {
				const bool wasConfirmed = ours.LocalOf(owner.refId) != 0;
				ours.Answered(owner.refId, owner.playerId, LocalPlayer(), now);
				if (!wasConfirmed) {
					owner.playerId == LocalPlayer() ? ++granted : ++refused;
				}
				continue;
			}
			if (owner.playerId == 0) {
				theirs.Gone(owner.refId);
			}
		}
	}

	void OnState(const Protocol::ActorState& a_state)
	{
		theirs.Moved(a_state.refId, RemoteEdits::SpotOf(a_state), a_state.heading);
	}

	void ApplyDeath(const Protocol::ActorDeath& a_death)
	{
		if (!Protocol::IsSpawnRef(a_death.refId)) {
			return;
		}
		std::uint32_t local = 0;
		if (theirs.Knows(a_death.refId)) {
			local = theirs.Died(a_death.refId);
			copiesKilled += local != 0;
		} else if ((local = ours.LocalOf(a_death.refId)) != 0) {
			++oursKilled;  // a friend killed their copy of ours: it dies here too
		}
		const auto actor = local ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
		if (!actor || actor->IsDead(false) || actor->IsDeleted() || !WorldSync::InWorld()) {
			return;
		}
		Kill(local);
		REX::INFO("Spawns: killed {:08X}: spawn {:08X} died in player {}'s game", local, a_death.refId, a_death.playerId);
	}

	std::uint32_t OnLocalDeath(std::uint32_t a_local)
	{
		if (killedHere.erase(a_local)) {
			return 0;
		}
		if (const auto id = ours.IdOf(a_local)) {
			return id;
		}
		const auto id = theirs.IdOf(a_local);
		return id != 0 && theirs.Died(id) != 0 ? id : 0;  // our copy, killed here first: the kill is ours to report
	}

	void Frame()
	{
		if (theirs.Size() == 0) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell || !WorldSync::InWorld() || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		const auto us = SpotOf(player);
		int        placedNow = 0;
		for (const auto id : theirs.Ids()) {
			const auto copy = theirs.Find(id);
			if (!copy) {
				continue;
			}
			SpawnBook::Facts facts{ .player = us };
			if (const auto state = RemotePlayers::LastStateOf(copy->spawn.playerId)) {
				facts.owner = RemoteEdits::SpotOf(*state);
			}
			if (copy->local != 0) {
				const auto it = placed.find(id);
				const auto ref = it != placed.end() ? it->second.handle.get() : RE::NiPointer<RE::TESObjectREFR>{};
				facts.copyThere = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
				if (facts.copyThere && ref->Get3D() && ref->GetParentCell()) {
					facts.copyAt = SpotOf(ref.get());
				}
			} else {
				facts.baseOk = BaseOk(copy->spawn.base);
			}
			switch (theirs.Decide(id, facts, now)) {
			case SpawnBook::Act::kPlace:
				{
					if (placedNow >= PLACE_PER_FRAME) {
						break;
					}
					++placedNow;
					const auto playerId = copy->spawn.playerId;
					const auto base = copy->spawn.base;
					const auto handle = PlaceCopy(*copy, cell);
					const auto ref = handle.get();
					if (!ref) {
						theirs.Placed(id, 0, now);
						++placeFailed;
						REX::WARN("Spawns: could not place a copy of player {}'s {:08X} (base {:08X})", playerId, id, base);
						break;
					}
					placed[id] = { handle, now };
					theirs.Placed(id, ref->GetFormID(), now);
					++copiesPlaced;
					if (logged < LOGGED) {
						++logged;
						REX::INFO("Spawns: placed {:08X} as our copy of player {}'s {:08X}", ref->GetFormID(), playerId, id);
					}
					break;
				}
			case SpawnBook::Act::kDiscard:
				Discard(id);
				break;
			case SpawnBook::Act::kLost:
				NpcSync::DropMirror(id);
				placed.erase(id);
				theirs.Unplaced(id);
				++copiesLost;
				break;
			case SpawnBook::Act::kForget:
				theirs.Forget(id);
				break;
			case SpawnBook::Act::kNone:
				break;
			}
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void DiscardCopies()
	{
		for (const auto id : theirs.Ids()) {
			if (const auto copy = theirs.Find(id); copy && copy->local != 0) {
				Discard(id);
			}
		}
	}

	void OnGameLoaded()
	{
		// Our copies went before the load (DiscardCopies); a handle left would name something of the loaded game.
		for (const auto id : theirs.Ids()) {
			theirs.Unplaced(id);
		}
		placed.clear();
		killedHere.clear();
		leaving.clear();
		// Our NPCs' ids name the loaded game's references now: the ones we shared are let go (the others' copies go,
		// before anything the loaded game makes is offered under the same id), and what it makes is shared afresh.
		const auto released = ours.Ids();
		ours.Clear();
		if (LocalPlayer() == 0) {
			return;
		}
		for (std::size_t i = 0; i < released.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
			const auto end = (std::min)(released.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
			outgoing.push_back(Protocol::EncodeRelease(std::vector<std::uint32_t>(released.begin() + i, released.begin() + end)));
		}
	}

	void Reset()
	{
		DiscardCopies();
		placed.clear();
		theirs.Clear();
		ours.Clear();
		killedHere.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("spawns: ours={} theirs={} copies={} reported={} granted={} refused={} received={} placed={} placeFailed={} discarded={} lost={} killed={}/{}", ours.Size(),
			theirs.Size(), theirs.PlacedCount(), reported, granted, refused, received, copiesPlaced, placeFailed, copiesDiscarded, copiesLost, copiesKilled, oursKilled);
	}
}
