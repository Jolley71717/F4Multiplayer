#include "game/MirrorFrames.h"

#include "MainThread.h"
#include "game/Equipment.h"
#include "game/InventoryStacks.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"

#include <format>
#include <optional>
#include <unordered_map>

namespace MirrorFrames
{
	namespace
	{
		struct Entry
		{
			std::uint32_t              want = 0;    // the frame base its owner's copy is in (Want), 0 out of one
			std::vector<std::uint32_t> pieces;      // the owner's copy's power armor pieces (Want)
			RE::ObjectRefHandle        copy;        // the frame copy placed for it
			std::uint32_t              copyId = 0;  // its form ID (the handle stops resolving as it goes)
			std::uint32_t              copyBase = 0;
			Clock::time_point          placedAt{};
			int                        tries = 0;
			Clock::time_point          triedAt{};
			std::uint32_t              gaveUp = 0;  // the base given up on (until the owner's copy is in another, or out)
			std::vector<Count>         before;      // its armor as it went in (what the exit is to leave it with)
			bool                       in = false;  // switched into the copy by us
			const RE::NiAVObject*      root = nullptr;  // its 3D last seen (a new one has a fresh graph: Puppets::GraphRebuilt)
			std::optional<Clock::time_point> outAt;    // got out at: its own 3D on its way (a fresh graph once it is in)
			std::optional<Clock::time_point> heldAt;   // held in its copy by NpcSync since (its owner's copy silent: Held)
			Clock::time_point                heldSeen{};  // ...last this frame or one before (a gap: mirrored on, Update)
		};

		std::unordered_map<std::uint32_t, Entry> entries;  // by the mirror's form ID

		struct Counters
		{
			std::uint32_t placed = 0, in = 0, inFailed = 0, out = 0, outFailed = 0, outWaited = 0, gaveUp = 0, dropped = 0, givenBack = 0, graphs = 0, released = 0, releasedKnocked = 0,
			              releasedNotMirror = 0, held = 0, heldOn = 0, heldLeft = 0, unloaded = 0;
		} counters;

		std::vector<Count> ArmorOf(RE::Actor* a_actor)
		{
			std::vector<Count> out;
			const auto list = a_actor ? a_actor->inventoryList : nullptr;
			if (!list) {
				return out;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				if (!item.object || item.object->GetFormType() != RE::ENUM_FORM_ID::kARMO) {
					continue;
				}
				if (!Equipment::IsPowerArmorPiece(item.object) && !Equipment::IsFrameItem(item.object)) {
					continue;  // only what a frame hands over: the rest of its armor is its own
				}
				out.push_back({ item.object->GetFormID(), static_cast<std::int32_t>(item.GetCount()) });
			}
			return out;
		}

		bool Ready(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			return Puppets::IsRegistered(a_actor) && !Puppets::IsPuppet(a_actor) && a_actor->Get3D() && !a_actor->IsDead(false) && !a_actor->IsDisabled() && process &&
			       process->processLevel <= 1 && a_actor->GetParentCell();
		}

		// The copy placed for it: by its handle, or the frame the actor is in when that is it under the same ID (a handle that
		// stopped resolving while the engine still has her in it).
		RE::NiPointer<RE::TESObjectREFR> CopyOf(const Entry& a_entry, RE::Actor* a_actor)
		{
			if (auto copy = a_entry.copy.get()) {
				return copy;
			}
			if (a_entry.copyId != 0 && a_actor) {
				if (auto frame = Equipment::PowerArmorFrameOf(a_actor); frame && frame->GetFormID() == a_entry.copyId) {
					return frame;
				}
			}
			return {};
		}

		// The copy is forgotten: the engine let go of it (unloaded with its cell) or it is on its way out already. Its record of
		// changes, kept under its cell, is dropped (SaveGuard::Forget), so neither a save nor the cell loading again has it.
		void ForgetCopy(std::uint32_t a_id, const Entry& a_entry, std::string_view a_how)
		{
			SaveGuard::Forget(a_entry.copyId);
			++counters.unloaded;
			REX::INFO("MirrorFrames: {:08X}'s frame copy {:08X} {}: forgotten, its record dropped", a_id, a_entry.copyId, a_how);
		}

