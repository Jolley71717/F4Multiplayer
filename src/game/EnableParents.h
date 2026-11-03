#pragma once

#include "game/EnableChain.h"

#include <array>
#include <format>

// Switching on a reference a friend's game has on (EnableChain.h says why a parent, and which). Main thread only.
namespace EnableParents
{
	// The reference's enable parent as the engine finds it (+565490: from ExtraEnableStateParent's handle, or, while that
	// is not set yet, from the parent's form id the reference keeps), into a_out; returns a_out.
	inline constexpr REL::ID GET_ENABLE_PARENT{ 2202691 };
	// ExtraEnableStateParent's flags byte (EnableChain::OPPOSITE), after BSExtraData's 0x18 bytes; its parent's handle
	// follows at +0x1C.
	inline constexpr std::size_t FLAGS_AT = 0x18;

	// a_ref's enable parent (null if none, or none found) and whether the link is the opposite way.
	inline RE::NiPointer<RE::TESObjectREFR> ParentOf(RE::TESObjectREFR* a_ref, bool& a_opposite)
	{
		a_opposite = false;
		const auto extra = a_ref && a_ref->extraList ? a_ref->extraList->GetByType(RE::EXTRA_DATA_TYPE::kEnableStateParent) : nullptr;
		if (!extra) {
			return nullptr;
		}
		a_opposite = (*(reinterpret_cast<const std::uint8_t*>(extra) + FLAGS_AT) & EnableChain::OPPOSITE) != 0;
		static REL::Relocation<RE::ObjectRefHandle* (*)(RE::TESObjectREFR*, RE::ObjectRefHandle*)> getParent{ GET_ENABLE_PARENT };
		RE::ObjectRefHandle handle;
		getParent(a_ref, &handle);
		return handle.get();
	}

	// What switching a_ref on takes here (EnableChain::Decide over its chain of enable parents).
	inline EnableChain::Plan Plan(RE::TESObjectREFR* a_ref, bool a_sharedStory)
	{
		std::array<EnableChain::Link, EnableChain::MAX_DEPTH + 1> links{};
		std::size_t                                              count = 0;
		RE::TESObjectREFR*                                       at = a_ref;
		RE::NiPointer<RE::TESObjectREFR>                         held;  // keeps the parent being looked at alive
		while (at && count < links.size() && at->extraList && at->extraList->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent)) {
			bool       opposite = false;
			const auto parent = ParentOf(at, opposite);
			links[count++] = { parent ? parent->GetFormID() : 0, opposite, parent && parent->IsDisabled() };
			if (!parent || opposite) {
				break;
			}
			held = parent;
			at = held.get();
		}
		return EnableChain::Decide(a_ref->GetFormID(), std::span{ links.data(), count }, a_sharedStory);
	}

	// Switches a_ref on as the plan for it says (through the console, as a command typed there: kept in saves like the
	// friend's enable was in theirs); returns the plan, which says what was switched on, or why nothing.
	inline EnableChain::Plan SwitchOn(RE::TESObjectREFR* a_ref, bool a_sharedStory)
	{
		const auto plan = Plan(a_ref, a_sharedStory);
		if (EnableChain::SwitchesOn(plan)) {
			RE::Console::ExecuteCommand(std::format("{:08X}.enable", plan.target).c_str());
		}
		return plan;
	}
}
