#pragma once

#include <cstdint>
#include <optional>

// A reference's inventory, stack by stack: found, and removed there and then with the engine's own RemoveItem. Main thread
// only, as every change to an inventory.
//
// Not the console's removeallitems to empty a reference that something is put into right after: by all the rig shows it
// empties the reference a moment after it is called, as the console's disable does (HavokGuard), so what went in right
// after it went with it. A friend's lent frame came to our player empty that way (FrameLoans' OnFrameLent).
namespace InventoryStacks
{
	// One stack of a reference's inventory.
	struct Stack
	{
		RE::TESBoundObject* object = nullptr;
		std::uint32_t       stack = 0;  // its number among its item's stacks, as RemoveItem takes it
		std::int32_t        count = 0;
	};

	constexpr int MAX_STACKS = 64;  // stacks looked at once each at most, in case one does not go

	// The first stack in a_ref's inventory that a_match takes (given its base and extra data). None when a_ref has no
	// inventory list yet (a reference never looked into, which the engine fills from its base's container the first time
	// it is read).
	template <class F>
	std::optional<Stack> FindStack(RE::TESObjectREFR* a_ref, F&& a_match)
	{
		const auto list = a_ref ? a_ref->inventoryList : nullptr;
		if (!list) {
			return std::nullopt;
		}
		RE::BSAutoReadLock l{ list->rwLock };
		for (auto& item : list->data) {
			if (!item.object) {
				continue;
			}
			std::uint32_t index = 0;
			for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get(), ++index) {
				if (a_match(item.object, stack->extra.get())) {
					return Stack{ .object = item.object, .stack = index, .count = static_cast<std::int32_t>(stack->count) };
				}
			}
		}
		return std::nullopt;
	}

	// That stack out of a_ref: gone, or into a_to.
	void RemoveStack(RE::TESObjectREFR* a_ref, const Stack& a_stack, RE::TESObjectREFR* a_to = nullptr);

	// Every stack in a_ref that a_keep does not take (given its base and extra data) removed, there and then. How many
	// stacks went.
	template <class F>
	std::uint32_t RemoveAllBut(RE::TESObjectREFR* a_ref, F&& a_keep)
	{
		std::uint32_t removed = 0;
		for (int i = 0; i < MAX_STACKS; ++i) {
			const auto found = FindStack(a_ref, [&](RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra) { return !a_keep(a_object, a_extra); });
			if (!found) {
				return removed;
			}
			RemoveStack(a_ref, *found);
			++removed;
		}
		REX::WARN("InventoryStacks: {:08X} still holds stacks after {} were removed", a_ref->GetFormID(), removed);
		return removed;
	}

	// A stack a quest holds (it fills a quest's alias): never taken by the session's emptying.
	inline bool QuestItem(const RE::ExtraDataList* a_extra)
	{
		return a_extra && a_extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
	}
}