		// The copy goes (nobody is in it): buried while its handle names it under its ID, forgotten else (HowToLetGo).
		void DropCopy(std::uint32_t a_id, Entry& a_entry)
		{
			const auto ref = a_entry.copy.get();
			switch (HowToLetGo(a_entry.copyId, ref ? ref->GetFormID() : 0)) {
			case LetGo::kNothing:
				break;
			case LetGo::kDiscard:
				RemotePlayers::Discard(a_entry.copy, a_entry.placedAt);
				++counters.dropped;
				break;
			case LetGo::kForget:
				ForgetCopy(a_id, a_entry, "was let go of by the engine (unloaded with its cell, its cell not loaded here)");
				break;
			}
			a_entry.copy = {};
			a_entry.copyId = 0;
			a_entry.copyBase = 0;
			a_entry.tries = 0;
			a_entry.in = false;
		}

		// How long a save, a load or the session's end waits on an anim graph preload of the mirror's still loading before its
		// exit (Equipment::SwitchOutOfPowerArmor); a frame's own try does not wait (tried again next frame).
		constexpr auto LEAVE_WAIT = std::chrono::milliseconds(1500);

		bool IsDown(RE::Actor* a_actor)
		{
			const auto& state = static_cast<RE::ActorState&>(*a_actor);
			return Down(a_actor->IsDead(false), static_cast<std::int32_t>(state.lifeState), static_cast<std::uint32_t>(state.knockState));
		}

		// The copy left to the game with the mirror in it (Leaving::kRelease): saved as the game's from now on, so a save has the
		// frame it wears; nothing of ours holds it any more.
		void ReleaseCopy(std::uint32_t a_id, Entry& a_entry, std::string_view a_why, std::string_view a_because)
		{
			SaveGuard::Release(a_entry.copyId);  // the frame it is in (GetOut found it is the copy, under this ID)
			++counters.released;  // the living ones among them are counted apart too (releasedKnocked, releasedNotMirror, outFailed)
			REX::WARN("MirrorFrames: {:08X} stays in frame copy {:08X} ({}; {}): the copy is left to the game with it", a_id, a_entry.copyId, a_why, a_because);
			a_entry.copy = {};
			a_entry.copyId = 0;
			a_entry.copyBase = 0;
			a_entry.tries = 0;
			a_entry.in = false;
		}

