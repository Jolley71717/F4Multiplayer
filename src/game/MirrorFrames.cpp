#include "game/MirrorFrames.h"

#include "MainThread.h"
#include "game/Equipment.h"
#include "game/InventoryStacks.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"

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
		};

		std::unordered_map<std::uint32_t, Entry> entries;  // by the mirror's form ID

		struct Counters
		{
			std::uint32_t placed = 0, in = 0, inFailed = 0, out = 0, outFailed = 0, gaveUp = 0, dropped = 0, givenBack = 0, graphs = 0;
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

		void DropCopy(Entry& a_entry)
		{
			if (a_entry.copyId != 0) {
				RemotePlayers::Discard(a_entry.copy, a_entry.placedAt);
				++counters.dropped;
			}
			a_entry.copy = {};
			a_entry.copyId = 0;
			a_entry.copyBase = 0;
			a_entry.tries = 0;
			a_entry.in = false;
		}

		// Out of its copy (when it is in it), what the switch-in gave it and the exit left taken back, the copy gone.
		void GetOut(std::uint32_t a_id, Entry& a_entry, std::string_view a_why)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
			const auto copy = a_entry.copy.get();
			if (actor && copy && Equipment::InPowerArmor(actor) && Equipment::PowerArmorFrameOf(actor).get() == copy.get()) {
				const auto state = Equipment::PowerArmorState(actor);
				if (Equipment::SwitchOutOfPowerArmor(actor, copy.get())) {
					++counters.out;
					a_entry.outAt = Clock::now();
					REX::INFO("MirrorFrames: {:08X} out of frame copy {:08X} ({})", a_id, copy->GetFormID(), a_why);
				} else {
					++counters.outFailed;
					REX::ERROR("MirrorFrames: {:08X} did not get out of frame copy {:08X} ({}; state {} -> {}, in power armor={})", a_id, copy->GetFormID(), a_why, state,
						Equipment::PowerArmorState(actor), Equipment::InPowerArmor(actor));
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
			// Still in it (the exit did not take): left as it is, never deleted from under it (its frame gone, a save would have it
			// wear one it does not have). Said above.
			if (actor && copy && Equipment::PowerArmorFrameOf(actor).get() == copy.get()) {
				return;
			}
			DropCopy(a_entry);
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

	void Leave(std::uint32_t a_actorId, std::string_view a_why)
	{
		const auto it = entries.find(a_actorId);
		if (it == entries.end()) {
			return;
		}
		MainThread::Assert("MirrorFrames::Leave");
		GetOut(a_actorId, it->second, a_why);
		entries.erase(it);
	}

	void LeaveAll(std::string_view a_why)
	{
		std::vector<std::uint32_t> ids;
		for (const auto& [id, entry] : entries) {
			ids.push_back(id);
		}
		for (const auto id : ids) {
			Leave(id, a_why);
		}
	}

	void Forget()
	{
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
				GetOut(id, entry, "no longer mirrored");  // StopMirroring leaves it first; this is for one it missed
				it = entries.erase(it);
				continue;
			}
			auto copy = entry.copy.get();
			if (entry.copyId != 0 && (!copy || copy->IsDeleted())) {
				// Gone from under us (its cell unloaded): forgotten here, its record kept out of saves (SaveGuard, as placed).
				const bool wasIn = entry.in;
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
				GetOut(id, entry, entry.want ? "its owner's copy is in another frame" : "its owner's copy got out");
				break;
			case Do::kDropCopy:
				DropCopy(entry);
				break;
			case Do::kGiveUp:
				REX::WARN("MirrorFrames: {:08X} is shown out of power armor: its frame copy {:08X} {}", id, entry.copyId,
					entry.tries >= MAX_TRIES ? "did not take it in" : "did not load");
				entry.gaveUp = entry.want;
				++counters.gaveUp;
				DropCopy(entry);
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
		const auto copy = it != entries.end() ? it->second.copy.get() : nullptr;
		auto out = std::format("{:08X} inPA={} state={} frame={:08X} ourCopy={} skeleton={} level={} badPosition={} mirror={}", a_actor->GetFormID(), Equipment::InPowerArmor(a_actor),
			Equipment::PowerArmorState(a_actor), frame ? frame->GetFormID() : 0, frame && copy && frame.get() == copy.get(), Equipment::HasPowerArmorSkeleton(a_actor),
			process ? static_cast<int>(process->processLevel) : -1, a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kBadPosition),
			Puppets::IsRegistered(a_actor) && !Puppets::IsPuppet(a_actor));
		if (it != entries.end()) {
			const auto& e = it->second;
			out += std::format(" want={:08X} copy={:08X} copyBase={:08X} copyLoaded={} pieces={} tries={} gaveUp={:08X}", e.want, e.copyId, e.copyBase, copy && copy->Get3D() != nullptr,
				e.pieces.size(), e.tries, e.gaveUp);
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("mirrorFrames: tracked={} placed={} in={} inFailed={} out={} outFailed={} gaveUp={} dropped={} givenBack={} graphs={}", entries.size(), counters.placed,
			counters.in, counters.inFailed, counters.out, counters.outFailed, counters.gaveUp, counters.dropped, counters.givenBack, counters.graphs);
	}
}
