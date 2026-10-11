#include "game/Equipment.h"

#include "Protocol.h"

namespace Equipment
{
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

	void Wear(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_actor) {
			return;
		}
		const auto id = a_actor->GetFormID();
		for (const auto item : a_items) {
			const auto form = RE::TESForm::GetFormByID(item);
			if (!form || !form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP)) {
				continue;
			}
			// Given only when missing: every gear report used to add another copy, so an NPC's
			// inventory grew with each one (and unique items were duplicated as loot).
			if (!Has(a_actor, item)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", id, item).c_str());
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", id, item).c_str());
		}
		// What it wears that the owner's doesn't (its own roll from the outfit) comes off, so the two
		// look the same; it stays in the inventory as loot.
		for (const auto worn : Read(a_actor)) {
			if (!std::ranges::contains(a_items, worn)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.unequipitem {:08X}", id, worn).c_str());
			}
		}
		HoldThrowable(a_actor, a_items);
	}

	void Apply(RE::Actor* a_puppet, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_puppet) {
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
			RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", id, item).c_str());
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

		// Whether the engine's own equipped list (not the inventory's flags, which Read() sees) holds
		// the item: a throw and the weaponAttach animation event read this list.
		bool EngineHolds(RE::Actor* a_actor, std::uint32_t a_item)
		{
			const auto process = a_actor->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return false;
			}
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				if (equipped.item.object && equipped.item.object->GetFormID() == a_item) {
					return true;
				}
			}
			return false;
		}
	}

	bool HoldThrowable(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items)
	{
		if (!a_actor || !a_actor->Get3D()) {
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
			if (EngineHolds(a_actor, item)) {
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
			const bool held = EngineHolds(a_actor, item);
			REX::INFO("Equipment: throwable {:08X} on {:08X} through the equip manager (slot {:08X}): accepted={} held={} holds{}", item, a_actor->GetFormID(), slot ? slot->GetFormID() : 0, accepted, held, DescribeHeld(a_actor));
			return held;
		}
		return false;
	}

	std::string DescribeHeld(RE::Actor* a_actor)
	{
		std::string out;
		const auto process = a_actor ? a_actor->currentProcess : nullptr;
		if (const auto middle = process ? process->middleHigh : nullptr) {
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				out += std::format(" {:08X}@{}", equipped.item.object ? equipped.item.object->GetFormID() : 0, equipped.equipIndex.index);
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
}
