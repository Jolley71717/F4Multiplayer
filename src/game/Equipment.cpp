#include "game/Equipment.h"

#include "MainThread.h"
#include "Protocol.h"
#include "game/ClickWatch.h"
#include "game/GearPlan.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/Puppets.h"
#include "game/StandInGear.h"
#include "game/Gifts.h"
#include "game/Storage.h"
#include "game/TextureFlush.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace Equipment
{
	namespace
	{
		constexpr REL::ID POWER_ARMOR_FURNITURE_KEYWORD{ 2194741 };  // PowerArmor::GetFurnitureKeyword
		constexpr REL::ID POWER_ARMOR_FRAME_KEYWORD{ 2194744 };      // PowerArmor::GetFrameKeyword (+374890)
		constexpr REL::ID POWER_ARMOR_DEFAULT_FRAME{ 2194757 };      // PowerArmor::GetDefaultFrame (+374AB0)
		constexpr REL::ID POWER_ARMOR_RACE{ 2194755 };               // PowerArmor::GetPowerArmorRace (+374A70)
		// PowerArmor's frame pick (+987EF0): the frame item the switch-in puts on the actor (see ArmorFrames::ChoosePick).
		// With true it always picks afresh from what the actor has on, touching nothing.
		constexpr REL::ID POWER_ARMOR_FRAME_PICK{ 2219424 };
		// PowerArmor's switch-in (+989D90), the one the Papyrus native SwitchToPowerArmor (+10FC2E0) calls with false
		// once the frame is furniture with the frame keyword and the actor is in no frame. With false it does, in order:
		// creates the actor's ExtraPowerArmor (state 1), sets the frame as its frame, unequips what the race's body slot
		// conflicts with, gives it the power armor race, sounds the steal alarm if the frame or its cell has an owner the
		// actor is not, moves the frame's items onto the actor (filling the frame from its base's container first if it was
		// never looked into), equips the frame item and locks it on, queues the rebuild of the actor's 3D (high or
		// middle-high process only: Set3DUpdateFlag(0x520) and AIProcess::Update3dModel with true, which clears the flags
		// and hands the rebuild to TaskQueueInterface::QueueActorUpdateModel, or to nothing when the model loader already
		// holds a load for the actor), promotes the frame to persist for the power armor race, queues the frame's
		// disabling, and sets state 2. No package, no AI and no animation are involved. The power armor skeleton is on the
		// actor only once that task has run, a frame or more later.
		constexpr REL::ID SWITCH_INTO_POWER_ARMOR{ 2219442 };
		constexpr REL::ID GET_POWER_ARMOR_HANDLE{ 2190600 };  // ExtraDataList::GetPowerArmorHandle (+2908C0)
		constexpr REL::ID SET_OWNER{ 2202623 };               // TESObjectREFR::SetOwner(TESForm*) (+562380)
		// What the engine asks before it lets our player into furniture (see FrameUse.h).
		constexpr REL::ID GET_MARKER_ENABLED{ 2198041 };      // TESFurniture::GetMarkerEnabled(uint) (+44C590)
		constexpr REL::ID GET_MARKER_USER{ 2202566 };         // TESObjectREFR::GetMarkerUser(ActorHandle&, uint, bool) (+55EA40)
		constexpr REL::ID HAS_FREE_MARKER{ 2202569 };         // TESObjectREFR::HasFreeMarker(bool) (+55F0B0)
		constexpr REL::ID HAS_USERS_OTHER_THAN{ 2202574 };    // TESObjectREFR::HasUsersOtherThan(Actor*, bool) (+55F5A0)
		constexpr REL::ID UPRIGHT_ANGLE_COSINE{ 4796566 };    // InteractionUtils::QUprightAngleCosine's value (+30EC270)
		constexpr std::uint32_t FURNITURE_MARKERS = 22;       // the markers a piece of furniture can have (its loops' bound)
		// A bone the power armor skeleton has and the human one has not (Meshes\Actors\PowerArmor\CharacterAssets\
		// skeleton.nif against Meshes\Actors\Character\CharacterAssets\skeleton.nif in Fallout4 - Meshes.ba2: the armor
		// bones, Pelvis_Armor, Helmet_Armor, Tank_Armor and the rest, are the power armor skeleton's alone; the human one
		// only has P-Armor connect points, which are no nodes).
		constexpr const char* POWER_ARMOR_BONE = "Pelvis_Armor";
		// A leg bone both skeletons have (Fallout 4's own bone names; the power armor one adds its armor bones to them).
		constexpr const char* LEG_BONE = "LLeg_Thigh";

		RE::BGSKeyword* FrameKeyword()
		{
			static REL::Relocation<RE::BGSKeyword* (*)()> keyword{ POWER_ARMOR_FRAME_KEYWORD };
			return keyword();
		}

		RE::TESObjectARMO* DefaultFrame()
		{
			static REL::Relocation<RE::TESObjectARMO* (*)()> frame{ POWER_ARMOR_DEFAULT_FRAME };
			return frame();
		}

		// What an actor was placed from: a leveled one's own base is the game's copy, its leveled data names what was
		// placed. The engine gives a frame to this as an NPC gets out of it (TESObjectREFR::GetOriginalObjectReference).
		RE::TESForm* OriginalBase(RE::Actor* a_actor)
		{
			if (a_actor->extraList) {
				if (const auto leveled = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>(); leveled && leveled->originalBase) {
					return leveled->originalBase;
				}
			}
			return a_actor->GetObjectReference();
		}

		// Both start afresh with each session (OnSessionStart).
		OwnGear::Tally     ownGear;
		OwnGear::LogBudget ownGearLog;

		constexpr std::uint32_t PIP_BOY = 0x00021B3B;  // Fallout4.esm: the Pip-Boy (armor)
		constexpr std::uint32_t CAPS = 0x0000000F;     // Caps001
	}

	OwnGear::Verdict Judge(RE::TESObjectREFR* a_ref, OwnGear::Change a_change)
	{
		OwnGear::Target target;
		target.exists = a_ref && !a_ref->IsDeleted();
		if (a_ref) {
			const auto player = RE::PlayerCharacter::GetSingleton();
			target.player = a_ref->IsPlayerRef() || a_ref == player;
			const auto actor = a_ref->As<RE::Actor>();
			target.standIn = actor && Puppets::IsPuppet(actor);
			target.mirrored = actor && !target.standIn && Puppets::IsRegistered(actor);
			target.madeHere = a_ref->IsCreated();
			const auto base = a_ref->GetObjectReference();
			target.container = !actor && base && base->Is(RE::ENUM_FORM_ID::kCONT);
			const auto ours = player ? PowerArmorFrameOf(player) : RE::NiPointer<RE::TESObjectREFR>{};
			target.playersFrame = ours && ours.get() == a_ref;
			target.inWorld = a_ref->GetParentCell() != nullptr;
			// A persistent reference an inventory took keeps the engine's handle of that inventory (ExtraReferenceHandles'
			// containerRef). Read only for one in no cell: whose inventory it is decides whether a pickup waits.
			if (!target.inWorld && a_ref->extraList) {
				if (const auto handles = a_ref->extraList->GetByType<RE::ExtraReferenceHandles>()) {
					if (const auto holder = handles->containerRef.get(); holder && holder.get() != a_ref) {
						target.heldByPlayer = holder->IsPlayerRef() || holder.get() == player;
						target.heldHere = !target.heldByPlayer;
					}
				}
			}
		}
		return OwnGear::Judge(a_change, target);
	}

	bool Count(RE::TESObjectREFR* a_ref, OwnGear::Change a_change, OwnGear::Verdict a_verdict)
	{
		if (ownGear.Count(a_verdict)) {
			return true;
		}
		switch (ownGearLog.Take(a_verdict)) {
		case OwnGear::LogBudget::Log::kSkip:
			break;
		case OwnGear::LogBudget::Log::kLog:
			REX::WARN("Equipment: {} for {:08X} refused: {}", OwnGear::Name(a_change), a_ref ? a_ref->GetFormID() : 0, OwnGear::Name(a_verdict));
			break;
		case OwnGear::LogBudget::Log::kLogLast:
			REX::WARN("Equipment: {} for {:08X} refused: {} (the last {} refusal logged this session; net status counts them)", OwnGear::Name(a_change),
				a_ref ? a_ref->GetFormID() : 0, OwnGear::Name(a_verdict), OwnGear::Alarming(a_verdict) ? "alarming" : "benign");
			break;
		}
		return false;
	}

	bool MayChange(RE::TESObjectREFR* a_ref, OwnGear::Change a_change)
	{
		return Count(a_ref, a_change, Judge(a_ref, a_change));
	}

	void OnSessionStart()
	{
		ownGear = {};
		ownGearLog = {};
	}

	std::string DescribeOwnGear()
	{
		using OwnGear::Verdict;
		const auto of = [](Verdict a_verdict) { return ownGear.refused[static_cast<std::size_t>(a_verdict)]; };
		return std::format("ownGear: refused={} ourPlayer={} wrongActor={} notCopy={} ourFrame={} inOurInventory={} inInventory={} notPlaced={} gone={}", ownGear.Refused(),
			ownGear.ourPlayer, of(Verdict::kWrongActor), of(Verdict::kNotCopy), of(Verdict::kOurFrame), of(Verdict::kInOurInventory), of(Verdict::kInInventory),
			of(Verdict::kNotPlaced), of(Verdict::kGone));
	}

	std::string DescribeInventory(RE::Actor* a_actor)
	{
		const auto list = a_actor ? a_actor->inventoryList : nullptr;
		if (!list) {
			return "error: no inventory";
		}
		std::vector<std::pair<std::uint32_t, std::uint32_t>> stacks;  // form, count
		std::uint32_t weapons = 0;
		std::uint32_t armor = 0;
		std::uint32_t pipBoys = 0;
		bool          pipBoyWorn = false;
		std::uint32_t caps = 0;
		{
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& entry : list->data) {
				if (!entry.object) {
					continue;
				}
				const auto id = entry.object->GetFormID();
				const auto count = entry.GetCount();
				stacks.emplace_back(id, count);
				if (entry.object->Is(RE::ENUM_FORM_ID::kWEAP)) {
					weapons += count;
				} else if (entry.object->Is(RE::ENUM_FORM_ID::kARMO)) {
					armor += count;
				}
				if (id == PIP_BOY) {
					pipBoys += count;
					for (auto stack = entry.stackData.get(); stack; stack = stack->nextStack.get()) {
						pipBoyWorn |= stack->IsEquipped();
					}
				}
				if (id == CAPS) {
					caps += count;
				}
			}
		}
		std::ranges::sort(stacks);
		// FNV-1a over every stack, so two looks can be compared at a glance.
		std::uint64_t hash = 14695981039346656037ull;
		std::uint64_t items = 0;
		for (const auto& [id, count] : stacks) {
			for (const auto value : { id, count }) {
				for (int shift = 0; shift < 32; shift += 8) {
					hash = (hash ^ ((value >> shift) & 0xFF)) * 1099511628211ull;
				}
			}
			items += count;
		}
		const auto worn = Read(a_actor);
		std::string out = std::format("stacks={} items={} weapons={} armor={} caps={} pipboy={} pipboyWorn={} hash={:016X} equipped({})=", stacks.size(), items, weapons, armor, caps,
			pipBoys, pipBoyWorn, hash, worn.size());
		for (std::size_t i = 0; i < worn.size(); ++i) {
			out += std::format("{}{:08X}", i ? "," : "", worn[i]);
		}
		return out;
	}

	std::vector<std::uint32_t> Read(RE::Actor* a_actor)
	{
		std::vector<std::uint32_t> items;
		const auto list = a_actor ? a_actor->inventoryList : nullptr;
		if (!list) {
			return items;
		}
		{
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& item : list->data) {
				const auto object = item.object;
				// Runtime-created forms (0xFF......) don't exist in other players' games.
				if (!object || !object->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP) || (object->GetFormID() >> 24) == 0xFF) {
					continue;
				}
				for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
					if (stack->IsEquipped()) {
						items.push_back(object->GetFormID());
						break;
					}
				}
			}
		}
		std::ranges::sort(items);
		items.erase(std::unique(items.begin(), items.end()), items.end());
		if (items.size() > Protocol::MAX_EQUIPMENT) {
			items.resize(Protocol::MAX_EQUIPMENT);
		}
		return items;
	}

	namespace
	{
		// The mods on one copy of an item, as its BGSObjectInstanceExtra lists them. One the engine keeps listed but
		// disabled is left out, and so is a runtime-created form (0xFF......), which no other game has.
		std::vector<Protocol::ItemMod> ModsOn(const RE::BGSObjectInstanceExtra& a_extra)
		{
			std::vector<Protocol::ItemMod> mods;
			if (!a_extra.values) {
				return mods;
			}
			for (const auto& data : a_extra.GetIndexData()) {
				if (data.disabled || data.objectID == 0 || (data.objectID >> 24) == 0xFF) {
					continue;
				}
				mods.push_back({ data.objectID, data.index, data.rank });
			}
			return mods;
		}

		const RE::BGSObjectInstanceExtra* InstanceOf(const RE::BGSInventoryItem::Stack& a_stack)
		{
			return a_stack.extra ? a_stack.extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
		}
	}

	std::vector<Protocol::ItemMods> ReadMods(RE::Actor* a_actor)
	{
		std::vector<Protocol::ItemMods> out;
		const auto items = Read(a_actor);
		const auto list = a_actor ? a_actor->inventoryList : nullptr;
		if (!list || items.empty()) {
			return out;
		}
		RE::BSAutoReadLock l{ list->rwLock };
		for (auto& item : list->data) {
			if (!item.object || !std::ranges::binary_search(items, item.object->GetFormID()) || StandInGear::ModsOf(out, item.object->GetFormID())) {
				continue;
			}
			for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
				if (!stack->IsEquipped()) {
					continue;
				}
				if (const auto instance = InstanceOf(*stack); instance && instance->values) {
					out.push_back({ item.object->GetFormID(), instance->itemIndex, ModsOn(*instance) });
				} else if (const auto armor = item.object->GetFormType() == RE::ENUM_FORM_ID::kARMO ? item.object->As<RE::TESObjectARMO>() : nullptr; armor && !armor->objectTemplate.items.empty()) {
					// A worn piece with combinations but no mods is said plain: said nothing, the other game rolls its own.
					out.push_back({ item.object->GetFormID(), StandInGear::NO_TEMPLATE, {} });
				}
				break;
			}
		}
		return out;
	}

	// Whether the actor carries at least one of the item.
	bool Has(RE::Actor* a_actor, std::uint32_t a_item)
	{
		const auto list = a_actor ? a_actor->inventoryList : nullptr;
		if (!list) {
			return false;
		}
		RE::BSAutoReadLock l{ list->rwLock };
		for (auto& entry : list->data) {
			if (entry.object && entry.object->GetFormID() == a_item) {
				return entry.GetCount() > 0;
			}
		}
		return false;
	}

	namespace
	{
		bool IsThrowable(const RE::TESForm* a_form)
		{
			const auto weapon = a_form && a_form->GetFormType() == RE::ENUM_FORM_ID::kWEAP ? a_form->As<RE::TESObjectWEAP>() : nullptr;
			if (!weapon) {
				return false;
			}
			const auto type = weapon->weaponData.type.get();
			return type == RE::WEAPON_TYPE::kGrenade || type == RE::WEAPON_TYPE::kMine;
		}

		GearPlan::Slots SlotsOf(std::uint32_t a_item)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			if (!form) {
				return 0;
			}
			if (const auto armor = form->GetFormType() == RE::ENUM_FORM_ID::kARMO ? form->As<RE::TESObjectARMO>() : nullptr) {
				return static_cast<const RE::BGSBipedObjectForm*>(armor)->bipedModelData.bipedObjectSlots;
			}
			if (form->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
				return IsThrowable(form) ? GearPlan::THROWABLE_SLOT : GearPlan::WEAPON_SLOT;
			}
			return 0;
		}
	}

	std::uint32_t Wear(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_actor || !MayChange(a_actor, OwnGear::Change::kDressCopy)) {
			return 0;
		}
		const auto id = a_actor->GetFormID();
		const auto has = [&](std::uint32_t a_item) {
			const auto form = RE::TESForm::GetFormByID(a_item);
			return form && form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP) && Has(a_actor, a_item);
		};
		const auto piece = [](std::uint32_t a_item) { return IsPowerArmorPiece(RE::TESForm::GetFormByID(a_item)); };
		const auto equip = GearPlan::Equip(Read(a_actor), a_items, InPowerArmor(a_actor), has, piece, SlotsOf);
		for (const auto item : equip) {
			ClickWatch::Note(ClickRates::Source::kEquip);
			RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", id, item).c_str());
		}
		// The one exception to "nothing is given": a replayed throw needs the grenade in the hand, and spends it.
		if (!InPowerArmor(a_actor)) {
			HoldThrowable(a_actor, a_items);
		}
		return static_cast<std::uint32_t>(equip.size());
	}

	namespace
	{
		// The mods on the actor's copy of an item (the one it wears, else its first; none on a copy without an instance
		// extra), or nullopt when it carries none.
		std::optional<std::vector<Protocol::ItemMod>> CarriedMods(RE::Actor* a_actor, std::uint32_t a_item)
		{
			const auto list = a_actor ? a_actor->inventoryList : nullptr;
			if (!list) {
				return std::nullopt;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& entry : list->data) {
				if (!entry.object || entry.object->GetFormID() != a_item) {
					continue;
				}
				if (entry.GetCount() == 0) {
					return std::nullopt;
				}
				const RE::BGSInventoryItem::Stack* copy = entry.stackData.get();
				for (auto stack = entry.stackData.get(); stack; stack = stack->nextStack.get()) {
					if (stack->IsEquipped()) {
						copy = stack;
						break;
					}
				}
				const auto instance = copy ? InstanceOf(*copy) : nullptr;
				return instance ? ModsOn(*instance) : std::vector<Protocol::ItemMod>{};
			}
			return std::nullopt;
		}

		// Whether this game has the mod: one from a plugin it hasn't is left off the stand-in's copy.
		bool ModHere(std::uint32_t a_mod)
		{
			return RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_mod) != nullptr;
		}

		// Gives the actor one of the item built as their copy is: with the mods of theirs that are here
		// (StandInGear::Buildable), or, none, with the item's default combination. The console's additem has the engine
		// roll the item's object template for the actor's level (a stock 10mm showed up on a friend's stand-in with a
		// long barrel). Here the instance extra is made first, as the engine's own instancing makes it (the mods, then the
		// instance data they make), and the item goes in with it: a copy that comes with its instance extra keeps it, as
		// an item handed to a companion keeps its mods. An item with neither mods nor templates is given by the console
		// as before.
		void Give(RE::Actor* a_actor, std::uint32_t a_item, const Protocol::ItemMods* a_mods)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			RE::TESBoundObject* object = nullptr;
			if (form && form->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
				object = form->As<RE::TESObjectWEAP>();
			} else if (form && form->GetFormType() == RE::ENUM_FORM_ID::kARMO) {
				object = form->As<RE::TESObjectARMO>();
			}
			// Built only with mods that went on and a template combination named: the engine makes instance data from
			// nothing else (BGSObjectInstanceExtra::CreateBaseInstanceData), and an extra without either is let go before
			// it is anywhere.
			std::unique_ptr<RE::BGSObjectInstanceExtra> instance;
			std::size_t                                 attached = 0;
			if (const auto buildable = StandInGear::Buildable(a_mods, ModHere); buildable && object) {
				instance.reset(new RE::BGSObjectInstanceExtra());
				instance->itemIndex = buildable->templateIndex;
				for (const auto& entry : buildable->mods) {
					if (const auto mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.mod)) {
						instance->AddMod(*mod, entry.index, entry.rank, false);
						++attached;
					}
				}
				if (attached == 0 || !instance->values || instance->itemIndex == StandInGear::NO_TEMPLATE) {
					instance.reset();
					attached = 0;
				}
			}
			if (!object || (!instance && object->objectTemplate.items.empty())) {
				RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", a_actor->GetFormID(), a_item).c_str());
				return;
			}
			RE::BSTSmartPointer<RE::ExtraDataList> extra{ new RE::ExtraDataList() };
			if (StandInGear::IsPlain(a_mods)) {
				// Theirs is a plain piece: given as it is, not with the default combination, which may be an armored one here.
			} else if (instance) {
				RE::BSTSmartPointer<RE::TBO_InstanceData> data;
				instance->CreateBaseInstanceData(*object, data);
				extra->AddExtra(instance.release());  // the list owns it from here
				if (data) {
					extra->AddExtra(new RE::ExtraInstanceData(object, data));
				}
			} else {
				RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*object, *extra, nullptr, true);
			}
			a_actor->AddInventoryItem(object, extra, 1, nullptr, nullptr, nullptr);
			REX::INFO("Equipment: {:08X} given to {:08X} with {} ({} of {} found here)", a_item, a_actor->GetFormID(), attached ? "their mods" : "its default mods", attached,
				a_mods ? a_mods->mods.size() : 0);
		}

		// Gives the actor its copy of one of their items as StandInGear::ChooseCopy says, and equips it, unless it wears a
		// copy that will do already. True when a gun in its hand was only taken away, its new copy to be given in a later
		// dressing (StandInGear::TakeAwayFirst).
		bool PutOn(RE::Actor* a_actor, std::uint32_t a_item, const std::vector<Protocol::ItemMods>& a_mods, bool a_wearing)
		{
			const auto theirs = StandInGear::ModsOf(a_mods, a_item);
			// Checked against the mods its copy is built with here: one of theirs this game hasn't never is on it.
			const auto buildable = StandInGear::Buildable(theirs, ModHere);
			const auto copy = StandInGear::ChooseCopy(buildable ? &*buildable : nullptr, CarriedMods(a_actor, a_item), StandInGear::IsPlain(theirs));
			if (a_wearing && copy == StandInGear::Copy::kKeep) {
				return false;
			}
			const auto id = a_actor->GetFormID();
			const auto form = RE::TESForm::GetFormByID(a_item);
			if (StandInGear::TakeAwayFirst(copy, a_wearing, form && form->GetFormType() == RE::ENUM_FORM_ID::kWEAP && !IsThrowable(form))) {
				// The same weapon taken off and put back on in one frame came out in pieces (its parts floating apart round
				// the hand): off now, every copy, and the new one given once the engine has let go of this one's 3D.
				RE::Console::ExecuteCommand(std::format("{:08X}.unequipitem {:08X}", id, a_item).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.removeitem {:08X} {}", id, a_item, Protocol::MAX_ITEM_COUNT).c_str());
				REX::INFO("Equipment: {:08X} on {:08X} carries other mods than theirs; taken away, its new copy given in a moment", a_item, id);
				return true;
			}
			if (copy == StandInGear::Copy::kReplace) {
				// Every copy it has: equipitem would pick any of them.
				RE::Console::ExecuteCommand(std::format("{:08X}.removeitem {:08X} {}", id, a_item, Protocol::MAX_ITEM_COUNT).c_str());
			}
			// Given only when missing (or carrying other mods): every gear report used to add another copy.
			if (copy != StandInGear::Copy::kKeep) {
				Give(a_actor, a_item, theirs);
			}
			ClickWatch::Note(ClickRates::Source::kEquip);
			RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", id, a_item).c_str());
			return false;
		}
	}

	bool WearOnStandIn(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods)
	{
		if (!a_actor || !MayChange(a_actor, OwnGear::Change::kDressStandIn)) {
			return false;
		}
		const auto id = a_actor->GetFormID();
		const auto wearing = Read(a_actor);
		bool       tookAway = false;
		for (const auto item : a_items) {
			const auto form = RE::TESForm::GetFormByID(item);
			if (!form || !form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP)) {
				continue;
			}
			tookAway |= PutOn(a_actor, item, a_mods, std::ranges::binary_search(wearing, item));
		}
		// What it wears that they don't comes off, so the two look the same.
		for (const auto worn : Read(a_actor)) {
			if (!std::ranges::contains(a_items, worn)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.unequipitem {:08X}", id, worn).c_str());
			}
		}
		HoldThrowable(a_actor, a_items);
		return tookAway;
	}

	bool InPowerArmor(const RE::Actor* a_actor)
	{
		return a_actor && a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
	}

	bool IsPowerArmorFrame(const RE::TESForm* a_base)
	{
		const auto furniture = a_base && a_base->GetFormType() == RE::ENUM_FORM_ID::kFURN ? a_base->As<RE::TESFurniture>() : nullptr;
		if (!furniture) {
			return false;
		}
		static REL::Relocation<RE::BGSKeyword* (*)()> furnitureKeyword{ POWER_ARMOR_FURNITURE_KEYWORD };
		const auto keyword = furnitureKeyword();
		return keyword && static_cast<const RE::BGSKeywordForm*>(furniture)->HasKeyword(keyword);
	}

	bool IsFrameItem(const RE::TESForm* a_item)
	{
		const auto armor = a_item && a_item->GetFormType() == RE::ENUM_FORM_ID::kARMO ? a_item->As<RE::TESObjectARMO>() : nullptr;
		if (!armor) {
			return false;
		}
		if (armor == DefaultFrame()) {
			return true;
		}
		const auto keyword = FrameKeyword();
		return keyword && static_cast<const RE::BGSKeywordForm*>(armor)->HasKeyword(keyword);
	}

	bool IsPowerArmorPiece(const RE::TESForm* a_item)
	{
		const auto armor = a_item && a_item->GetFormType() == RE::ENUM_FORM_ID::kARMO ? a_item->As<RE::TESObjectARMO>() : nullptr;
		return armor && armor->HasKeywordString("ArmorTypePower") && !IsFrameItem(armor);
	}

	std::uint32_t FramePick(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0;
		}
		static REL::Relocation<RE::TESObjectARMO* (*)(RE::Actor*, bool)> pick{ POWER_ARMOR_FRAME_PICK };
		const auto frame = pick(a_actor, true);
		return frame ? frame->GetFormID() : 0;
	}

	namespace
	{
		// The keywords of a frame item that make the engine pick it: all its own but the frame keyword and ArmorTypePower,
		// which every frame item has (and every piece the second).
		std::vector<const RE::BGSKeyword*> PickKeywords(const RE::TESObjectARMO* a_frame)
		{
			std::vector<const RE::BGSKeyword*> out;
			const auto frameKeyword = FrameKeyword();
			static_cast<const RE::BGSKeywordForm*>(a_frame)->ForEachKeyword([&](RE::BGSKeyword* a_keyword) {
				if (a_keyword != frameKeyword && a_keyword->formEditorID != "ArmorTypePower") {
					out.push_back(a_keyword);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out;
		}

		// Armor that makes the engine pick the frame item for an actor wearing it: neither a frame item nor a piece (whose
		// ArmorTypePower would have it pick the first frame item of all), with one of the frame item's own keywords.
		bool PicksFrame(const RE::TESForm* a_form, const std::vector<const RE::BGSKeyword*>& a_keywords)
		{
			const auto armor = a_form && a_form->GetFormType() == RE::ENUM_FORM_ID::kARMO ? a_form->As<RE::TESObjectARMO>() : nullptr;
			if (!armor || IsFrameItem(armor) || armor->HasKeywordString("ArmorTypePower") || (armor->GetFormID() >> 24) == 0xFF) {
				return false;
			}
			const auto keywords = static_cast<const RE::BGSKeywordForm*>(armor);
			return std::ranges::any_of(a_keywords, [&](const RE::BGSKeyword* a_keyword) { return keywords->HasKeyword(a_keyword); });
		}
	}

	std::uint32_t FrameKey(std::uint32_t a_frame, const std::vector<std::uint32_t>& a_prefer)
	{
		const auto frame = RE::TESForm::GetFormByID<RE::TESObjectARMO>(a_frame);
		if (!frame || !IsFrameItem(frame)) {
			return 0;
		}
		const auto keywords = PickKeywords(frame);
		if (keywords.empty()) {
			return 0;
		}
		for (const auto item : a_prefer) {
			if (PicksFrame(RE::TESForm::GetFormByID(item), keywords)) {
				return item;
			}
		}
		// The first in the game's own list, found once per frame item.
		static std::unordered_map<std::uint32_t, std::uint32_t> found;
		if (const auto it = found.find(a_frame); it != found.end()) {
			return it->second;
		}
		std::uint32_t key = 0;
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			for (const auto armor : data->GetFormArray<RE::TESObjectARMO>()) {
				if (PicksFrame(armor, keywords)) {
					key = armor->GetFormID();
					break;
				}
			}
		}
		found.emplace(a_frame, key);
		return key;
	}

	void PutOnForPick(RE::Actor* a_actor, std::uint32_t a_item)
	{
		if (a_actor && RE::TESForm::GetFormByID(a_item) && MayChange(a_actor, OwnGear::Change::kDressStandIn)) {
			PutOn(a_actor, a_item, {}, std::ranges::binary_search(Read(a_actor), a_item));
		}
	}

	bool WearInArmor(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods)
	{
		if (!a_actor || !MayChange(a_actor, OwnGear::Change::kDressStandIn)) {
			return false;
		}
		const auto id = a_actor->GetFormID();
		const auto pieceOrWeapon = [](const RE::TESForm* a_form) { return a_form && (a_form->Is(RE::ENUM_FORM_ID::kWEAP) || IsPowerArmorPiece(a_form)); };
		// What it has on already is left alone, as in Wear: its gun is not equipped over again on a change that keeps it
		// (StandInGear draws it again only when it changed).
		const auto wearing = Read(a_actor);
		bool       tookAway = false;
		for (const auto item : a_items) {
			if (!pieceOrWeapon(RE::TESForm::GetFormByID(item))) {
				continue;
			}
			tookAway |= PutOn(a_actor, item, a_mods, std::ranges::binary_search(wearing, item));
		}
		for (const auto worn : Read(a_actor)) {
			if (pieceOrWeapon(RE::TESForm::GetFormByID(worn)) && !std::ranges::contains(a_items, worn)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.unequipitem {:08X}", id, worn).c_str());
			}
		}
		HoldThrowable(a_actor, a_items);
		return tookAway;
	}

	RE::NiPointer<RE::TESObjectREFR> PowerArmorFrameOf(RE::Actor* a_actor)
	{
		if (!InPowerArmor(a_actor)) {
			return {};
		}
		static REL::Relocation<bool (*)(RE::ExtraDataList*, RE::ObjectRefHandle&)> powerArmorHandle{ GET_POWER_ARMOR_HANDLE };
		RE::ObjectRefHandle handle;
		if (!powerArmorHandle(a_actor->extraList.get(), handle)) {
			return {};
		}
		return handle.get();
	}

	bool SwitchIntoPowerArmor(RE::Actor* a_actor, RE::TESObjectREFR* a_frame)
	{
		if (!a_actor || !a_frame || InPowerArmor(a_actor) || !IsPowerArmorFrame(a_frame->GetObjectReference())) {
			return false;
		}
		// Only a stand-in, and only into a copy this game placed for it: the switch-in empties the frame and moves its
		// items onto the actor.
		if (!MayChange(a_actor, OwnGear::Change::kDressStandIn) || !MayChange(a_frame, OwnGear::Change::kEmptyFrame)) {
			return false;
		}
		// Empty, so that what goes on is only what the actor is told to wear: a frame never looked into is filled from its
		// base's container as the actor gets in (the engine's own switch-in does so), and those pieces would be moved onto
		// the actor.
		RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", a_frame->GetFormID()).c_str());
		// The actor's own: the engine's switch-in sounds the steal alarm when the frame, or else its cell, has an owner the
		// actor is not. It gives a frame to the NPC getting out of it in the same way.
		static REL::Relocation<void (*)(RE::TESObjectREFR*, RE::TESForm*)> setOwner{ SET_OWNER };
		if (const auto owner = OriginalBase(a_actor)) {
			setOwner(a_frame, owner);
		}
		static REL::Relocation<void (*)(RE::Actor&, RE::TESObjectREFR*, bool)> switchIn{ SWITCH_INTO_POWER_ARMOR };
		switchIn(*a_actor, a_frame, false);
		return PowerArmorFrameOf(a_actor).get() == a_frame;
	}

	bool HasPowerArmorSkeleton(RE::Actor* a_actor)
	{
		const auto root = a_actor ? a_actor->Get3D() : nullptr;
		if (!root) {
			return false;
		}
		static const RE::BSFixedString bone{ POWER_ARMOR_BONE };
		return root->GetObjectByName(bone) != nullptr;
	}

	bool LegRotation(RE::Actor* a_actor, float (&a_out)[9])
	{
		const auto root = a_actor ? a_actor->Get3D() : nullptr;
		static const RE::BSFixedString bone{ LEG_BONE };
		const auto leg = root ? root->GetObjectByName(bone) : nullptr;
		if (!leg) {
			return false;
		}
		const auto& rotate = leg->local.rotate;
		for (int row = 0; row < 3; ++row) {
			a_out[row * 3] = rotate.entry[row].x;
			a_out[row * 3 + 1] = rotate.entry[row].y;
			a_out[row * 3 + 2] = rotate.entry[row].z;
		}
		return true;
	}

	void ReleasePowerArmorFrame(RE::TESObjectREFR* a_frame)
	{
		static REL::Relocation<RE::TESRace* (*)()> powerArmorRace{ POWER_ARMOR_RACE };
		const auto manager = RE::BGSDynamicPersistenceManager::GetSingleton();
		const auto race = powerArmorRace();
		if (a_frame && manager && race) {
			manager->DemoteReference(a_frame, race, false);
		}
	}

	void Apply(RE::Actor* a_puppet, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods)
	{
		// It empties the inventory first: a friend's stand-in only.
		if (!a_puppet || !MayChange(a_puppet, OwnGear::Change::kDressStandIn)) {
			return;
		}
		// The console commands handle inventory bookkeeping and 3D updates for us.
		const auto id = a_puppet->GetFormID();
		RE::Console::ExecuteCommand(std::format("{:08X}.removeallitems", id).c_str());
		for (const auto item : a_items) {
			const auto form = RE::TESForm::GetFormByID(item);
			if (!form || !form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP)) {
				continue;
			}
			Give(a_puppet, item, StandInGear::ModsOf(a_mods, item));
			ClickWatch::Note(ClickRates::Source::kEquip);
			RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", id, item).c_str());
		}
		HoldThrowable(a_puppet, a_items);
	}

	namespace
	{
		RE::TESObjectWEAP* ThrowableForm(std::uint32_t a_item)
		{
			const auto weapon = RE::TESForm::GetFormByID<RE::TESObjectWEAP>(a_item);
			if (!weapon) {
				return nullptr;
			}
			const auto type = weapon->weaponData.type.get();
			return type == RE::WEAPON_TYPE::kGrenade || type == RE::WEAPON_TYPE::kMine ? weapon : nullptr;
		}

		// The engine's own equipped entry for an item (not the inventory's flags, which Read() sees):
		// a throw and the weaponAttach animation event read this list, by equip index.
		struct Held
		{
			std::uint32_t         index;
			RE::TBO_InstanceData* instance;
		};

		std::optional<Held> EngineHolds(RE::Actor* a_actor, std::uint32_t a_item)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return std::nullopt;
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				if (equipped.item.object && equipped.item.object->GetFormID() == a_item) {
					return Held{ equipped.equipIndex.index, equipped.item.instanceData.get() };
				}
			}
			return std::nullopt;
		}

		// The biped slot a throwable's 3D hangs in, which the throw's weaponAttach moves to the hand.
		RE::BIPED_OBJECT BipedSlot(const RE::TESObjectWEAP* a_weapon)
		{
			return a_weapon->weaponData.type.get() == RE::WEAPON_TYPE::kMine ? RE::BIPED_OBJECT::kWeaponMine : RE::BIPED_OBJECT::kWeaponGrenade;
		}

		bool Has3D(RE::Actor* a_actor, const RE::TESObjectWEAP* a_weapon)
		{
			const auto biped = a_actor->biped.get();
			return biped && biped->GetBipObject(BipedSlot(a_weapon))->partClone.get() != nullptr;
		}

		// The equip manager lists a throwable on an NPC but loads no model for it (seen on the rig: the
		// grenade sat at equip index 2 and the throw's weaponAttach(2) came, with the biped's grenade
		// slot empty throughout), so the hand stayed empty. AttachWeapon is the engine's own load-and-hang
		// of one equipped weapon's 3D, the step its equip path skipped. The model comes in through the
		// loader, so the slot fills a moment later; asked again (next gear report or throw) only once
		// that moment has clearly passed, never twice for one load.
		constexpr auto                                                       ATTACH_RETRY = 5s;
		std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> attachAsked;  // actor -> when
		std::uint32_t                                                        attachesAsked = 0;

		void LoadThrowable3D(RE::Actor* a_actor, RE::TESObjectWEAP* a_weapon, const Held& a_held)
		{
			const auto now = std::chrono::steady_clock::now();
			const auto asked = attachAsked.find(a_actor->GetFormID());
			if (Has3D(a_actor, a_weapon) || (asked != attachAsked.end() && now < asked->second + ATTACH_RETRY)) {
				return;
			}
			if (attachAsked.size() > 64) {
				attachAsked.clear();  // stand-ins come and go with new form IDs; a spacing lost is one early retry
			}
			attachAsked[a_actor->GetFormID()] = now;
			++attachesAsked;
			const RE::BGSObjectInstanceT<RE::TESObjectWEAP> instance{ a_weapon, a_held.instance };
			a_actor->AttachWeapon(instance, RE::BGSEquipIndex{ a_held.index });
			REX::INFO("Equipment: throwable {:08X} 3D asked of AttachWeapon on {:08X} (index {}): loaded now={} holds{}", a_weapon->GetFormID(), a_actor->GetFormID(), a_held.index, Has3D(a_actor, a_weapon), DescribeHeld(a_actor));
		}
	}

	bool HoldThrowable(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_actor || !a_actor->Get3D()) {
			return false;
		}
		if (!MayChange(a_actor, Puppets::IsPuppet(a_actor) ? OwnGear::Change::kDressStandIn : OwnGear::Change::kDressCopy)) {
			return false;
		}
		for (const auto item : a_items) {
			const auto weapon = ThrowableForm(item);
			if (!weapon) {
				continue;
			}
			if (!Has(a_actor, item)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", a_actor->GetFormID(), item).c_str());
			}
			if (const auto held = EngineHolds(a_actor, item)) {
				LoadThrowable3D(a_actor, weapon, *held);
				return true;
			}
			const auto manager = RE::ActorEquipManager::GetSingleton();
			if (!manager) {
				return false;
			}
			// Forced, applied now, silently: the stand-in neither chooses nor announces its gear. The
			// weapon's own slot (the grenade slot, read from the form rather than asked of the engine
			// with no instance data) is named, so the throwable goes where the throw looks for it.
			const RE::BGSEquipSlot* slot = weapon->weaponData.equipSlot ? weapon->weaponData.equipSlot : weapon->equipSlot;
			const RE::BGSObjectInstance instance{ weapon, nullptr };
			const bool accepted = manager->EquipObject(a_actor, instance, 0, 1, slot, false, true, false, true, false);
			const auto held = EngineHolds(a_actor, item);
			REX::INFO("Equipment: throwable {:08X} on {:08X} through the equip manager (slot {:08X}): accepted={} held={} holds{}", item, a_actor->GetFormID(), slot ? slot->GetFormID() : 0, accepted, held.has_value(), DescribeHeld(a_actor));
			if (held) {
				LoadThrowable3D(a_actor, weapon, *held);
			}
			return held.has_value();
		}
		return false;
	}

	std::uint32_t AttachesAsked()
	{
		return attachesAsked;
	}

	std::string DescribeHeld(RE::Actor* a_actor)
	{
		std::string out;
		const auto process = a_actor ? a_actor->currentProcess : nullptr;
		if (const auto middle = process ? process->middleHigh : nullptr) {
			RE::BSAutoLock l{ middle->equippedItemsLock };
			static const auto weaponData = RE::VTABLE::EquippedWeaponData[0].address();
			for (const auto& equipped : middle->equippedItems) {
				// An entry with no data (a weapon's holds its sounds, preloads and fire node) was never set up as a weapon.
				out += std::format(" {:08X}@{}{}", equipped.item.object ? equipped.item.object->GetFormID() : 0, equipped.equipIndex.index, equipped.data ? "" : "(nodata)");
				// A weapon's preloads, its model (m) and its projectile (p), are queued as the engine sets the weapon up;
				// what its throw shows in the hand would come from the model one. Read only off data that is a weapon's.
				const auto data = equipped.data.get();
				if (data && *reinterpret_cast<const std::uintptr_t*>(data) == weaponData) {
					const auto weapon = static_cast<const RE::EquippedWeaponData*>(data);
					out += std::format("(pre={}{})", weapon->weaponPreload ? "m" : "-", weapon->projectilePreload ? "p" : "-");
				}
			}
		}
		// A throwable's 3D hangs in the biped's grenade or mine slot once loaded; the throw's
		// weaponAttach event is meant to move it to the hand and show it.
		if (const auto biped = a_actor ? a_actor->biped.get() : nullptr) {
			for (const auto slot : { RE::BIPED_OBJECT::kWeaponGrenade, RE::BIPED_OBJECT::kWeaponMine }) {
				const auto part = biped->GetBipObject(slot)->partClone.get();
				if (!part) {
					continue;
				}
				const auto parent = part->parent;
				out += std::format(" 3d[{}]='{}' culled={} under='{}'", std::to_underlying(slot), part->name.c_str() ? part->name.c_str() : "", part->GetAppCulled(),
					parent && parent->name.c_str() ? parent->name.c_str() : "none");
			}
		}
		return out.empty() ? " nothing" : out;
	}

	RE::TESObjectWEAP* HeldThrowable(RE::Actor* a_actor)
	{
		const auto process = a_actor ? a_actor->currentProcess : nullptr;
		const auto middle = process ? process->middleHigh : nullptr;
		if (!middle) {
			return nullptr;
		}
		RE::BSAutoLock l{ middle->equippedItemsLock };
		for (const auto& equipped : middle->equippedItems) {
			if (const auto weapon = equipped.item.object ? ThrowableForm(equipped.item.object->GetFormID()) : nullptr) {
				return weapon;
			}
		}
		return nullptr;
	}

	RE::NiNode* ChildNode(RE::NiAVObject* a_root, std::string_view a_name)
	{
		const auto found = a_root ? a_root->GetObjectByName(RE::BSFixedString{ a_name }) : nullptr;
		return found ? found->IsNode() : nullptr;
	}

	RE::NiAVObject* DirectChild(RE::NiNode* a_node, std::string_view a_name)
	{
		if (!a_node) {
			return nullptr;
		}
		for (const auto& child : a_node->children) {  // the array runs to its capacity: empty slots are null
			const auto object = child.get();
			if (object && object->name.c_str() && a_name == object->name.c_str()) {
				return object;
			}
		}
		return nullptr;
	}

	std::string DescribeHands(RE::Actor* a_actor)
	{
		const auto root = a_actor ? a_actor->Get3D() : nullptr;
		if (!root) {
			return " no3D";
		}
		std::string out;
		for (const auto name : { "LArm_Hand"sv, "RArm_Hand"sv, "Weapon"sv }) {
			const auto node = ChildNode(root, name);
			if (!node) {
				continue;
			}
			out += std::format(" {}[", name);
			for (const auto& child : node->children) {
				const auto object = child.get();
				if (!object) {
					continue;
				}
				out += std::format("'{}'{}", object->name.c_str() ? object->name.c_str() : "", object->GetAppCulled() ? "(culled)" : "");
				// The engine's own grenade on its own throws: where it sits in the hand and what it holds, for placing ours.
				if (const auto held = object->IsNode(); held && object->name.c_str() && "WeaponLeft"sv == object->name.c_str()) {
					const auto& at = object->local.translate;
					out += std::format("@({:.1f},{:.1f},{:.1f})x{:.2f}{{", at.x, at.y, at.z, object->local.scale);
					for (const auto& inner : held->children) {
						if (inner.get()) {
							out += std::format("'{}' ", inner->name.c_str() ? inner->name.c_str() : "");
						}
					}
					out += "}";
				}
				out += " ";
			}
			out += "]";
		}
		return out.empty() ? " nohands" : out;
	}

	namespace
	{
		FrameGate keepOut = nullptr;  // SetFrameGate

		// The engine's ExtraFlags (EXTRA_DATA_TYPE::kFlags; CommonLibF4 has no class for it): its flags word follows the
		// BSExtraData head. Papyrus's BlockActivation puts the block in it (bit 0, value 1, on 1.11.240: the rig verified it on
		// an engine-placed drop copy and on a blocked frame; Skyrim's ExtraFlags uses 0x2 for kBlockActivate), and TESObjectREFR::Activate then does nothing: no word, no
		// seat. The whole word is shown so a block set another way is seen too.
		struct ExtraFlagsData : RE::BSExtraData
		{
			std::uint32_t flags;  // 18
		};
		constexpr std::uint32_t BLOCK_ACTIVATE = 1u << 0;
	}

	std::uint32_t ExtraFlagsOf(const RE::TESObjectREFR* a_ref)
	{
		const auto extra = a_ref && a_ref->extraList ? a_ref->extraList->GetByType(RE::EXTRA_DATA_TYPE::kFlags) : nullptr;
		return extra ? static_cast<const ExtraFlagsData*>(extra)->flags : 0;
	}

	namespace
	{
		// Whether our player is in furniture, getting in or out of some, or wears a frame.
		bool PlayerBusy(RE::PlayerCharacter* a_player)
		{
			const auto middle = a_player->currentProcess ? a_player->currentProcess->middleHigh : nullptr;
			return static_cast<int>(a_player->DoGetSitSleepState()) != 0 || (middle && middle->occupiedFurniture.get()) || InPowerArmor(a_player);
		}

		// The input enable layers in use (a script's DisablePlayerControls, a conversation's, a menu's, the game's own) that
		// have the Activate user event off, by their debug names: with one, E activates nothing, while the console's activate
		// still does. The session makes no layer of its own. Empty when Activate is on.
		std::string LayersWithActivateOff()
		{
			std::string out;
			const auto  enable = RE::BSInputEnableManager::GetSingleton();
			if (!enable) {
				return out;
			}
			// The engine grows the layers under this lock as a script makes one (InputEnableLayer.Create, on the VM's
			// thread): read under it too, or the walk could read a buffer just let go.
			RE::BSAutoLock l{ enable->layerLock };
			for (std::uint32_t i = 0; i < enable->layers.size(); ++i) {
				const auto name = i < enable->debugNames.size() ? enable->debugNames[i].c_str() : "";
				if (!name || !*name || enable->layers[i].inputUserEvents.any(RE::UserEvents::USER_EVENT_FLAG::kActivate)) {
					continue;  // a free slot, or one that leaves Activate on
				}
				out += std::format("{}{}'{}'", out.empty() ? "" : ",", i, name);
			}
			return out;
		}

		// What else holds our player off the frame (FrameUse::Holds): the gate's answer (asked to look only: nothing is
		// told), the frame's block and state, and our player's input and state.
		FrameUse::Holds HoldsOf(RE::TESObjectREFR* a_frame)
		{
			FrameUse::Holds holds;
			holds.keptOut = keepOut && keepOut(a_frame, FrameAsk::kLook, nullptr);
			holds.activationBlocked = (ExtraFlagsOf(a_frame) & BLOCK_ACTIVATE) != 0;
			holds.disabled = a_frame->IsDisabled();
			holds.no3d = a_frame->Get3D() == nullptr;
			holds.activateOff = !LayersWithActivateOff().empty();
			const auto controls = RE::PlayerControls::GetSingleton();
			holds.inputBlocked = controls && controls->blockPlayerInput;
			const auto player = RE::PlayerCharacter::GetSingleton();
			holds.playerBusy = player && PlayerBusy(player);
			return holds;
		}
	}

	FrameUse::Facts FrameUseOf(RE::TESObjectREFR* a_frame)
	{
		FrameUse::Facts facts;
		const auto      base = a_frame ? a_frame->GetObjectReference() : nullptr;
		const auto      furniture = base && base->GetFormType() == RE::ENUM_FORM_ID::kFURN ? base->As<RE::TESFurniture>() : nullptr;
		const auto      player = RE::PlayerCharacter::GetSingleton();
		if (!furniture || !player) {
			return facts;
		}
		static REL::Relocation<float*> upMin{ UPRIGHT_ANGLE_COSINE };
		facts.upMin = *upMin;
		facts.playerBusy = PlayerBusy(player);
		// TESObjectREFR::GetOrientation builds a non-actor's from its angle (NiMatrix3::FromEulerAnglesXYZ), and the
		// engine compares the z of its up axis: cos x times cos y.
		facts.up = std::cos(a_frame->data.angle.x) * std::cos(a_frame->data.angle.y);
		static REL::Relocation<bool (*)(const RE::TESFurniture*, std::uint32_t)>                          markerEnabled{ GET_MARKER_ENABLED };
		static REL::Relocation<RE::ActorHandle* (*)(RE::TESObjectREFR*, RE::ActorHandle*, std::uint32_t, bool)> markerUser{ GET_MARKER_USER };
		for (std::uint32_t i = 0; i < FURNITURE_MARKERS; ++i) {
			if (!markerEnabled(furniture, i)) {
				continue;
			}
			++facts.markers;
			RE::ActorHandle handle;
			const auto      user = markerUser(a_frame, &handle, i, true)->get();
			if (!user) {
				continue;
			}
			if (facts.used++ == 0) {
				const auto middle = user->currentProcess ? user->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				facts.user = user->GetFormID();
				facts.userMarker = static_cast<int>(i);
				facts.userIsPlayer = user.get() == player;
				facts.userIn = (occupied && occupied.get() == a_frame) || PowerArmorFrameOf(user.get()).get() == a_frame;
			}
		}
		static REL::Relocation<bool (*)(RE::TESObjectREFR*, RE::Actor*, bool)> usersOtherThan{ HAS_USERS_OTHER_THAN };
		static REL::Relocation<bool (*)(RE::TESObjectREFR*, bool)>             freeMarker{ HAS_FREE_MARKER };
		facts.othersThanPlayer = usersOtherThan(a_frame, player, false);
		facts.free = freeMarker(a_frame, true);
		return facts;
	}

	std::string DescribeFrameUse(RE::TESObjectREFR* a_frame)
	{
		if (!a_frame) {
			return "none";
		}
		const auto facts = FrameUseOf(a_frame);
		const auto holds = HoldsOf(a_frame);
		const auto base = a_frame->GetObjectReference();
		const auto root = a_frame->Get3D();
		const auto owner = a_frame->GetOwner();
		return std::format("{:08X} base={:08X} {}: up={:.2f} (min {:.2f}) markers={} used={} user={:08X}{}{} othersThanPlayer={} free={} disabled={} 3d={} culled={} flags={:X} ownedBy={:08X} steal={}; {}",
			a_frame->GetFormID(), base ? base->GetFormID() : 0, FrameUse::Name(FrameUse::Judge(facts)), facts.up, facts.upMin, facts.markers, facts.used, facts.user, facts.userIsPlayer ? "(player)" : "",
			facts.used ? (facts.userIn ? "(in it)" : "(not in it)") : "", facts.othersThanPlayer, facts.free, a_frame->IsDisabled(), root != nullptr, root && root->GetAppCulled(), ExtraFlagsOf(a_frame),
			owner ? owner->GetFormID() : 0, a_frame->IsCrimeToActivate(), FrameUse::Describe(holds));
	}

	std::string DescribePlayerUse()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "no player";
		}
		const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
		const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
		const auto worn = PowerArmorFrameOf(player);
		const auto last = player->lastUsedPowerArmor.get();
		const auto controls = RE::PlayerControls::GetSingleton();
		const auto layersOff = LayersWithActivateOff();
		return std::format("our player: sitState={} occupied={:08X} inPA={} wornFrame={:08X} lastFrame={:08X} activateInput={}{} inputBlocked={}", static_cast<int>(player->DoGetSitSleepState()),
			occupied ? occupied->GetFormID() : 0, InPowerArmor(player), worn ? worn->GetFormID() : 0, last ? last->GetFormID() : 0, layersOff.empty() ? "on" : "off by layers ", layersOff,
			controls && controls->blockPlayerInput);
	}

	namespace
	{
		constexpr REL::ID SET_MARKER_USED{ 2202567 };         // TESObjectREFR::SetMarkerUsed(uint, Actor&, bool used, bool force) (+55EB70)
		constexpr REL::ID FURNITURE_ACTIVATE{ 2198054 };      // TESFurniture::Activate (+44CA40)
		constexpr REL::ID PLAYER_USE_FURNITURE{ 2232956 };    // the player's furniture use, which asks for a free marker (+D5D6B0)
		constexpr REL::ID PLAYER_ENTER_FURNITURE{ 2233039 };  // the player's furniture enter, which picks the animation (+D69B80)
		constexpr REL::ID SHOW_HUD_MESSAGE{ 2222440 };        // the HUD's corner message (+A37270)
		constexpr std::size_t CALL_STUB = 14;                 // what a write_call<5> takes of the trampoline (one per target)

		using UseFn = bool (*)(RE::PlayerCharacter*, RE::TESObjectREFR*);
		using MessageFn = void (*)(const char*, const char*, bool, bool);
		UseFn                   originalUse = nullptr;
		MessageFn               originalMessage = nullptr;
		std::uint32_t           seatsFreed = 0;
		std::uint32_t           framesStoodUp = 0;
		std::uint32_t           refusals = 0;
		std::uint32_t           keptOut = 0;          // our player kept out of a frame by the gate
		std::uint32_t           transfersKeptOut = 0; // a frame's inventory closed again by the gate
		std::uint32_t           enterRefusals = 0;
		std::uint32_t           lastFrame = 0;  // the frame our player last asked to get into, for the enter's message
		constexpr std::uint32_t MAX_REFUSALS_LOGGED = 20;

		// TESFurniture::Activate let our player at a frame (nobody else uses it) and asks the engine to seat them: what
		// would turn them away is mended first (FrameUse::ChooseMend), and a refusal is logged with what the engine found.
		bool UseHook(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_ref)
		{
			const bool frame = a_player && a_ref && IsPowerArmorFrame(a_ref->GetObjectReference());
			if (frame && keepOut && keepOut(a_ref, FrameAsk::kEnter, nullptr)) {
				// Not seated, and no word from the engine: Activate returns true whatever the use answers (FrameUse.h).
				++keptOut;
				return false;
			}
			if (frame) {
				lastFrame = a_ref->GetFormID();
				const auto facts = FrameUseOf(a_ref);
				const auto mend = FrameUse::ChooseMend(facts);
				if (mend.Any()) {
					REX::INFO("Equipment: our player can't get into frame {} as it is; {}{}", DescribeFrameUse(a_ref), mend.standUp ? "standing it up" : "",
						mend.freeSeat ? (mend.standUp ? " and giving back the seat the engine still has as theirs" : "giving back the seat the engine still has as theirs") : "");
				}
				if (mend.standUp) {
					a_ref->data.angle.x = 0.0f;
					a_ref->data.angle.y = 0.0f;
					a_ref->Update3DPosition(true);
					++framesStoodUp;
				}
				if (mend.freeSeat && facts.userMarker >= 0) {
					static REL::Relocation<void (*)(RE::TESObjectREFR*, std::uint32_t, RE::Actor&, bool, bool)> setMarkerUsed{ SET_MARKER_USED };
					setMarkerUsed(a_ref, static_cast<std::uint32_t>(facts.userMarker), *a_player, false, true);
					++seatsFreed;
				}
			}
			const bool seated = originalUse(a_player, a_ref);
			if (frame && !seated && refusals++ < MAX_REFUSALS_LOGGED) {
				REX::INFO("Equipment: the engine did not let our player into frame {}; {}", DescribeFrameUse(a_ref), DescribePlayerUse());
			}
			return seated;
		}

		// The player's furniture enter found no animation to get them in (FrameUse.h): what it says, and the frame they
		// last asked for.
		void EnterMessageHook(const char* a_text, const char* a_sound, bool a_throttle, bool a_warning)
		{
			if (enterRefusals++ < MAX_REFUSALS_LOGGED) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto race = player ? player->race : nullptr;
				const auto frame = lastFrame ? RE::TESForm::GetFormByID(lastFrame) : nullptr;
				const auto ref = frame ? frame->As<RE::TESObjectREFR>() : nullptr;
				REX::INFO("Equipment: no animation gets our player into furniture (\"{}\"); race {:08X}, inPA={}, last frame {}", a_text ? a_text : "", race ? race->GetFormID() : 0,
					player && InPowerArmor(player), ref ? DescribeFrameUse(ref) : "none");
			}
			originalMessage(a_text, a_sound, a_throttle, a_warning);
		}

		// The call at a_function + a_offset, if it calls a_target as 1.11.240 has it.
		std::optional<std::uintptr_t> CallAt(const REL::ID& a_function, std::size_t a_offset, const REL::ID& a_target)
		{
			const auto                  site = a_function.address() + a_offset;
			std::array<std::uint8_t, 5> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
			if (TextureFlush::CallTarget(bytes, site) != a_target.address()) {
				return std::nullopt;
			}
			return site;
		}

		// Our player opened a frame's inventory (Transfer): the container menu's own messages are hooked in its vtable
		// (the menu is never looked up through the UI: OpenMenus). As it is shown, on the UI's thread, a copy our player is
		// kept out of (by its handle, from the main thread's last word: SetTransfersKeptOut) has the menu closed at once,
		// in the same pass of the UI's messages, before anything can be moved into it: items left in a copy go with it.
		// Its target is asked of the gate on the main thread too, which tells our player whose it is and closes a menu the
		// UI's thread did not (a copy placed that very frame).
		constexpr std::size_t PROCESS_MESSAGE_INDEX = 0x03;
		using ProcessMessageFn = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
		ProcessMessageFn                 originalContainerMessage = nullptr;
		std::mutex                       keptOutLock;
		std::vector<RE::ObjectRefHandle> transfersKeptOutNow;  // the copies our player is kept out of
		std::atomic<std::uint32_t>       transfersClosedAtShow{ 0 };

		bool KeptOutNow(const RE::ObjectRefHandle& a_target)
		{
			std::scoped_lock l{ keptOutLock };
			return static_cast<bool>(a_target) && std::ranges::find(transfersKeptOutNow, a_target) != transfersKeptOutNow.end();
		}

		void HideContainerMenu()
		{
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::ContainerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
			}
		}

		// Closed once the UI may be written to (Hud::IsSettled), asked again each frame for a while, while it is open.
		void CloseTransfer(std::uint32_t a_tries)
		{
			if (!OpenMenus::Open(OpenMenus::Menu::kContainer)) {
				return;
			}
			if (!Hud::IsSettled()) {
				if (a_tries < 120) {
					MainThread::Post([a_tries] { CloseTransfer(a_tries + 1); });
				}
				return;
			}
			HideContainerMenu();
		}

		void CheckTransfer(RE::ObjectRefHandle a_target, bool a_closed)
		{
			const auto ref = a_target.get();
			if (!ref || !keepOut || !IsPowerArmorFrame(ref->GetObjectReference()) || !keepOut(ref.get(), FrameAsk::kTransfer, nullptr)) {
				return;
			}
			++transfersKeptOut;
			if (!a_closed) {
				CloseTransfer(0);
			}
		}

		RE::UI_MESSAGE_RESULTS ContainerMessageHook(RE::IMenu* a_menu, RE::UIMessage& a_message)
		{
			const auto result = originalContainerMessage(a_menu, a_message);
			if (a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
				const auto target = static_cast<RE::ContainerMenu*>(a_menu)->containerRef;
				// Only the handle is compared here: no game object is read off the main thread.
				const bool closed = KeptOutNow(target) && Hud::IsSettled();
				if (closed) {
					HideContainerMenu();
					++transfersClosedAtShow;
				}
				MainThread::Post([target, closed] { CheckTransfer(target, closed); });
				if (!closed) {
					Storage::OnMenuShown(target);  // a friend's storage: its true copy shown in ours
					Gifts::OnMenuShown(target);    // the gifts lying in it shown in our copy
				}
			}
			return result;
		}
	}

	void InstallFrameGate()
	{
		if (originalContainerMessage) {
			return;
		}
		const auto slot = RE::VTABLE::ContainerMenu[0].address() + sizeof(void*) * PROCESS_MESSAGE_INDEX;
		originalContainerMessage = *reinterpret_cast<ProcessMessageFn*>(slot);
		REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&ContainerMessageHook));
		REX::INFO("Equipment: the container menu watched (a friend's frame's Transfer)");
	}

	void InstallFrameUse()
	{
		if (originalUse) {
			return;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < 2 * CALL_STUB) {
			REX::WARN("Equipment: no room to watch power armor entry ({} bytes free); left as it is", trampoline.free_size());
			return;
		}
		if (const auto site = CallAt(FURNITURE_ACTIVATE, FrameUse::USE_CALL, PLAYER_USE_FURNITURE)) {
			originalUse = reinterpret_cast<UseFn>(trampoline.write_call<5>(*site, &UseHook));
		} else {
			REX::WARN("Equipment: TESFurniture::Activate doesn't call the player's furniture use where 1.11.240 does (another plugin's patch?); frames are not mended");
		}
		if (const auto site = CallAt(PLAYER_ENTER_FURNITURE, FrameUse::ENTER_MESSAGE_CALL, SHOW_HUD_MESSAGE)) {
			originalMessage = reinterpret_cast<MessageFn>(trampoline.write_call<5>(*site, &EnterMessageHook));
		} else {
			REX::WARN("Equipment: the player's furniture enter doesn't show its message where 1.11.240 does; its refusals are not logged");
		}
		REX::INFO("Equipment: power armor entry watched (use {}, enter message {})", originalUse != nullptr, originalMessage != nullptr);
	}

	void SetFrameGate(FrameGate a_gate)
	{
		keepOut = a_gate;
	}

	void SetTransfersKeptOut(std::vector<RE::ObjectRefHandle> a_handles)
	{
		std::scoped_lock l{ keptOutLock };
		transfersKeptOutNow = std::move(a_handles);
	}

	std::string DescribeFrameMends()
	{
		return std::format("seatsFreed={} framesStoodUp={} refusals={} enterRefusals={} keptOut={} transfersKeptOut={} transfersClosedAtShow={}", seatsFreed, framesStoodUp,
			refusals, enterRefusals, keptOut, transfersKeptOut, transfersClosedAtShow.load());
	}
}
