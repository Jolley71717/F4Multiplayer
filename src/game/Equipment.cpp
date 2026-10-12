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

	bool InPowerArmor(const RE::Actor* a_actor)
	{
		return a_actor && a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
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
			for (const auto& equipped : middle->equippedItems) {
				// An entry with no data (a weapon's holds its sounds, preloads and fire node) was never set up as a weapon.
				out += std::format(" {:08X}@{}{}", equipped.item.object ? equipped.item.object->GetFormID() : 0, equipped.equipIndex.index, equipped.data ? "" : "(nodata)");
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
