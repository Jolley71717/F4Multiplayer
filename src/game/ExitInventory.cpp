#include "game/ExitInventory.h"

#include "game/TextureFlush.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>

namespace ExitInventory
{
	namespace
	{
		// 1.11.240, by Address Library ID (ExitInventory.h: how each was read).
		constexpr REL::ID     LIST_DESTRUCTOR{ 2194154 };     // ~BGSInventoryList (+34D730)
		constexpr REL::ID     REMOVE_ALL_AGENTS{ 2194033 };   // BGSInventoryInterface::RemoveAllAgents (+3486A0): (this, const BSPointerHandle* owner)
		constexpr REL::ID     INTERFACE_SINGLETON{ 4796586 };  // BGSInventoryInterface* (+30EC470): null once the shutdown deleted it
		constexpr std::size_t LOAD_SINGLETON = 0x7C;           // mov rcx,[rip+disp32] (48 8B 0D) of the singleton, in the destructor
		constexpr std::size_t CALL = 0x87;                     // call RemoveAllAgents (E8 rel32), the one call guarded
		constexpr std::size_t CALL_STUB = 14;                  // what a write_call<5> takes of the trampoline

		using RemoveFn = void (*)(void*, const std::uint32_t*);
		RemoveFn originalRemove = nullptr;

		std::atomic<bool>          installed{ false };
		std::atomic<std::uint32_t> skipped{ 0 };

		// The destructor runs on whichever thread destroys a reference with an inventory (the main thread at the shutdown,
		// a cell's unload otherwise). With the interface there it is the engine's own call; with it gone there is nothing
		// to drop the agents from.
		void RemoveHook(void* a_interface, const std::uint32_t* a_owner)
		{
			if (a_interface) {
				originalRemove(a_interface, a_owner);
				return;
			}
			if (skipped.fetch_add(1, std::memory_order_relaxed) == 0) {
				REX::INFO("ExitInventory: an inventory list was destroyed after the inventory interface was gone (the game is shutting down); its agents are left, not read through null");
			}
		}

		// Where `mov rcx,[rip+disp32]` (48 8B 0D) at a_at reads from, or nothing when the bytes are something else.
		std::optional<std::uintptr_t> LoadSource(std::uintptr_t a_at)
		{
			std::array<std::uint8_t, 7> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(a_at), bytes.size());
			if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x0D) {
				return std::nullopt;
			}
			std::int32_t rel = 0;
			std::memcpy(&rel, bytes.data() + 3, sizeof(rel));
			return a_at + bytes.size() + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("ExitInventory: no room for the hook ({} bytes free); quitting from workshop mode can still crash at the end of the shutdown", trampoline.free_size());
			return false;
		}
		const auto destructor = LIST_DESTRUCTOR.address();
		if (LoadSource(destructor + LOAD_SINGLETON) != INTERFACE_SINGLETON.address()) {
			REX::WARN("ExitInventory: the inventory list's destructor doesn't load the inventory interface where 1.11.240 does (another plugin's patch?); left as it is");
			return false;
		}
		const auto                  site = destructor + CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != REMOVE_ALL_AGENTS.address()) {
			REX::WARN("ExitInventory: the inventory list's destructor doesn't call RemoveAllAgents where 1.11.240 does (another plugin's patch?); left as it is");
			return false;
		}
		originalRemove = reinterpret_cast<RemoveFn>(trampoline.write_call<5>(site, &RemoveHook));
		installed = true;
		REX::INFO("ExitInventory: an inventory list destroyed after the inventory interface is gone (+{:X}+{:X}) no longer reads it through null", LIST_DESTRUCTOR.offset(), CALL);
		return true;
	}

	std::string Describe()
	{
		return std::format("exitInventoryGuard={} listsAfterInterface={}", installed ? "on" : "off", skipped.load(std::memory_order_relaxed));
	}
}