		// Out of its copy (when it is in it), what the switch-in gave it and the exit left taken back, the copy gone.
		// a_leaving: we stop mirroring it (Leave), so nothing is tried again: a body (Down), or one the exit did not take out,
		// is left in the copy and the copy left to the game (ChooseLeaving, AfterExit). Else (a frame's own try) one still in
		// it is left as it is, tried again next frame.
		void GetOut(std::uint32_t a_id, Entry& a_entry, std::string_view a_why, std::chrono::milliseconds a_wait, bool a_leaving)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
			const auto copy = CopyOf(a_entry, actor);
			const bool inCopy = actor && copy && Equipment::InPowerArmor(actor) && Equipment::PowerArmorFrameOf(actor).get() == copy.get();
			const bool down = inCopy && IsDown(actor);
			const bool mirror = actor && Puppets::IsRegistered(actor) && !Puppets::IsPuppet(actor);
			const auto leaving = ChooseLeaving(inCopy, down, mirror);
			if (leaving == Leaving::kRelease) {
				if (a_leaving) {
					// Alive and left in a frame of this world's for good (and the owner's pieces on it): counted apart from the
					// bodies, which the engine leaves in their suits anyway.
					const auto& life = static_cast<RE::ActorState&>(*actor);
					const bool  dead = actor->IsDead(false) || static_cast<std::int32_t>(life.lifeState) == LIFE_DYING || static_cast<std::int32_t>(life.lifeState) == LIFE_DEAD;
					if (!down) {
						++counters.releasedNotMirror;
					} else if (!dead) {
						++counters.releasedKnocked;
					}
					ReleaseCopy(a_id, a_entry, a_why,
						!down  ? "no longer a mirror (a stop missed): never ours to take out"
						: dead ? "dead or dying: never taken out"
							   : "knocked down, alive: never taken out under a ragdoll");
				}
				return;  // never exited (Choose never asks it for a body, and runs only for a mirror)
			}
			if (leaving == Leaving::kExit) {
				const auto state = Equipment::PowerArmorState(actor);
				switch (Equipment::SwitchOutOfPowerArmor(actor, copy.get(), a_wait)) {
				case Equipment::ExitResult::kOut:
					++counters.out;
					a_entry.outAt = Clock::now();
					REX::INFO("MirrorFrames: {:08X} out of frame copy {:08X} ({}{})", a_id, a_entry.copyId, a_why, actor->Get3D() ? "" : "; no 3D, its cell unloaded");
					break;
				case Equipment::ExitResult::kWaiting:
					// A preload of its 3D still loading: nothing done over it. A frame's try is made again next frame; a save, a
					// load or the session's end waited LEAVE_WAIT for it and goes on (said: the copy is kept out of the save).
					if (++counters.outWaited == 1 || a_wait.count() > 0) {
						REX::WARN("MirrorFrames: {:08X}'s exit from frame copy {:08X} waits on the engine's preload of its 3D ({}; state {} -> {}, waited {} ms)", a_id,
							a_entry.copyId, a_why, state, Equipment::PowerArmorState(actor), a_wait.count());
					}
					break;
				case Equipment::ExitResult::kFailed:
				case Equipment::ExitResult::kRefused:
					++counters.outFailed;
					REX::ERROR("MirrorFrames: {:08X} did not get out of frame copy {:08X} ({}; state {} -> {}, in power armor={})", a_id, a_entry.copyId, a_why, state,
						Equipment::PowerArmorState(actor), Equipment::InPowerArmor(actor));
					break;
				}
			}
			if (actor && a_entry.in && !Equipment::InPowerArmor(actor)) {
				for (const auto& gained : Gained(a_entry.before, ArmorOf(actor))) {
					const auto object = RE::TESForm::GetFormByID<RE::TESBoundObject>(gained.item);
					if (!object) {
						continue;
					}
					RE::TESObjectREFR::RemoveItemData data{ object, gained.count };
					actor->RemoveItem(data);
					counters.givenBack += static_cast<std::uint32_t>(gained.count);
					REX::INFO("MirrorFrames: {:08X} gives back {} of {:08X} the switch-in gave it", a_id, gained.count, gained.item);
				}
			}
			// Still in it (the exit did not take): never deleted from under it (its frame gone, a save would have it wear one it
			// does not have). Leaving, the copy is the game's now; else tried again next frame. Said above.
			if (actor && copy && Equipment::PowerArmorFrameOf(actor).get() == copy.get()) {
				if (a_leaving && AfterExit(true) == Leaving::kRelease) {
					ReleaseCopy(a_id, a_entry, a_why, "its exit did not take");
				}
				return;
			}
			// Out: the copy goes. An exit run while its cell is not loaded here (it unloaded here, a save, the session's end)
			// has already had the engine unload it with the demote: forgotten then, its record dropped (HowToLetGo).
			DropCopy(a_id, a_entry);
		}

