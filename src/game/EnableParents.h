#pragma once

#include "game/DisabledParents.h"
#include "game/EnableChain.h"
#include "game/Official.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <vector>

// Switching on a reference a friend's game has on (EnableChain.h says why a parent, and which, and when none). Main thread
// only, but for OnDataReady.
namespace EnableParents
{
	// The reference's enable parent as the engine finds it (+565490: from ExtraEnableStateParent's handle, or, while that
	// is not set yet, from the parent's form id the reference keeps), into a_out; returns a_out.
	inline constexpr REL::ID GET_ENABLE_PARENT{ 2202691 };
	// ExtraEnableStateParent's flags byte (EnableChain::OPPOSITE), after BSExtraData's 0x18 bytes; its parent's handle
	// follows at +0x1C.
	inline constexpr std::size_t FLAGS_AT = 0x18;
	// ExtraEnableStateChildren (type 0x37): the references a parent switches, a BSSimpleList of handles after BSExtraData's
	// 0x18 bytes: the first handle at +0x18, then the next node at +0x20, each node a handle and the next node at +8 (as
	// its destructor, +2A52A0, lets them go).
	inline constexpr std::size_t CHILDREN_AT = 0x18;
	inline constexpr std::size_t CHILD_NODE_NEXT = 0x8;
	inline constexpr std::uint32_t MAX_CHILDREN_COUNTED = 4096;  // a count past the cap is enough; a broken list ends here
	// g_BGSSaveLoadGame (+32A85A8; see SaveGuard.cpp) and BGSSaveLoadGame::GetChangeFlags(std::uint32_t* out, TESForm*)
	// (+BECA80): the change flags our save keeps for the form (0 if it has no record), into out; returns out. TESForm's
	// AddChange (+311840) sets them through +BEC970; Enable and Disable set CHANGE_FORM_FLAGS (bit 0) on the reference, and
	// so does much else (EnableChain::CHANGE_FORM_FLAGS): only for the log, and for a parent whose plugin state isn't known.
	inline constexpr REL::ID SAVE_LOAD_GAME{ 2697789 };
	inline constexpr REL::ID GET_CHANGE_FLAGS{ 2227900 };

	// The references loaded with the game's data (kGameDataReady, before any save is loaded), and of them those that were
	// off then: off as their plugins have them (the record's Initially Disabled flag), the others on. Sorted. Only the
	// persistent references are loaded with the data: every enable parent in Fallout4.esm is (616 off, 394 on); a
	// temporary one is loaded with its cell, and its plugin's state isn't known (EnableChain::PluginState::kUnknown).
	struct LoadedWithData
	{
		std::vector<std::uint32_t> all;
		std::vector<std::uint32_t> off;
	};

	inline LoadedWithData& Loaded()
	{
		static LoadedWithData lists;
		return lists;
	}

	// Takes the lists above, once: a later message would find the states a save gave them. At kGameDataReady, on the
	// game's loading thread (not the main one): written once, before any session, and only read later, on the main thread.
	// Another F4SE plugin's kGameDataReady handler that runs before ours and switches references off would make them read
	// as off in their plugins; only one of the game's own files' references (Official::IsOfficialThroughout) is ever
	// switched on through it.
	inline void OnDataReady()
	{
		static bool taken = false;
		if (taken) {
			return;
		}
		taken = true;
		const auto start = std::chrono::steady_clock::now();
		auto&      lists = Loaded();
		const auto& [map, lock] = RE::TESForm::GetAllForms();
		RE::BSAutoReadLock l{ lock };
		if (!map) {
			REX::WARN("EnableParents: no forms to read which references are off in their plugins");
			return;
		}
		for (const auto& [id, form] : *map) {
			const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
			if (!ref) {
				continue;
			}
			lists.all.push_back(id);
			if (ref->IsDisabled()) {
				lists.off.push_back(id);
			}
		}
		std::ranges::sort(lists.all);
		std::ranges::sort(lists.off);
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		REX::INFO("EnableParents: {} references loaded with the game's data, {} of them off in their plugins (read in {:.1f} ms)", lists.all.size(),
			lists.off.size(), ms);
	}

	inline EnableChain::PluginState PluginStateOf(std::uint32_t a_id)
	{
		const auto& lists = Loaded();
		if (!std::ranges::binary_search(lists.all, a_id)) {
			return EnableChain::PluginState::kUnknown;
		}
		return std::ranges::binary_search(lists.off, a_id) ? EnableChain::PluginState::kOff : EnableChain::PluginState::kOn;
	}

