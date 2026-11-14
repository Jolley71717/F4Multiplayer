#include "game/VatsTargets.h"

#include "Config.h"
#include "game/Puppets.h"
#include "game/VatsTargetRules.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>

namespace VatsTargets
{
	namespace
	{
		// 1.11.240, by Address Library ID (VatsTargetRules.h: how each was read).
		constexpr REL::ID IS_VALID_TARGET{ 2224729 };  // VATS's target check (+B16220): (const ActorHandle*, bool its 3D must be drawn)
		constexpr REL::ID POTENTIAL_TARGETS{ 4804507 };  // VATS's potential target list (+328FE00): BSTArray of 0x20-byte entries
		constexpr REL::ID GATHER{ 2224731 };            // the gather of potential targets (+B164B0)
		constexpr REL::ID REFERENCE_CHECK{ 2224730 };   // the reference's check (+B16380)
		constexpr REL::ID MENU_BUTTON{ 2224716 };       // VATSMenu::HandleMenuButton (+B154F0)
		constexpr REL::ID REFILL{ 2224725 };            // the potential target list's refill (+B15DC0)
		constexpr std::size_t CALL_STUB = 14;           // what a write_call<5> takes of the trampoline (the four calls share it)

		// The four sites, in VatsTargetRules::SITES's order (one ID each, so tools/addrcheck.ps1 checks each).
		constexpr std::array<const REL::ID*, VatsTargetRules::SITES.size()> SITE_FUNCTIONS{ &GATHER, &REFERENCE_CHECK, &MENU_BUTTON, &REFILL };

		// An entry of the potential target list, as the gather's add (+B22ED0) writes it.
		struct PotentialTarget
		{
			std::uint32_t  handle;    // 00: the actor's (or a projectile's) reference handle
			std::uint32_t  pad04;     // 04
			std::int64_t   unk08;     // 08: -1
			void*          effect;    // 10: its VatsEffectTarget (the highlight)
			float          distance;  // 18: from the camera
			bool           unk1C;     // 1C
		};
		static_assert(sizeof(PotentialTarget) == 0x20);

		struct PotentialList
		{
			PotentialTarget* data;      // 00
			std::uint32_t    capacity;  // 08
			std::uint32_t    pad0C;     // 0C
			std::uint32_t    size;      // 10
		};

		using IsValidTargetFn = bool (*)(const RE::ActorHandle*, bool);
		IsValidTargetFn originalIsValidTarget = nullptr;

		// The VATS menu's code asks from whichever thread runs the menu: nothing here may throw, and only Puppets' lock
		// is taken (InertShots asks it from the engine's threads too).
		std::atomic<bool>          installed{ false };
		std::atomic<bool>          skipFriends{ true };
		std::atomic<std::uint32_t> skipped{ 0 };
		std::atomic<std::uint32_t> logged{ 0 };
		constexpr std::uint32_t    LOGGED = 3;  // the first few skips are logged, the rest only counted

		bool IsStandIn(const RE::ActorHandle* a_handle)
		{
			if (!a_handle || !*a_handle) {
				return false;
			}
			const auto actor = a_handle->get();
			return actor && Puppets::IsPuppet(actor.get());
		}

		bool IsValidTargetHook(const RE::ActorHandle* a_handle, bool a_drawn)
		{
			const bool skip = skipFriends.load(std::memory_order_relaxed);
			if (VatsTargetRules::Judge(skip, skip && IsStandIn(a_handle)) == VatsTargetRules::Verdict::kSkip) {
				skipped.fetch_add(1, std::memory_order_relaxed);
				if (logged.fetch_add(1, std::memory_order_relaxed) < LOGGED) {
					const auto actor = a_handle->get();
					REX::INFO("VatsTargets: {:08X}, a friend's stand-in, is no VATS target (bVatsSkipsFriends)", actor ? actor->GetFormID() : 0);
				}
				return false;
			}
			return originalIsValidTarget(a_handle, a_drawn);
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		skipFriends = Config::Get().vatsSkipsFriends;
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("VatsTargets: no room for the hook ({} bytes free); VATS can pick friends' stand-ins", trampoline.free_size());
			return false;
		}
		constexpr auto                                                count = VatsTargetRules::SITES.size();
		std::array<std::uintptr_t, count>                             at{};
		std::array<std::array<std::uint8_t, 5>, count>                bytes{};
		for (std::size_t i = 0; i < count; ++i) {
			at[i] = SITE_FUNCTIONS[i]->address() + VatsTargetRules::SITES[i].offset;
			std::memcpy(bytes[i].data(), reinterpret_cast<const void*>(at[i]), bytes[i].size());
		}
		const auto sites = VatsTargetRules::Check(bytes, at, IS_VALID_TARGET.address());
		if (!sites.All()) {
			REX::WARN("VatsTargets: {} of the {} calls to VATS's target check are not where 1.11.240 has them (another plugin's patch?); none patched, so VATS can pick friends' stand-ins",
				sites.total - sites.calling, sites.total);
			return false;
		}
		for (const auto site : at) {
			// The same function at each site: the first patch's answer is the original.
			const auto original = trampoline.write_call<5>(site, &IsValidTargetHook);
			if (!originalIsValidTarget) {
				originalIsValidTarget = reinterpret_cast<IsValidTargetFn>(original);
			}
		}
		installed = true;
		REX::INFO("VatsTargets: VATS's target check (+{:X}, {} calls) leaves friends' stand-ins out while bVatsSkipsFriends is on (now {})", IS_VALID_TARGET.offset(), count,
			skipFriends.load() ? "on" : "off");
		return true;
	}

	void SetSkipFriends(bool a_skip)
	{
		skipFriends = a_skip;
	}

	std::string Describe()
	{
		return std::format("vatsSkip={} vatsFriendsSkipped={}", installed ? (skipFriends ? "on" : "off") : "unhooked", skipped.load(std::memory_order_relaxed));
	}

	std::string DescribeTargets()
	{
		const auto list = reinterpret_cast<const PotentialList*>(POTENTIAL_TARGETS.address());
		std::string out = std::format("{} | potential targets {}:", Describe(), list->size);
		if (!list->data) {
			return out;
		}
		for (std::uint32_t i = 0; i < list->size && i < list->capacity && i < 64; ++i) {
			const auto& entry = list->data[i];
			const auto  handle = reinterpret_cast<const RE::ObjectRefHandle*>(&entry.handle);
			const auto  ref = handle->get();
			const auto  actor = ref ? ref->As<RE::Actor>() : nullptr;
			out += std::format(" {:08X}@{:.0f}{}", ref ? ref->GetFormID() : 0, entry.distance, actor && Puppets::IsPuppet(actor) ? "(friend)" : "");
		}
		return out;
	}
}