		void GetIn(std::uint32_t a_id, Entry& a_entry, RE::Actor* a_actor, RE::TESObjectREFR* a_copy)
		{
			a_entry.triedAt = Clock::now();
			// The copy holds the owner's pieces alone: emptied (a loot frame's base fills it from its container the first time it is
			// read), then given them; the switch-in moves them onto the mirror and puts them on.
			InventoryStacks::RemoveAllBut(a_copy, [](RE::TESBoundObject*, RE::ExtraDataList* a_extra) { return InventoryStacks::QuestItem(a_extra); });
			for (const auto piece : a_entry.pieces) {
				if (const auto object = RE::TESForm::GetFormByID<RE::TESBoundObject>(piece)) {
					a_copy->AddObjectToContainer(object, nullptr, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
				}
			}
			a_entry.before = ArmorOf(a_actor);
			if (Equipment::SwitchIntoPowerArmor(a_actor, a_copy, true)) {
				a_entry.in = true;
				a_entry.outAt.reset();
				++counters.in;
				REX::INFO("MirrorFrames: {:08X} is in frame copy {:08X} (base {:08X}, {} pieces), as its owner's copy is", a_id, a_copy->GetFormID(), a_entry.copyBase, a_entry.pieces.size());
				return;
			}
			++a_entry.tries;
			++counters.inFailed;
			REX::WARN("MirrorFrames: {:08X} did not get into frame copy {:08X} (try {} of {}; in power armor={})", a_id, a_copy->GetFormID(), a_entry.tries, MAX_TRIES,
				Equipment::InPowerArmor(a_actor));
		}

		// A fresh 3D (the power armor skeleton in, or its own back) has a fresh graph: told at once whether it moves.
		void WatchGraph(Entry& a_entry, RE::Actor* a_actor)
		{
			const auto root = a_actor->Get3D();
			if (!root || root == a_entry.root || (!a_entry.in && !a_entry.outAt)) {
				a_entry.root = root;
				return;
			}
			const bool armor = Equipment::HasPowerArmorSkeleton(a_actor);
			const bool inNow = Equipment::InPowerArmor(a_actor);
			if (armor == inNow && a_entry.root != nullptr) {
				Puppets::GraphRebuilt(a_actor);
				++counters.graphs;
			}
			a_entry.root = root;
		}
	}

