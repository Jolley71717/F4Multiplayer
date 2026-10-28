#include "game/Spawns.h"

#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/FrameLoans.h"
#include "game/FrameShare.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RefTag.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/Rolls.h"
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
		// The id and base it was placed under go with it (see HeldCopy): the engine unloads a copy with its cell, its handle
		// stops resolving and the cell keeps its record, which brought it back beside the next copy placed (Refresh).
		struct Placed
		{
			RE::ObjectRefHandle handle;
			Clock::time_point   placedAt{};
			HeldCopy::Ids       ids;
		};
		std::unordered_map<std::uint32_t, Placed> placed;

		// When each friend's spawn was first seen here: its copy waits a moment for their roll of it (NpcRoll::IDENTITY_WAIT).
		std::unordered_map<std::uint32_t, Clock::time_point> seenAt;

		// What our copy of a friend's spawn is placed from: the NPC their game placed it as, which ours rolls and their
		// roll then makes theirs, when their roll of it came and that is a leveled NPC here; else the spawn's base.
		std::uint32_t PlaceBase(std::uint32_t a_id, std::uint32_t a_base)
		{
			const auto roll = NpcSync::RollOf(a_id);
			return NpcRoll::PlaceFrom(roll, a_base, roll && Rolls::IsLeveledNpc(roll->originalBase));
		}

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

		// Power armor frames our game made that it reports or shares (answered or not), by their ids here: no more than
		// FrameShare::MAX_MADE at once.
		std::unordered_set<std::uint32_t> ownFrames;

		std::uint32_t reported = 0, granted = 0, refused = 0, received = 0;
		std::uint32_t copiesPlaced = 0, placeFailed = 0, copiesDiscarded = 0, copiesLost = 0, copiesKilled = 0, oursKilled = 0;
		std::uint32_t framesReported = 0, frameCopiesPlaced = 0, frameCopiesMoved = 0, frameCopiesTaken = 0;
		std::uint32_t copiesUnloaded = 0, copiesBack = 0;  // copies the engine unloaded with their cell: forgotten, or held again (Refresh)
		// Friends' frames a frame of our own stands for (SpawnBook::FrameFacts::oursThere), by spawn id while it does: no copy.
		std::unordered_set<std::uint32_t> oursThere;
		std::uint32_t                     frameCopiesWithheld = 0;
		std::uint32_t logged = 0;

		std::uint32_t LocalPlayer()
		{
			return NpcSync::LocalPlayer();
		}

		// Our copy of spawn a_id, kept true once its handle stops resolving (see HeldCopy): the same copy back under its id
		// with its cell is held again; one gone is forgotten (SaveGuard drops the record its cell keeps, so the cell never
		// brings it back beside a new copy), and the book then finds no copy there (kLost).
		void Refresh(std::uint32_t a_id, Placed& a_placed)
		{
			const auto local = a_placed.ids.id;
			switch (SaveGuard::Refresh(a_placed.handle, a_placed.ids)) {
			case HeldCopy::Lost::kNone:
				break;
			case HeldCopy::Lost::kBack:
				++copiesBack;
				REX::INFO("Spawns: {:08X}, our copy of spawn {:08X}, came back with its cell; held again, no second one placed", local, a_id);
				break;
			case HeldCopy::Lost::kForget:
				++copiesUnloaded;
				REX::INFO("Spawns: {:08X}, our copy of spawn {:08X}, is gone from under us (unloaded with its cell); forgotten, its record dropped", local, a_id);
				break;
			}
		}

		// Our copy of spawn a_id, if one is placed and still there (see Refresh).
		RE::NiPointer<RE::TESObjectREFR> CopyOf(std::uint32_t a_id)
		{
			const auto it = placed.find(a_id);
			if (it == placed.end()) {
				return {};
			}
			Refresh(a_id, it->second);
			return it->second.handle.get();
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

		// Our runtime NPCs that are hostile or fighting but cannot be reported (SpawnBook::ShareBlock), by their ids here:
		// each is said once, for a few lines in all, and counted (`net status`: spawns notShared=). A friend does not see
		// these, which is how "the host could not see the Mr Gutsy" would show.
		constexpr std::size_t           MAX_BLOCKED_NOTED = 256;
		std::unordered_set<std::uint32_t> blockedNoted;
		std::uint32_t                   blocked = 0, blockedLogged = 0;

		// What the actor's base is put together from, for the log: its own base, the NPC it was placed as, and its templates.
		std::string DescribeBase(RE::Actor* a_actor)
		{
			const auto npc = a_actor->GetNPC();
			std::string out = std::format("own {:08X}{}", npc ? npc->GetFormID() : 0, npc && npc->UsesTemplate() ? " (uses a template)" : "");
			if (a_actor->extraList) {
				if (const auto leveled = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>()) {
					out += std::format(", placed as {:08X}", leveled->originalBase ? leveled->originalBase->GetFormID() : 0);
					for (const auto base : leveled->templates) {
						if (base) {
							const auto templated = base->As<RE::TESNPC>();
							out += std::format(", template {:08X}{}", base->GetFormID(), templated ? (templated->UsesTemplate() ? " (has one of its own)" : "") : " (not an NPC)");
						}
					}
				} else {
					out += ", no leveled data";
				}
			}
			return out;
		}

		void NoteBlocked(RE::Actor* a_actor, std::uint32_t a_local, SpawnBook::Block a_block)
		{
			if (blockedNoted.size() >= MAX_BLOCKED_NOTED) {
				blockedNoted.clear();
			}
			if (!blockedNoted.insert(a_local).second) {
				return;
			}
			++blocked;
			if (blockedLogged >= LOGGED) {
				return;
			}
			++blockedLogged;
			const auto name = a_actor->GetDisplayFullName();
			REX::INFO("Spawns: {:08X} '{}' (race {:08X}) is hostile to our player or fighting, but not shared (so far): {}; friends' games do not see it ({})", a_local, name ? name : "", a_actor->race ? a_actor->race->GetFormID() : 0,
				SpawnBook::BlockName(a_block), DescribeBase(a_actor));
		}

		// Friends' spawns that have no copy here and are not about to get one, by spawn id, with the reason last said
		// (SpawnBook::Theirs::Why): a reason is logged once, after the spawn has been known UNPLACED_AFTER, a few lines for
		// each reason in all (an owner's stand-in walking off from its spawns again and again must not use up the lines a
		// unique base or a failed placing needs), and counted in `net status` (unplaced=). A spawn merely far from our
		// player is the usual case and only counted.
		constexpr auto                                                    UNPLACED_AFTER = std::chrono::seconds(3);
		std::unordered_map<std::uint32_t, SpawnBook::Theirs::Why>        unplacedWhy;
		std::array<std::uint32_t, static_cast<std::size_t>(SpawnBook::Theirs::Why::kUnknown) + 1> unplacedLogged{};  // lines said, for each reason

		void NoteUnplaced(std::uint32_t a_id, const SpawnBook::Theirs::Copy& a_copy, SpawnBook::Theirs::Why a_why, const SpawnBook::Facts& a_facts, Clock::time_point a_now)
		{
			using Why = SpawnBook::Theirs::Why;
			if (a_why == Why::kRollWait || a_why == Why::kRetryWait || a_why == Why::kPlace || a_why == Why::kHasCopy) {
				unplacedWhy.erase(a_id);
				return;
			}
			const auto seen = seenAt.find(a_id);
			if (seen == seenAt.end() || a_now - seen->second < UNPLACED_AFTER) {
				return;
			}
			auto& last = unplacedWhy.try_emplace(a_id, Why::kPlace).first->second;  // kPlace: none said yet
			if (last == a_why) {
				return;
			}
			last = a_why;
			auto& said = unplacedLogged[static_cast<std::size_t>(a_why)];
			if (a_why == Why::kFarFromUs || said >= LOGGED) {
				return;
			}
			++said;
			const float fromUs = RemoteEdits::Distance(a_copy.at, a_facts.player);
			const float fromOwner = a_facts.owner ? RemoteEdits::Distance(*a_facts.owner, a_copy.at) : -1.0f;
			REX::INFO("Spawns: player {}'s {:08X} (base {:08X}) has no copy here: {} ({:.0f} units from our player, {:.0f} from its owner's stand-in, -1 if not heard)", a_copy.spawn.playerId, a_id, a_copy.spawn.base,
				SpawnBook::Theirs::WhyName(a_why), fromUs, fromOwner);
		}

		// For `net status`: our runtime NPCs that are hostile but cannot be shared, and the spawns of friends with no copy here
		// for a lasting reason: their base is no NPC we place / it is far from our player / its owner's stand-in is unheard or far.
		std::string Unplaced()
		{
			using Why = SpawnBook::Theirs::Why;
			int base = 0, far = 0, owner = 0;
			for (const auto& [id, why] : unplacedWhy) {
				base += why == Why::kBaseNotOk || why == Why::kGaveUp;
				far += why == Why::kFarFromUs;
				owner += why == Why::kOwnerUnheard || why == Why::kOwnerFar;
			}
			return std::format(" notShared={} unplaced={}/{}/{}", blocked, base, far, owner);
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
		RE::ObjectRefHandle PlaceCopy(const SpawnBook::Theirs::Copy& a_copy, std::uint32_t a_base, RE::TESObjectCELL* a_cell)
		{
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
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
				Refresh(a_id, it->second);  // one the engine unloaded is forgotten here, its record dropped
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

		// Somebody here is in our copy of a friend's frame (our player, getting in or in it, or anyone else of ours), or it
		// is the frame our player left last: theirs now (SpawnBook::Act::kAbandon).
		bool Occupied(RE::TESObjectREFR* a_frame)
		{
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				if ((occupied && occupied.get() == a_frame) || player->lastUsedPowerArmor.get().get() == a_frame) {
					return true;
				}
			}
			return NpcSync::InFrame(a_frame);
		}

		// Our copy of a frame a friend's game made (SpawnBook::Theirs::DecideFrame): an empty frame of its base, as the
		// copies a stand-in gets into are (a frame's pieces are each game's own loot), standing where their game has it
		// while that is in our sight. a_cell: our player's.
		void UpdateFrameCopy(std::uint32_t a_id, RE::TESObjectCELL* a_cell, Clock::time_point a_now, int& a_placedNow)
		{
			const auto copy = theirs.Find(a_id);
			if (!copy) {
				return;
			}
			SpawnBook::FrameFacts            facts{ .inView = NpcSync::InSight(copy->at) };
			facts.lent = FrameLoans::LentToUs(a_id);  // held as it is for the loan (FrameLoans)
			// A frame of our own, of that base, standing where their game has theirs (two saves with a shared past hold the
			// same frames): ours stands for it, and no copy goes beside it (FrameShare::SAME_SPOT). Never one lent to us: our
			// copy is the frame our player borrowed, held for the loan whatever of ours stands at that spot.
			facts.oursThere = !facts.lent && facts.inView && NpcSync::OwnMadeFrameAt(copy->spawn.base, copy->at, FrameShare::SAME_SPOT);
			if (facts.oursThere) {
				if (oursThere.insert(a_id).second) {
					++frameCopiesWithheld;
					REX::INFO("Spawns: a frame of our own stands where player {}'s game has their frame {:08X} (base {:08X}), at ({:.0f},{:.0f},{:.0f}); it stands for theirs, no copy beside it", copy->spawn.playerId, a_id,
						copy->spawn.base, copy->at.x, copy->at.y, copy->at.z);
				}
			} else {
				oursThere.erase(a_id);
			}
			RE::NiPointer<RE::TESObjectREFR> ref;
			if (copy->local != 0) {
				ref = CopyOf(a_id);  // one the engine unloaded with its cell: back, or forgotten (see Refresh)
				facts.copyThere = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
				if (facts.copyThere) {
					facts.occupied = Occupied(ref.get());
					facts.leaving = RemotePlayers::BeingSentOut(ref.get());
					// Just placed, it has a cell before its 3D (SpawnBook::LOAD_WAIT).
					if (ref->GetParentCell()) {
						facts.copyAt = SpotOf(ref.get());
						facts.copyHeading = ref->data.angle.z;
						facts.copyLoaded = ref->Get3D() != nullptr;
					}
				}
			}
			const auto playerId = copy->spawn.playerId;
			const auto base = copy->spawn.base;
			const auto at = copy->at;
			const auto heading = copy->heading;
			switch (theirs.DecideFrame(a_id, facts, a_now)) {
			case SpawnBook::Act::kPlace:
				{
					if (a_placedNow >= PLACE_PER_FRAME) {
						break;
					}
					++a_placedNow;
					// The copy their status had us show where they left it parked (ArmorFrames::ChoosePark) is this frame:
					// taken over and moved onto the spot next, not a second suit placed in it.
					auto       handle = RemotePlayers::TakeParkedCopy(playerId, base, at);
					const bool adopted = static_cast<bool>(handle);
					if (!adopted) {
						handle = RemotePlayers::PlaceFrameCopy(base, a_cell, { at.x, at.y, at.z }, heading);
					}
					const auto placedRef = handle.get();
					if (!placedRef) {
						theirs.Placed(a_id, 0, a_now);
						++placeFailed;
						REX::WARN("Spawns: could not place a copy of player {}'s frame {:08X} (base {:08X})", playerId, a_id, base);
						break;
					}
					placed[a_id] = { handle, a_now, SaveGuard::IdsOf(placedRef.get()) };
					theirs.Placed(a_id, placedRef->GetFormID(), a_now);
					++frameCopiesPlaced;
					REX::INFO("Spawns: {} {} at ({:.0f},{:.0f},{:.0f}) as our copy of player {}'s frame {:08X} (base {:08X}), made in their game", adopted ? "took the parked copy" : "placed", RefTag::Of(placedRef.get()), at.x, at.y, at.z,
						playerId, a_id, base);
					break;
				}
			case SpawnBook::Act::kMove:
				ref->data.location = { at.x, at.y, at.z };
				ref->data.angle = { 0.0f, 0.0f, heading };
				ref->Update3DPosition(true);
				++frameCopiesMoved;
				break;
			case SpawnBook::Act::kDiscard:
				if (ref) {
					const float off = facts.copyAt ? RemoteEdits::Distance(*facts.copyAt, at) : -1.0f;
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame {:08X} goes (over={} inView={} inCell={} loaded={} leaving={} oursThere={} {:.0f} units off)", ref->GetFormID(),
						playerId, a_id, copy->gone, facts.inView, facts.copyAt.has_value(), facts.copyLoaded, facts.leaving, facts.oursThere, off);
				}
				Discard(a_id);
				break;
			case SpawnBook::Act::kLost:
				placed.erase(a_id);
				theirs.Unplaced(a_id);
				++copiesLost;
				break;
			case SpawnBook::Act::kAbandon:
				REX::INFO("Spawns: someone here got into {:08X}, our copy of player {}'s frame {:08X}; it is theirs now", ref->GetFormID(), playerId, a_id);
				SaveGuard::Release(ref->GetFormID());  // no longer ours: never kept out of a save, nor noted at a load
				placed.erase(a_id);
				theirs.Taken(a_id);
				++frameCopiesTaken;
				break;
			case SpawnBook::Act::kForget:
				theirs.Forget(a_id);
				break;
			case SpawnBook::Act::kNone:
				break;
			}
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
		if (const auto block = SpawnBook::ShareBlock(base, (at.cell == 0) != (at.worldspace == 0)); block != SpawnBook::Block::kNone) {
			NoteBlocked(a_actor, local, block);
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

	std::uint32_t ShareFrame(RE::TESObjectREFR* a_frame, bool a_share)
	{
		if (!a_frame || !a_share || LocalPlayer() == 0) {
			return 0;
		}
		const auto local = a_frame->GetFormID();
		if (!Protocol::IsSpawnRef(local) || theirs.IdOf(local) != 0 || Leaving(local)) {
			return 0;
		}
		if (const auto id = ours.IdOf(local)) {
			return id;
		}
		std::erase_if(ownFrames, [](std::uint32_t a_local) { return !ours.Shares(a_local); });
		if (!ownFrames.contains(local) && !FrameShare::RoomForMade(ownFrames.size())) {
			return 0;
		}
		const auto base = a_frame->GetObjectReference();
		const auto at = SpotOf(a_frame);
		if (!base || !Protocol::IsShareableRef(base->GetFormID()) || (at.cell == 0) == (at.worldspace == 0)) {
			return 0;
		}
		const auto id = ours.Offer(local, Clock::now(), [](std::uint32_t a_id) { return theirs.Knows(a_id); });
		if (!id) {
			return 0;
		}
		ownFrames.insert(local);
		outgoing.push_back(Protocol::Encode(Protocol::Spawn{ 0, *id, base->GetFormID(), at.cell, at.worldspace, at.x, at.y, at.z, a_frame->data.angle.z }, Protocol::MessageType::kReportSpawn));
		++framesReported;
		REX::INFO("Spawns: sharing our frame {:08X} (base {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X} as {:08X}", local, base->GetFormID(), at.x, at.y, at.z, at.cell ? at.cell : at.worldspace, *id);
		return 0;  // not until the server grants it
	}

	bool FrameCopyAt(std::uint32_t a_owner, const Protocol::ParkedFrame& a_parked, float a_radius)
	{
		const auto id = theirs.FrameAt(a_owner, a_parked.base, { a_parked.cell, a_parked.worldspace, a_parked.x, a_parked.y, a_parked.z }, a_radius);
		const auto it = id != 0 ? placed.find(id) : placed.end();
		const auto ref = it != placed.end() ? it->second.handle.get() : RE::NiPointer<RE::TESObjectREFR>{};
		return ref && !ref->IsDeleted() && !ref->IsDisabled() && ref->GetParentCell();
	}

	std::uint32_t FrameCopyOwnerOf(std::uint32_t a_local)
	{
		const auto id = theirs.IdOf(a_local);
		const auto copy = id != 0 && placed.contains(id) ? theirs.Find(id) : nullptr;
		return copy ? copy->spawn.playerId : 0;
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
			REX::INFO("Spawns: player {} shared {:08X} again; our copy {:08X} of it goes", a_spawn.playerId, a_spawn.refId, known->local);
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
			unplacedWhy.clear();  // none left to have no copy (`net status` counts these)
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
			// A frame its owner's game made: placed as a friend's parked frame is, not as an NPC.
			if (Equipment::IsPowerArmorFrame(RE::TESForm::GetFormByID(copy->spawn.base))) {
				UpdateFrameCopy(id, cell, now, placedNow);
				continue;
			}
			SpawnBook::Facts facts{ .player = us };
			if (const auto state = RemotePlayers::LastStateOf(copy->spawn.playerId)) {
				facts.owner = RemoteEdits::SpotOf(*state);
			}
			const auto from = PlaceBase(id, copy->spawn.base);
			if (copy->local != 0) {
				const auto ref = CopyOf(id);  // one the engine unloaded with its cell: back, or forgotten (see Refresh)
				facts.copyThere = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
				// Just placed, it has a cell before its 3D (SpawnBook::LOAD_WAIT): not taken for one that unloaded.
				if (facts.copyThere && ref->GetParentCell()) {
					facts.copyAt = SpotOf(ref.get());
					facts.copyLoaded = ref->Get3D() != nullptr;
				}
			} else {
				facts.baseOk = BaseOk(from);
				const auto seen = seenAt.try_emplace(id, now).first->second;
				facts.waiting = !NpcSync::RollOf(id) && now - seen < NpcRoll::IDENTITY_WAIT;
			}
			const auto act = theirs.Decide(id, facts, now);
			switch (act) {
			case SpawnBook::Act::kPlace:
				{
					if (placedNow >= PLACE_PER_FRAME) {
						break;
					}
					++placedNow;
					const auto playerId = copy->spawn.playerId;
					const auto base = copy->spawn.base;
					const auto handle = PlaceCopy(*copy, from, cell);
					const auto ref = handle.get();
					if (!ref) {
						theirs.Placed(id, 0, now);
						++placeFailed;
						REX::WARN("Spawns: could not place a copy of player {}'s {:08X} (base {:08X}, from {:08X})", playerId, id, base, from);
						break;
					}
					placed[id] = { handle, now, SaveGuard::IdsOf(ref.get()) };
					theirs.Placed(id, ref->GetFormID(), now);
					++copiesPlaced;
					if (logged < LOGGED) {
						++logged;
						REX::INFO("Spawns: placed {} as our copy of player {}'s {:08X}", RefTag::Of(ref.get()), playerId, id);
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
				seenAt.erase(id);
				break;
			case SpawnBook::Act::kMove:     // a frame's (UpdateFrameCopy): never an NPC's
			case SpawnBook::Act::kAbandon:
			case SpawnBook::Act::kNone:
				break;
			}
			if (act == SpawnBook::Act::kNone && copy->local == 0) {
				NoteUnplaced(id, *copy, theirs.WhyNotPlaced(id, facts, now), facts, now);
			} else if (act != SpawnBook::Act::kNone) {
				unplacedWhy.erase(id);
			}
		}
		// A spawn forgotten some other way (its player gone, the server's word) takes its first sighting with it.
		std::erase_if(unplacedWhy, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(seenAt, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(oursThere, [&](std::uint32_t a_id) { return !theirs.Find(a_id); });
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void DiscardCopies()
	{
		for (const auto id : theirs.Ids()) {
			const auto copy = theirs.Find(id);
			if (!copy || copy->local == 0) {
				continue;
			}
			// A frame copy someone is being sent out of is kept as it is, saved with the frame they wear (a load takes it with
			// the old world): buried, the save would have them wear a frame it does not have (RemotePlayers::SomeoneSentOutOf).
			if (const auto it = placed.find(id); it != placed.end()) {
				// One our player wears on loan is saved with them, as it is (FrameLoans: the LOAN record cleans it up after a load).
				if (const auto ref = it->second.handle.get(); ref && FrameLoans::KeepsThroughSave(ref.get())) {
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame is kept through the save or load; our player wears it on loan", ref->GetFormID(), copy->spawn.playerId);
					continue;
				}
				if (const auto ref = it->second.handle.get(); ref && RemotePlayers::SomeoneSentOutOf(ref.get())) {
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame is kept through the save or load; someone is being sent out of it", ref->GetFormID(), copy->spawn.playerId);
					RemotePlayers::NoteKeptThroughSave(ref.get());
					continue;
				}
			}
			Discard(id);
		}
	}

	bool DropLentCopy(std::uint32_t a_id)
	{
		if (!placed.contains(a_id)) {
			return false;
		}
		Discard(a_id);
		return true;
	}

	std::vector<RE::ObjectRefHandle> FrameCopies()
	{
		std::vector<RE::ObjectRefHandle> out;
		for (const auto& [id, copy] : placed) {
			const auto spawn = theirs.Find(id);
			if (spawn && Equipment::IsPowerArmorFrame(RE::TESForm::GetFormByID(spawn->spawn.base))) {
				out.push_back(copy.handle);
			}
		}
		return out;
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
		blockedNoted.clear();  // the loaded world's ids name other NPCs
		unplacedWhy.clear();
		oursThere.clear();  // the loaded world's frames are looked at afresh
		// Our NPCs' ids name the loaded game's references now: the ones we shared are let go (the others' copies go,
		// before anything the loaded game makes is offered under the same id), and what it makes is shared afresh.
		const auto released = ours.Ids();
		ours.Clear();
		ownFrames.clear();
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
		// Every copy goes, as at a save, but none is kept: one someone is being sent out of waits for them in RemotePlayers
		// (or, with the session over, is left to them).
		for (const auto id : theirs.Ids()) {
			if (const auto copy = theirs.Find(id); copy && copy->local != 0) {
				Discard(id);
			}
		}
		placed.clear();
		seenAt.clear();
		unplacedWhy.clear();
		blockedNoted.clear();
		theirs.Clear();
		ours.Clear();
		ownFrames.clear();
		oursThere.clear();
		killedHere.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("spawns: ours={} theirs={} copies={} reported={} granted={} refused={} received={} placed={} placeFailed={} discarded={} lost={} killed={}/{} frames: ours={} reported={} placed={} moved={} taken={} unloaded={} back={} oursThere={}/{}", ours.Size(),
			theirs.Size(), theirs.PlacedCount(), reported, granted, refused, received, copiesPlaced, placeFailed, copiesDiscarded, copiesLost, copiesKilled, oursKilled, ownFrames.size(), framesReported,
			frameCopiesPlaced, frameCopiesMoved, frameCopiesTaken, copiesUnloaded, copiesBack, oursThere.size(), frameCopiesWithheld) + Unplaced() + " " + FrameLoans::Describe();
	}
}
