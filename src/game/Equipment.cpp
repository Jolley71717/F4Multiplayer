#include "game/Equipment.h"

#include "Protocol.h"
#include "game/GearPlan.h"
#include "game/StandInGear.h"

namespace Equipment
{
	namespace
	{
		constexpr REL::ID POWER_ARMOR_FURNITURE_KEYWORD{ 2194741 };  // PowerArmor::GetFurnitureKeyword
		constexpr REL::ID POWER_ARMOR_FRAME_KEYWORD{ 2194744 };      // PowerArmor::GetFrameKeyword (+374890)
		constexpr REL::ID POWER_ARMOR_DEFAULT_FRAME{ 2194757 };      // PowerArmor::GetDefaultFrame (+374AB0)
		constexpr REL::ID POWER_ARMOR_RACE{ 2194755 };               // PowerArmor::GetPowerArmorRace (+374A70)
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
		// A bone the power armor skeleton has and the human one has not (Meshes\Actors\PowerArmor\CharacterAssets\
		// skeleton.nif against Meshes\Actors\Character\CharacterAssets\skeleton.nif in Fallout4 - Meshes.ba2: the armor
		// bones, Pelvis_Armor, Helmet_Armor, Tank_Armor and the rest, are the power armor skeleton's alone; the human one
		// only has P-Armor connect points, which are no nodes).
		constexpr const char* POWER_ARMOR_BONE = "Pelvis_Armor";

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
		if (!a_actor) {
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

		// Gives the actor one of the item built as their copy is: with these mods, or, none on record, with the item's
		// default combination. The console's additem has the engine roll the item's object template for the actor's
		// level (a stock 10mm showed up on a friend's stand-in with a long barrel). Here the instance extra is made
		// first, as the engine's own instancing makes it (the mods, then the instance data they make), and the item goes
		// in with it: a copy that comes with its instance extra keeps it, as an item handed to a companion keeps its
		// mods. An item with neither mods nor templates is given by the console as before.
		void Give(RE::Actor* a_actor, std::uint32_t a_item, const Protocol::ItemMods* a_mods)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			RE::TESBoundObject* object = nullptr;
			if (form && form->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
				object = form->As<RE::TESObjectWEAP>();
			} else if (form && form->GetFormType() == RE::ENUM_FORM_ID::kARMO) {
				object = form->As<RE::TESObjectARMO>();
			}
			if (!object || (!a_mods && object->objectTemplate.items.empty())) {
				RE::Console::ExecuteCommand(std::format("{:08X}.additem {:08X} 1", a_actor->GetFormID(), a_item).c_str());
				return;
			}
			RE::BSTSmartPointer<RE::ExtraDataList> extra{ new RE::ExtraDataList() };
			std::size_t                            attached = 0;
			if (a_mods) {
				const auto instance = new RE::BGSObjectInstanceExtra();
				instance->itemIndex = a_mods->templateIndex;
				for (const auto& entry : a_mods->mods) {
					// A mod from a plugin this game hasn't is left off; the rest still go on.
					if (const auto mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.mod)) {
						instance->AddMod(*mod, entry.index, entry.rank, false);
						++attached;
					}
				}
				extra->AddExtra(instance);
				RE::BSTSmartPointer<RE::TBO_InstanceData> data;
				object->ApplyMods(data, instance);
				if (data) {
					extra->AddExtra(new RE::ExtraInstanceData(object, data));
				}
			} else {
				RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*object, *extra, nullptr, true);
			}
			a_actor->AddInventoryItem(object, extra, 1, nullptr, nullptr, nullptr);
			REX::INFO("Equipment: {:08X} given to {:08X} with {} ({} of {} found here)", a_item, a_actor->GetFormID(), a_mods ? "their mods" : "its default mods", attached,
				a_mods ? a_mods->mods.size() : 0);
		}

		// Gives the actor its copy of one of their items as StandInGear::ChooseCopy says, and equips it, unless it wears a
		// copy that will do already.
		void PutOn(RE::Actor* a_actor, std::uint32_t a_item, const std::vector<Protocol::ItemMods>& a_mods, bool a_wearing)
		{
			const auto theirs = StandInGear::ModsOf(a_mods, a_item);
			const auto copy = StandInGear::ChooseCopy(theirs, CarriedMods(a_actor, a_item));
			if (a_wearing && copy == StandInGear::Copy::kKeep) {
				return;
			}
			const auto id = a_actor->GetFormID();
			if (copy == StandInGear::Copy::kReplace) {
				// Every copy it has: equipitem would pick any of them.
				RE::Console::ExecuteCommand(std::format("{:08X}.removeitem {:08X} {}", id, a_item, Protocol::MAX_ITEM_COUNT).c_str());
			}
			// Given only when missing (or carrying other mods): every gear report used to add another copy.
			if (copy != StandInGear::Copy::kKeep) {
				Give(a_actor, a_item, theirs);
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.equipitem {:08X}", id, a_item).c_str());
		}
	}

	void WearOnStandIn(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods)
	{
		if (!a_actor) {
			return;
		}
		const auto id = a_actor->GetFormID();
		const auto wearing = Read(a_actor);
		for (const auto item : a_items) {
			const auto form = RE::TESForm::GetFormByID(item);
			if (!form || !form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP)) {
				continue;
			}
			PutOn(a_actor, item, a_mods, std::ranges::binary_search(wearing, item));
		}
		// What it wears that they don't comes off, so the two look the same.
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

	void WearInArmor(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods)
	{
		if (!a_actor) {
			return;
		}
		const auto id = a_actor->GetFormID();
		const auto pieceOrWeapon = [](const RE::TESForm* a_form) { return a_form && (a_form->Is(RE::ENUM_FORM_ID::kWEAP) || IsPowerArmorPiece(a_form)); };
		// What it has on already is left alone, as in Wear: its gun is not equipped over again on a change that keeps it
		// (StandInGear draws it again only when it changed).
		const auto wearing = Read(a_actor);
		for (const auto item : a_items) {
			if (!pieceOrWeapon(RE::TESForm::GetFormByID(item))) {
				continue;
			}
			PutOn(a_actor, item, a_mods, std::ranges::binary_search(wearing, item));
		}
		for (const auto worn : Read(a_actor)) {
			if (pieceOrWeapon(RE::TESForm::GetFormByID(worn)) && !std::ranges::contains(a_items, worn)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.unequipitem {:08X}", id, worn).c_str());
			}
		}
		HoldThrowable(a_actor, a_items);
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
			Give(a_puppet, item, StandInGear::ModsOf(a_mods, item));
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
