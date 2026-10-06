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
	}
}
