#include "game/InventoryStacks.h"

namespace InventoryStacks
{
	void RemoveStack(RE::TESObjectREFR* a_ref, const Stack& a_stack, RE::TESObjectREFR* a_to)
	{
		RE::TESObjectREFR::RemoveItemData data{ a_stack.object, a_stack.count };
		data.stackData.push_back(a_stack.stack);
		if (a_to) {
			data.otherContainer = a_to;
			data.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
		}
		a_ref->RemoveItem(data);
	}
}