	// Whether a script of the game's own turns a_ref off somewhere (DisabledParents.h): by the plugin that made it and its
	// form id there, whatever the load order.
	inline bool StoryTurnsOff(const RE::TESObjectREFR* a_ref)
	{
		const auto file = a_ref ? a_ref->GetFile(0) : nullptr;
		return file && DisabledParents::Contains(file->GetFilename(), a_ref->GetFormID());
	}

	// a_ref's enable parent (null if none, none found, or deleted) and whether the link is the opposite way.
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
		auto parent = handle.get();
		if (parent && parent->IsDeleted()) {
			return nullptr;
		}
		return parent;
	}

	// How many references a_parent switches here (its ExtraEnableStateChildren), up to MAX_CHILDREN_COUNTED.
	inline std::uint32_t ChildrenOf(RE::TESObjectREFR* a_parent)
	{
		const auto extra = a_parent && a_parent->extraList ? a_parent->extraList->GetByType(RE::EXTRA_DATA_TYPE::kEnableStateChildren) : nullptr;
		if (!extra) {
			return 0;
		}
		const auto    at = reinterpret_cast<const std::uint8_t*>(extra) + CHILDREN_AT;
		std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(at) != 0 ? 1 : 0;
		for (auto node = *reinterpret_cast<const std::uint8_t* const*>(at + 0x8); node && count < MAX_CHILDREN_COUNTED;
			node = *reinterpret_cast<const std::uint8_t* const*>(node + CHILD_NODE_NEXT)) {
			count += *reinterpret_cast<const std::uint32_t*>(node) != 0 ? 1 : 0;
		}
		return count;
	}

	// What our save records as changed on a_ref (all of them, CHANGE_FORM_FLAGS among them); EnableChain::CHANGES_UNKNOWN
	// when the game's save bookkeeping isn't there.
	inline std::uint32_t ChangeFlags(RE::TESObjectREFR* a_ref)
	{
		static REL::Relocation<void**>                                                 game{ SAVE_LOAD_GAME };
		static REL::Relocation<std::uint32_t* (*)(void*, std::uint32_t*, RE::TESForm*)> getChangeFlags{ GET_CHANGE_FLAGS };
		if (!*game) {
			return EnableChain::CHANGES_UNKNOWN;
		}
		std::uint32_t flags = 0;
		getChangeFlags(*game, &flags, a_ref);
		return flags;
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
			auto&      link = links[count++];
			link.opposite = opposite;
			if (parent) {
				const auto actor = parent->As<RE::Actor>();
				link.parent = parent->GetFormID();
				link.parentOff = parent->IsDisabled();
				// The child's record names the parent (XESP): the link is the game's own if no plugin changes the child.
				link.official = Official::IsOfficialThroughout(at) && Official::IsOfficialThroughout(parent.get());
				link.storyTurnsOff = link.official && StoryTurnsOff(parent.get());
				link.plugin = PluginStateOf(link.parent);
				link.changeFlags = ChangeFlags(parent.get());
				link.parentDead = actor && actor->IsDead(false);
				link.children = ChildrenOf(parent.get());
			}
			if (!parent || opposite) {
				break;
			}
			held = parent;
			at = held.get();
		}
		return EnableChain::Decide(a_ref->GetFormID(), std::span{ links.data(), count }, a_sharedStory);
	}

	// The plan for the log: what was switched on, or why nothing, how many references the chain's parents switch here, and
	// the top's change flags in our save and its state in its plugin (what AsItsPluginLeftIt went by).
	inline std::string Describe(const EnableChain::Plan& a_plan)
	{
		auto text = std::format("{} {:08X}", EnableChain::VerdictName(a_plan.verdict), a_plan.target);
		if (a_plan.atTop) {
			const auto flags = a_plan.changeFlags == EnableChain::CHANGES_UNKNOWN ? std::string{ "not known" } : std::format("{:08X}", a_plan.changeFlags);
			const auto plugin = a_plan.plugin == EnableChain::PluginState::kOff ? "yes" : a_plan.plugin == EnableChain::PluginState::kOn ? "no" : "not known";
			text += std::format(" ({} references under its enable parents here, at most {}; its change flags in our save {}, off in its plugin: {})",
				a_plan.children, EnableChain::MAX_CHILDREN, flags, plugin);
		}
		return text;
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
