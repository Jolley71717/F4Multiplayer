#pragma once

#include "Protocol.h"
#include "SessionBuilds.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The mods on an NPC's own reference, given to its copy in a friend's game (src/game/NpcSync.cpp). A robot's parts from
// the robot workbench (Automatron: Ada's arms, legs, head, torso, armor and paint) are mods attached to the robot itself,
// not items it wears, so the gear report (items only) never carried them: a friend saw the robot as it was before the
// workbench. The game that runs the NPC now reports them (NpcEquipment::actorMods), the server keeps the session's build
// (the first runner's, common/SessionBuilds.h), and a game that mirrors it attaches the mods its copy lacks and takes off
// those the session's build lacks, one Papyrus call at a time (ObjectReference.AttachMod, then RemoveMod), reading the
// copy again after each. The copy keeps that build in this game's saves, as it keeps the session's roll. Kept free of the
// game so the rules can be tested.
namespace ActorMods
{
	using Mod = Protocol::ItemMod;

	// What changes the copy: the mods to attach (the session's build has them, the copy lacks them) and to take off (the
	// other way), each a mod at its attach index, in the order the lists name them. The same mod at two attach indexes is
	// two parts (a paint on the torso and on the head).
	struct Plan
	{
		std::vector<Mod> attach;
		std::vector<Mod> remove;

		[[nodiscard]] bool Empty() const { return attach.empty() && remove.empty(); }
		[[nodiscard]] std::size_t Size() const { return attach.size() + remove.size(); }
	};

	inline bool Has(std::span<const Mod> a_mods, const Mod& a_mod)
	{
		return std::ranges::any_of(a_mods, [&](const Mod& a_has) { return a_has.mod == a_mod.mod && a_has.index == a_mod.index; });
	}

	// The mod itself is in a_mods, at any attach index.
	inline bool HasMod(std::span<const Mod> a_mods, std::uint32_t a_mod)
	{
		return std::ranges::any_of(a_mods, [&](const Mod& a_has) { return a_has.mod == a_mod; });
	}

	// A part the session's build has at another index is not taken off where it sits: ObjectReference.RemoveMod names the
	// mod alone, not its index, so it could take off the one just attached at the right index, and the next attach put it
	// back (a swap going back and forth until the budget ran out). The attach at the right index is the change; a copy left
	// with the mod at both indexes as well is taken as done.
	inline Plan Diff(std::span<const Mod> a_have, std::span<const Mod> a_want)
	{
		Plan plan;
		for (const auto& mod : a_want) {
			if (!Has(a_have, mod) && !Has(plan.attach, mod)) {
				plan.attach.push_back(mod);
			}
		}
		for (const auto& mod : a_have) {
			if (!Has(a_want, mod) && !HasMod(a_want, mod.mod) && !Has(plan.remove, mod)) {
				plan.remove.push_back(mod);
			}
		}
		return plan;
	}

	// The next call: an attach first, then a removal. A part attached where another one sits takes its place (the robot
	// workbench's own swap), so a copy stopped half way through a swap (a fight, a load, the session ending) still has
	// every part, never a robot missing an arm in a save. What is left of the old part after the attaches goes last.
	struct Call
	{
		bool remove = false;
		Mod  mod;
	};

	inline std::optional<Call> Next(const Plan& a_plan)
	{
		if (!a_plan.attach.empty()) {
			return Call{ false, a_plan.attach.front() };
		}
		if (!a_plan.remove.empty()) {
			return Call{ true, a_plan.remove.front() };
		}
		return std::nullopt;
	}

	// The calls one build may cost before our game gives up on it (a mod the engine won't attach to this robot, say):
	// each change twice, and two over. A new build from the owner starts again.
	inline int Budget(const Plan& a_plan)
	{
		return static_cast<int>(a_plan.Size()) * 2 + 2;
	}

	// One NPC's work: the session's build, and how many calls it has cost.
	struct Work
	{
		std::vector<Mod> want;
		int              calls = 0;
		int              budget = 0;      // set when the first call is made
		bool             done = false;    // the copy matches
		bool             gaveUp = false;  // the budget is spent and it still doesn't

