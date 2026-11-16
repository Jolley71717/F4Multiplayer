#pragma once

#include "Protocol.h"
#include "SessionBuilds.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
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
		using Clock = std::chrono::steady_clock;

		static constexpr std::chrono::seconds LOOK_AGAIN{ 5 };  // between looks for the script object of a noScript copy

		std::vector<Mod> want;
		int              calls = 0;
		int              budget = 0;      // set when the first call is made
		bool             done = false;    // the copy matches
		bool             gaveUp = false;  // the budget is spent and it still doesn't
		// No call could be made on the copy at the last look: no Actor script object is bound to it (a brahmin's horns and
		// heads are mods on it, and its base has no script; or the VM hasn't bound a robot's yet, just after a load), and
		// making one inside the VM on a mirrored NPC has crashed the game (Papyrus.h). No call is counted and nothing is
		// given up on: the copy keeps the mods it has, the work open (not said if our game comes to run it). Looked at again
		// LOOK_AGAIN later (one lookup, no call), and at once when the copy is mirrored anew or the build changes; cleared
		// when one is found.
		bool              noScript = false;
		Clock::time_point noScriptAt{};  // the look that found none

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
			noScript = false;
			return true;
		}

		// The copy is looked at afresh (it was mirrored again: loaded anew, or from a save of before). A copy that matched
		// is looked at again with a new budget. Work not finished keeps its calls and budget: a copy at a cell's edge is
		// mirrored again every few seconds, and a fresh count each time would never give up on a part the engine never
		// takes (each attach resets its 3D), nor keep a swap under way going through a fight (Started). One given up on
		// stays given up on until the build changes. One no call could be made on is looked at again at once (Step).
		void Again()
		{
			noScript = false;
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
			noScript = false;
		}

		// A swap is under way: calls made and the copy not yet the build. It is finished as one (a fight starting doesn't
		// stop it half way); only starting one waits for calm.
		[[nodiscard]] bool Started() const { return calls > 0 && !done && !gaveUp && !noScript; }

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

		// No call could be made on it at the last look, and it isn't time to look again: the copy isn't even read.
		[[nodiscard]] bool Resting(Clock::time_point a_now) const { return noScript && a_now - noScriptAt < LOOK_AGAIN; }

		// The copy as it is now: the call to make next, if any. Counts it against the budget. a_scripted() is asked only when
		// a call is needed: whether one can be made on the copy at all (an Actor script object is bound to it). When none
		// can, a copy that already is the build is done all the same, and one that isn't is marked noScript (at a_now), with
		// nothing counted; a later look that finds one clears it and goes on.
		template <class Scripted>
		std::optional<Call> Step(std::span<const Mod> a_have, Clock::time_point a_now, Scripted&& a_scripted)
		{
			if (done || gaveUp || Resting(a_now)) {
				return std::nullopt;
			}
			const auto plan = Diff(a_have, want);
			if (plan.Empty()) {
				done = true;
				noScript = false;
				return std::nullopt;
			}
			if (!a_scripted()) {
				noScript = true;
				noScriptAt = a_now;
				return std::nullopt;
			}
			noScript = false;
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

		// As above, a_callable said up front (the tests).
		std::optional<Call> Step(std::span<const Mod> a_have, bool a_callable = true, Clock::time_point a_now = {})
		{
			return Step(a_have, a_now, [&] { return a_callable; });
		}
	};

	// Whether the copy may be changed now: mirrored here (registered, its 3D loaded), alive and the game not paused, and
	// calm (a part attached mid-fight resets its 3D and its health's share) unless a swap is under way, which is finished.
	inline bool Ready(const Work& a_work, bool a_mirrored, bool a_loaded, bool a_dead, bool a_inCombat, bool a_paused)
	{
		return a_mirrored && a_loaded && !a_dead && !a_paused && (!a_inCombat || a_work.Started());
	}

	// Our mod calls on the copies we mirror that the VM hasn't answered yet (NpcSync's ModCallDone: run, or cancelled), by
	// serial. An NPC with one unanswered is in flight for Say however long the call takes: it can still land (a Papyrus
	// overloaded past NpcSync's MOD_CALL_WAIT, which only lets the other copies' calls go on), and landing after our game
	// took the NPC over, it would read as our player's change at the robot workbench and replace the session's build with
	// half a swap. Only the answer clears it (or a load: the calls of the world before it are gone), and the NPC stays in
	// flight SETTLE past it, so a change the engine applies a frame after the call returns is read before the build of the
	// take-over is kept. Main thread only: the caller hands the VM's answers over (Answered).
	class Calls
	{
	public:
		using Clock = std::chrono::steady_clock;

		// One gear read (NpcSync's GEAR_INTERVAL) past the answer.
		static constexpr std::chrono::milliseconds SETTLE{ 1000 };

		void Sent(std::uint32_t a_refId, std::uint32_t a_serial) { calls[a_serial] = { a_refId, std::nullopt }; }

		// The VM answered a_serial. One not known (from before a load) is let be. Calls settled by a_now go.
		void Answered(std::uint32_t a_serial, Clock::time_point a_now)
		{
			if (const auto it = calls.find(a_serial); it != calls.end() && !it->second.answered) {
				it->second.answered = a_now;
			}
			std::erase_if(calls, [&](const auto& a_entry) { return a_entry.second.answered && a_now - *a_entry.second.answered >= SETTLE; });
		}

		// The VM hasn't answered a_serial yet.
		[[nodiscard]] bool Pending(std::uint32_t a_serial) const
		{
			const auto it = calls.find(a_serial);
			return it != calls.end() && !it->second.answered;
		}

		// A call on a_refId the VM hasn't answered yet: no other is made on it.
		[[nodiscard]] bool Unanswered(std::uint32_t a_refId) const
		{
			return std::ranges::any_of(calls, [&](const auto& a_entry) { return a_entry.second.refId == a_refId && !a_entry.second.answered; });
		}

		// A call on a_refId can still change it: unanswered, or answered less than SETTLE ago. Nothing is said or kept of its
		// mods until none can (Say).
		[[nodiscard]] bool InFlight(std::uint32_t a_refId, Clock::time_point a_now) const
		{
			return std::ranges::any_of(calls, [&](const auto& a_entry) {
				return a_entry.second.refId == a_refId && (!a_entry.second.answered || a_now - *a_entry.second.answered < SETTLE);
			});
		}

		// Calls the VM hasn't answered.
		[[nodiscard]] std::size_t Outstanding() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(calls, [](const auto& a_entry) { return !a_entry.second.answered; }));
		}

		void Clear() { calls.clear(); }

	private:
		struct Call
		{
			std::uint32_t                    refId = 0;
			std::optional<Clock::time_point> answered;
		};

		std::unordered_map<std::uint32_t, Call> calls;
	};

	// What our game says of the mods on an NPC it runs (NpcEquipment::modsRead and modsChanged).
	struct Said
	{
		bool read = false;     // the mods are said
		bool changed = false;  // they changed while we ran it: the session's build is ours now
	};

	// a_inFlight: a call of ours on it may not have landed yet (Calls::InFlight): nothing said and nothing kept until it
	// has. a_open: our own work to give the NPC the session's build hadn't finished when we took it over: its parts are half
	// ours and half the world's, not a build anyone made, so it isn't said and the session's stands. a_first: the build read
	// first in this run of it (none yet: this one is kept, open work or not), or the last one said as changed. A later one
	// that differs is a change our player made (the robot workbench), open work or not: said as changed once, so it
	// replaces the session's, and the caller makes it the work's (Work::Mine) so no later mirror undoes it in our own world.
	// It becomes a_first in turn: changed means changed since the last said. A read with no mods says none and keeps none: a robot
	// always has parts, so it is a read made before its instance was there (Equipment::ReadActorMods), not a build.
	inline Said Say(bool a_inFlight, bool a_open, std::optional<std::vector<Mod>>& a_first, std::span<const Mod> a_now)
	{
		if (a_inFlight || a_now.empty()) {
			return {};
		}
		if (!a_first) {
			a_first.emplace(a_now.begin(), a_now.end());
		} else if (!SessionBuilds::Same(*a_first, a_now)) {
			a_first->assign(a_now.begin(), a_now.end());
			return { true, true };
		}
		if (a_open) {
			return {};
		}
		return { true, false };
	}
}