	void Want(RE::Actor* a_actor, std::uint32_t a_frameBase, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_actor) {
			return;
		}
		auto& entry = entries[a_actor->GetFormID()];
		if (entry.want != a_frameBase) {
			entry.gaveUp = 0;
		}
		entry.want = a_frameBase;
		entry.pieces.clear();
		for (const auto item : a_items) {
			if (Equipment::IsPowerArmorPiece(RE::TESForm::GetFormByID(item))) {
				entry.pieces.push_back(item);
			}
		}
	}

	namespace
	{
		void LeaveFor(std::uint32_t a_actorId, std::string_view a_why, std::chrono::milliseconds a_wait)
		{
			const auto it = entries.find(a_actorId);
			if (it == entries.end()) {
				return;
			}
			MainThread::Assert("MirrorFrames::Leave");
			if (it->second.heldAt) {
				++counters.heldLeft;
				REX::INFO("MirrorFrames: {:08X}'s hold in its frame copy ends after {:.1f} s: {}", a_actorId,
					std::chrono::duration<float>(Clock::now() - *it->second.heldAt).count(), a_why);
			}
			GetOut(a_actorId, it->second, a_why, a_wait, true);
			entries.erase(it);
		}
	}

	void Leave(std::uint32_t a_actorId, std::string_view a_why)
	{
		LeaveFor(a_actorId, a_why, LEAVE_WAIT);
	}

	void LeaveAll(std::string_view a_why)
	{
		std::vector<std::uint32_t> ids;
		for (const auto& [id, entry] : entries) {
			ids.push_back(id);
		}
		// One budget for all of them: a save with several mirrors in copies waits LEAVE_WAIT at most, not that for each.
		const auto until = Clock::now() + LEAVE_WAIT;
		for (const auto id : ids) {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
			LeaveFor(id, a_why, (std::max)(left, std::chrono::milliseconds(0)));
		}
	}

	bool InCopy(RE::Actor* a_actor)
	{
		const auto it = a_actor ? entries.find(a_actor->GetFormID()) : entries.end();
		if (it == entries.end()) {
			return false;
		}
		const auto copy = CopyOf(it->second, a_actor);
		return copy && Equipment::InPowerArmor(a_actor) && Equipment::PowerArmorFrameOf(a_actor).get() == copy.get();
	}

	void Held(RE::Actor* a_actor, Duration a_silentFor)
	{
		const auto it = a_actor ? entries.find(a_actor->GetFormID()) : entries.end();
		if (it == entries.end()) {
			return;
		}
		const auto now = Clock::now();
		it->second.heldSeen = now;
		if (!it->second.heldAt) {
			it->second.heldAt = now;
			++counters.held;
			REX::INFO("MirrorFrames: {:08X} is held in frame copy {:08X}, still mirrored: its owner's copy is silent ({:.1f} s); taken out after {} s of it unless it dies or its states come back",
				a_actor->GetFormID(), it->second.copyId, std::chrono::duration<float>(a_silentFor).count(), std::chrono::duration_cast<std::chrono::seconds>(HOLD).count());
		}
	}

	void Forget()
	{
		// LeaveAll ran before the load (Session's step), so none is left in a copy; one that is would be said here.
		for (const auto& [id, entry] : entries) {
			if (entry.copyId != 0) {
				REX::WARN("MirrorFrames: {:08X}'s frame copy {:08X} forgotten at a load without its leaving", id, entry.copyId);
			}
		}
		entries.clear();
	}

	void Update()
	{
		const auto now = Clock::now();
		for (auto it = entries.begin(); it != entries.end();) {
			const auto id = it->first;
			auto&      entry = it->second;
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
			if (!actor || !Puppets::IsRegistered(actor) || Puppets::IsPuppet(actor)) {
				// StopMirroring leaves it first, while it is a mirror; this is a net for one it missed (no longer a mirror: one still
				// in its copy is left in it, the copy the game's, never orphaned).
				GetOut(id, entry, "no longer mirrored", LEAVE_WAIT, true);
				it = entries.erase(it);
				continue;
			}
			if (entry.heldAt && now - entry.heldSeen > std::chrono::milliseconds(500)) {
				++counters.heldOn;
				REX::INFO("MirrorFrames: {:08X}'s hold in its frame copy ends after {:.1f} s: its owner's states are back, mirrored on", id,
					std::chrono::duration<float>(entry.heldSeen - *entry.heldAt).count());
				entry.heldAt.reset();
			}
			auto copy = CopyOf(entry, actor);
			if (entry.copyId != 0 && (!copy || copy->IsDeleted()) && !Equipment::InPowerArmor(actor)) {
				// Gone from under us (unloaded with its cell: placed and not yet got into, it was never made persistent), or deleted
				// by something else: forgotten, its record dropped, so its cell never brings it back (ForgetCopy).
				const bool wasIn = entry.in;
				ForgetCopy(id, entry, copy ? "is deleted from under us" : "is gone from under us (unloaded with its cell)");
				entry.copy = {};
				entry.copyId = 0;
				entry.copyBase = 0;
				entry.in = false;
				copy.reset();
				if (wasIn) {
					REX::WARN("MirrorFrames: {:08X}'s frame copy is gone while it is in it (in power armor={})", id, Equipment::InPowerArmor(actor));
				}
			}
			const auto frame = Equipment::PowerArmorFrameOf(actor);
			Facts facts;
			facts.ownerFrame = entry.want;
			facts.ready = Ready(actor);
			facts.inFrame = Equipment::InPowerArmor(actor);
			facts.inCopy = facts.inFrame && copy && frame.get() == copy.get();
			facts.state = Equipment::PowerArmorState(actor);
			facts.copyPlaced = copy.get() != nullptr;
			facts.copyLoaded = copy && copy->Get3D() != nullptr;
			facts.copyBase = entry.copyBase;
			facts.sincePlaced = now - entry.placedAt;
			facts.tries = entry.tries;
			facts.sinceTried = entry.tries > 0 ? now - entry.triedAt : Duration::max();
			facts.gaveUp = entry.gaveUp != 0 && entry.gaveUp == entry.want;
			facts.down = IsDown(actor);
			switch (Choose(facts)) {
			case Do::kNothing:
				break;
			case Do::kPlaceCopy:
				{
					const auto handle = RemotePlayers::PlaceFrameCopy(entry.want, actor->GetParentCell(), actor->data.location, actor->data.angle.z);
					if (const auto placed = handle.get()) {
						entry.copy = handle;
						entry.copyId = placed->GetFormID();
						entry.copyBase = entry.want;
						entry.placedAt = now;
						entry.tries = 0;
						++counters.placed;
						REX::INFO("MirrorFrames: {:08X} is in power armor in its owner's game (frame {:08X}): frame copy {:08X} placed for it", id, entry.want, entry.copyId);
					} else {
						entry.gaveUp = entry.want;
						++counters.gaveUp;
						REX::WARN("MirrorFrames: no frame copy of {:08X} could be placed for {:08X}", entry.want, id);
					}
				}
				break;
			case Do::kGetIn:
				GetIn(id, entry, actor, copy.get());
				break;
			case Do::kGetOut:
				GetOut(id, entry,
					facts.state != STATE_IN         ? "its exit carried on"
					: entry.want                    ? "its owner's copy is in another frame"
													: "its owner's copy got out",
					std::chrono::milliseconds(0), false);
				break;
			case Do::kDropCopy:
				DropCopy(id, entry);
				break;
			case Do::kGiveUp:
				REX::WARN("MirrorFrames: {:08X} is shown out of power armor: its frame copy {:08X} {}", id, entry.copyId,
					entry.tries >= MAX_TRIES ? "did not take it in" : "did not load");
				entry.gaveUp = entry.want;
				++counters.gaveUp;
				DropCopy(id, entry);
				break;
			}
			WatchGraph(entry, actor);
			if (entry.outAt && now - *entry.outAt > std::chrono::seconds(10)) {
				entry.outAt.reset();
			}
			// Nothing left to do for it: out, no copy, its owner's copy out too.
			if (entry.want == 0 && entry.copyId == 0 && !entry.outAt) {
				it = entries.erase(it);
				continue;
			}
			++it;
		}
	}

	std::string DescribeOf(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "error: no such actor";
		}
		const auto frame = Equipment::PowerArmorFrameOf(a_actor);
		const auto process = a_actor->currentProcess;
		const auto it = entries.find(a_actor->GetFormID());
		const auto copy = it != entries.end() ? CopyOf(it->second, a_actor) : RE::NiPointer<RE::TESObjectREFR>{};
		auto out = std::format("{:08X} inPA={} state={} frame={:08X} ourCopy={} skeleton={} level={} badPosition={} mirror={} preloading={}", a_actor->GetFormID(), Equipment::InPowerArmor(a_actor),
			Equipment::PowerArmorState(a_actor), frame ? frame->GetFormID() : 0, frame && copy && frame.get() == copy.get(), Equipment::HasPowerArmorSkeleton(a_actor),
			process ? static_cast<int>(process->processLevel) : -1, a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kBadPosition),
			Puppets::IsRegistered(a_actor) && !Puppets::IsPuppet(a_actor), Equipment::PowerArmorPreloadPending(a_actor));
		if (it != entries.end()) {
			const auto& e = it->second;
			out += std::format(" want={:08X} copy={:08X} copyBase={:08X} copyLoaded={} pieces={} tries={} gaveUp={:08X}", e.want, e.copyId, e.copyBase, copy && copy->Get3D() != nullptr,
				e.pieces.size(), e.tries, e.gaveUp);
		}
		return out;
	}

	std::string Describe()
	{
		return std::format(
			"mirrorFrames: tracked={} placed={} in={} inFailed={} out={} outFailed={} outWaited={} gaveUp={} dropped={} givenBack={} graphs={} released={} releasedKnocked={} "
			"releasedNotMirror={} held={} heldOn={} heldLeft={} unloaded={}",
			entries.size(), counters.placed, counters.in, counters.inFailed, counters.out, counters.outFailed, counters.outWaited, counters.gaveUp, counters.dropped,
			counters.givenBack, counters.graphs, counters.released, counters.releasedKnocked, counters.releasedNotMirror, counters.held, counters.heldOn, counters.heldLeft, counters.unloaded);
	}
}
