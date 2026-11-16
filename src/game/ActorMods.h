#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The mods on an NPC's own reference, given to its copy in a friend's game (src/game/NpcSync.cpp). A robot's parts from
// the robot workbench (Automatron: Ada's arms, legs, head, torso, armor and paint) are mods attached to the robot itself,
// not items it wears, so the gear report (items only) never carried them: a friend saw the robot as it was before the
// workbench. The game that runs the NPC now reports them (NpcEquipment::actorMods), and a game that mirrors it takes off
// the mods its copy has that the owner's lacks and attaches those it lacks, one Papyrus call at a time
// (ObjectReference.RemoveMod, then AttachMod), reading the copy again after each. The copy keeps that build in this game's
// saves, as it keeps the owner's roll. Kept free of the game so the rules can be tested.
namespace ActorMods
{
	using Mod = Protocol::ItemMod;

	// What changes the copy: the mods to take off (it has, the owner's lacks) and to attach (the other way), each by its
	// form, in the order the lists name them.
	struct Plan
	{
		std::vector<Mod> remove;
		std::vector<Mod> attach;

		[[nodiscard]] bool Empty() const { return remove.empty() && attach.empty(); }
		[[nodiscard]] std::size_t Size() const { return remove.size() + attach.size(); }
	};

	inline bool Has(std::span<const Mod> a_mods, std::uint32_t a_mod)
	{
		return std::ranges::find(a_mods, a_mod, &Mod::mod) != a_mods.end();
	}

	inline Plan Diff(std::span<const Mod> a_have, std::span<const Mod> a_want)
	{
		Plan plan;
		for (const auto& mod : a_have) {
			if (!Has(a_want, mod.mod) && !Has(plan.remove, mod.mod)) {
				plan.remove.push_back(mod);
			}
		}
		for (const auto& mod : a_want) {
			if (!Has(a_have, mod.mod) && !Has(plan.attach, mod.mod)) {
				plan.attach.push_back(mod);
			}
		}
		return plan;
	}

	// The next call: a removal first (a part the owner swapped out goes before its replacement comes), then an attach.
	struct Call
	{
		bool remove = false;
		Mod  mod;
	};

	inline std::optional<Call> Next(const Plan& a_plan)
	{
		if (!a_plan.remove.empty()) {
			return Call{ true, a_plan.remove.front() };
		}
		if (!a_plan.attach.empty()) {
			return Call{ false, a_plan.attach.front() };
		}
		return std::nullopt;
	}

	// The calls one report may cost before our game gives up on it (a mod the engine won't attach to this robot, say):
	// each change twice, and two over. A new report from the owner starts again.
	inline int Budget(const Plan& a_plan)
	{
		return static_cast<int>(a_plan.Size()) * 2 + 2;
	}

	// Whether the copy may be changed now: mirrored here (registered, its 3D loaded), alive, not fighting (a part attached
	// mid-fight resets its 3D and its health's share), and the game not paused.
	inline bool Ready(bool a_mirrored, bool a_loaded, bool a_dead, bool a_inCombat, bool a_paused)
	{
		return a_mirrored && a_loaded && !a_dead && !a_inCombat && !a_paused;
	}

	// One NPC's work: the owner's list, and how many calls it has cost.
	struct Work
	{
		std::vector<Mod> want;
		int              calls = 0;
		int              budget = 0;  // set when the first call is made
		bool             done = false;    // the copy matches
		bool             gaveUp = false;  // the budget is spent and it still doesn't

		// A new report: the same list leaves the work as it stands (a report of the items alone repeats it); another
		// starts it again.
		bool Want(std::span<const Mod> a_want)
		{
			if (std::ranges::equal(want, a_want)) {
				return false;
			}
			want.assign(a_want.begin(), a_want.end());
			calls = 0;
			budget = 0;
			done = false;
			gaveUp = false;
			return true;
		}

		// The copy is looked at afresh (it was mirrored again: loaded anew, or from a save of before): the same list, a new
		// budget.
		void Again()
		{
			calls = 0;
			budget = 0;
			done = false;
			gaveUp = false;
		}

		// The copy as it is now: the call to make next, if any. Counts it against the budget.
		std::optional<Call> Step(std::span<const Mod> a_have)
		{
			if (done || gaveUp) {
				return std::nullopt;
			}
			const auto plan = Diff(a_have, want);
			if (plan.Empty()) {
				done = true;
				return std::nullopt;
			}
			if (budget == 0) {
				budget = Budget(plan);
			}
			if (calls >= budget) {
				gaveUp = true;
				return std::nullopt;
			}
			++calls;
			return Next(plan);
		}
	};
}