		// A new report: the same build (in any order) leaves the work as it stands (a report of the items alone repeats
		// it); another starts it again.
		bool Want(std::span<const Mod> a_want)
		{
			if (SessionBuilds::Same(want, a_want)) {
				return false;
			}
			want.assign(a_want.begin(), a_want.end());
			calls = 0;
			budget = 0;
			done = false;
			gaveUp = false;
			return true;
		}

		// The copy is looked at afresh (it was mirrored again: loaded anew, or from a save of before). A copy that matched
		// is looked at again with a new budget. Work not finished keeps its calls and budget: a copy at a cell's edge is
		// mirrored again every few seconds, and a fresh count each time would never give up on a part the engine never
		// takes (each attach resets its 3D), nor keep a swap under way going through a fight (Started). One given up on
		// stays given up on until the build changes.
		void Again()
		{
			if (!done) {
				return;
			}
			calls = 0;
			budget = 0;
			done = false;
		}

		// Our player made a_build while our game ran the NPC (the robot workbench: Say said it changed). It is what our copy
		// has, and the session's once the server takes it: nothing to do, so no later mirror of it undoes it.
		void Mine(std::span<const Mod> a_build)
		{
			want.assign(a_build.begin(), a_build.end());
			calls = 0;
			budget = 0;
			done = true;
			gaveUp = false;
		}

		// A swap is under way: calls made and the copy not yet the build. It is finished as one (a fight starting doesn't
		// stop it half way); only starting one waits for calm.
		[[nodiscard]] bool Started() const { return calls > 0 && !done && !gaveUp; }

		// Not finished: the copy may not be the session's build (or half of it). Our game doesn't say the mods of an NPC it
		// runs with work open (Say).
		[[nodiscard]] bool Open() const { return !done; }

		// The copy looked at without a call (our game runs it now, so no call is made): the work is done if it already is the
		// build, given up on or not. Whether it is.
		bool Check(std::span<const Mod> a_have)
		{
			if (!done && Diff(a_have, want).Empty()) {
				done = true;
				gaveUp = false;
			}
			return done;
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

	// Whether the copy may be changed now: mirrored here (registered, its 3D loaded), alive and the game not paused, and
	// calm (a part attached mid-fight resets its 3D and its health's share) unless a swap is under way, which is finished.
	inline bool Ready(const Work& a_work, bool a_mirrored, bool a_loaded, bool a_dead, bool a_inCombat, bool a_paused)
	{
		return a_mirrored && a_loaded && !a_dead && !a_paused && (!a_inCombat || a_work.Started());
	}

	// What our game says of the mods on an NPC it runs (NpcEquipment::modsRead and modsChanged).
	struct Said
	{
		bool read = false;     // the mods are said
		bool changed = false;  // they changed while we ran it: the session's build is ours now
	};

	// a_inFlight: a call of ours on it hasn't landed yet: nothing said and nothing kept until it has. a_open: our own work to
	// give the NPC the session's build hadn't finished when we took it over: its parts are half ours and half the world's,
	// not a build anyone made, so it isn't said and the session's stands. a_first: the build read first in this run of it
	// (none yet: this one is kept, open work or not). A later one that differs is a change our player made (the robot
	// workbench), open work or not: said as changed, so it replaces the session's, and the caller makes it the work's
	// (Work::Mine) so no later mirror undoes it in our own world. A read with no mods says none and keeps none: a robot
	// always has parts, so it is a read made before its instance was there (Equipment::ReadActorMods), not a build.
	inline Said Say(bool a_inFlight, bool a_open, std::optional<std::vector<Mod>>& a_first, std::span<const Mod> a_now)
	{
		if (a_inFlight || a_now.empty()) {
			return {};
		}
		if (!a_first) {
			a_first.emplace(a_now.begin(), a_now.end());
		} else if (!SessionBuilds::Same(*a_first, a_now)) {
			return { true, true };
		}
		if (a_open) {
			return {};
		}
		return { true, false };
	}
}
