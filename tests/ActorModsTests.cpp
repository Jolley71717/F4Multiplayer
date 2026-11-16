// The mods on an NPC's own reference (a robot's parts from the robot workbench), given to its copy in a friend's game
// (src/game/ActorMods.h): what is attached and taken off, in which order, when our game stops trying, and what a game that
// runs the NPC says of them.

#include "Test.h"

#include "game/ActorMods.h"

namespace
{
	using ActorMods::Mod;

	constexpr Mod ARMS_CLAWS{ 0x0100A101, 0, 0 };
	constexpr Mod ARMS_SAWS{ 0x0100A102, 0, 0 };
	constexpr Mod LEGS_WHEEL{ 0x0100A201, 0, 0 };
	constexpr Mod HEAD_EYEBOT{ 0x0100A301, 0, 0 };
	constexpr Mod PAINT_RED{ 0x0100A401, 1, 0 };

	bool Ready(const ActorMods::Work& a_work, bool a_inCombat)
	{
		return ActorMods::Ready(a_work, true, true, false, a_inCombat, false);
	}
}

TEST("actor mods: the copy gains what the session's robot has first, then loses what it lacks")
{
	const std::vector<Mod> have{ ARMS_CLAWS, LEGS_WHEEL };
	const std::vector<Mod> want{ ARMS_SAWS, LEGS_WHEEL, PAINT_RED };
	const auto             plan = ActorMods::Diff(have, want);
	CHECK(plan.remove.size() == 1);
	CHECK(plan.remove[0].mod == ARMS_CLAWS.mod);
	CHECK(plan.attach.size() == 2);
	CHECK(plan.attach[0].mod == ARMS_SAWS.mod);
	CHECK(plan.attach[1].mod == PAINT_RED.mod);
	CHECK(plan.attach[1].index == 1);  // its attach index goes with it
	// The new arm goes on first (it takes the old one's place); what is left of the old one goes after: a copy stopped
	// between the two still has two arms.
	const auto first = ActorMods::Next(plan);
	REQUIRE(first.has_value());
	CHECK(!first->remove);
	CHECK(first->mod.mod == ARMS_SAWS.mod);
	const auto last = ActorMods::Next(ActorMods::Diff(std::vector<Mod>{ ARMS_CLAWS, ARMS_SAWS, LEGS_WHEEL, PAINT_RED }, want));
	REQUIRE(last.has_value());
	CHECK(last->remove);
	CHECK(last->mod.mod == ARMS_CLAWS.mod);
	CHECK(ActorMods::Diff(want, want).Empty());
	CHECK(!ActorMods::Next(ActorMods::Diff(want, want)).has_value());
	// The same mod and index named twice is one change.
	const std::vector<Mod> twice{ HEAD_EYEBOT, HEAD_EYEBOT };
	CHECK(ActorMods::Diff({}, twice).attach.size() == 1);
	CHECK(ActorMods::Diff(twice, {}).remove.size() == 1);
}

TEST("actor mods: one mod at two attach indexes is two parts, and a part at the wrong index is attached at the right one")
{
	constexpr Mod PAINT_TORSO{ 0x0100A402, 1, 0 };
	constexpr Mod PAINT_HEAD{ 0x0100A402, 2, 0 };
	constexpr Mod OTHER_PAINT_TORSO{ 0x0100A403, 1, 0 };
	constexpr Mod OTHER_PAINT_HEAD{ 0x0100A403, 2, 0 };
	// Painted all over in another paint: both parts change, not one.
	const auto plan = ActorMods::Diff(std::vector<Mod>{ OTHER_PAINT_TORSO, OTHER_PAINT_HEAD }, std::vector<Mod>{ PAINT_TORSO, PAINT_HEAD });
	CHECK(plan.attach.size() == 2);
	CHECK(plan.remove.size() == 2);
	// Painted on the torso alone: the head is still to do, so the work isn't done.
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ PAINT_TORSO, PAINT_HEAD });
	const auto call = work.Step(std::vector<Mod>{ PAINT_TORSO });
	REQUIRE(call.has_value());
	CHECK(!call->remove);
	CHECK(call->mod.index == 2);
	CHECK(!work.done);
	// The right mod at the wrong index: attached at the right one. The other isn't taken off: RemoveMod names no index, so
	// it could take off the one just attached (and the next attach put it back, until the budget ran out).
	const auto moved = ActorMods::Diff(std::vector<Mod>{ PAINT_HEAD }, std::vector<Mod>{ PAINT_TORSO });
	REQUIRE(moved.attach.size() == 1);
	CHECK(moved.attach[0].index == 1);
	CHECK(moved.remove.empty());
	// Once it is there the copy is taken as done, the extra instance and all: no removal that could undo the attach.
	ActorMods::Work move;
	move.Want(std::vector<Mod>{ PAINT_TORSO });
	REQUIRE(move.Step(std::vector<Mod>{ PAINT_HEAD }).has_value());
	CHECK(!move.Step(std::vector<Mod>{ PAINT_HEAD, PAINT_TORSO }).has_value());
	CHECK(move.done);
	CHECK(move.calls == 1);
	// A mod the build doesn't have at any index is still taken off.
	CHECK(ActorMods::Diff(std::vector<Mod>{ PAINT_TORSO, OTHER_PAINT_HEAD }, std::vector<Mod>{ PAINT_TORSO }).remove.size() == 1);
	// The same build listed in another order is the same report.
	CHECK(!work.Want(std::vector<Mod>{ PAINT_HEAD, PAINT_TORSO }));
}

TEST("actor mods: one call at a time until the copy matches, within a budget")
{
	ActorMods::Work work;
	CHECK(work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL }));
	CHECK(!work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL }));  // the same report again changes nothing
	std::vector<Mod> copy{ ARMS_CLAWS, LEGS_WHEEL };
	auto             call = work.Step(copy);
	REQUIRE(call.has_value());
	CHECK(!call->remove);
	CHECK(call->mod.mod == ARMS_SAWS.mod);
	CHECK(work.budget == 6);  // two changes: twice each, and two over
	copy.push_back(ARMS_SAWS);
	call = work.Step(copy);
	REQUIRE(call.has_value());
	CHECK(call->remove);
	CHECK(call->mod.mod == ARMS_CLAWS.mod);
	std::erase_if(copy, [](const Mod& a_mod) { return a_mod.mod == ARMS_CLAWS.mod; });
	CHECK(!work.Step(copy).has_value());
	CHECK(work.done);
	CHECK(!work.gaveUp);
	CHECK(!work.Open());
	// Mirrored again (a load brought the copy back as a save had it): looked at afresh, with a new budget.
	copy = { ARMS_CLAWS, LEGS_WHEEL };
	CHECK(!work.Step(copy).has_value());  // done: not looked at until told
	work.Again();
	CHECK(work.Open());
	CHECK(work.Step(copy).has_value());
}

TEST("actor mods: a part the engine won't attach is given up on until the session's robot changes, re-mirrors or not")
{
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ HEAD_EYEBOT });
	const std::vector<Mod> copy{};  // the attach never takes
	int                    calls = 0;
	while (work.Step(copy)) {
		++calls;
		REQUIRE(calls < 10);
	}
	CHECK(calls == 4);  // one change: twice, and two over
	CHECK(work.gaveUp);
	CHECK(work.Open());  // the copy isn't the build: not said if we come to run it
	CHECK(!work.Step(copy).has_value());
	// Mirrored again: still given up on (no new budget, no new pops of its 3D, no new warning).
	work.Again();
	CHECK(work.gaveUp);
	CHECK(!work.Step(copy).has_value());
	// A new build: tried again.
	CHECK(work.Want(std::vector<Mod>{ HEAD_EYEBOT, PAINT_RED }));
	CHECK(!work.gaveUp);
	CHECK(work.Step(copy).has_value());
}

TEST("actor mods: a copy mirrored again between calls keeps its count, so a part that never takes is still given up on")
{
	// A copy at a cell's edge, mirrored again before every call: the budget isn't renewed each time.
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ HEAD_EYEBOT });
	const std::vector<Mod> copy{};
	int                    calls = 0;
	while (work.Step(copy)) {
		++calls;
		REQUIRE(calls < 10);
		work.Again();
		CHECK(work.calls == calls);
		CHECK(work.budget == 4);
	}
	CHECK(calls == 4);
	CHECK(work.gaveUp);
	// A swap under way mirrored again mid-fight is still under way: it is finished, not stopped half way until calm.
	ActorMods::Work swap;
	swap.Want(std::vector<Mod>{ ARMS_SAWS });
	REQUIRE(swap.Step(std::vector<Mod>{ ARMS_CLAWS }).has_value());
	swap.Again();
	CHECK(swap.Started());
	CHECK(Ready(swap, true));
	const auto next = swap.Step(std::vector<Mod>{ ARMS_CLAWS, ARMS_SAWS });
	REQUIRE(next.has_value());
	CHECK(next->remove);
	CHECK(swap.calls == 2);
}

TEST("actor mods: a swap starts only while calm, and once started is finished through a fight")
{
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ ARMS_SAWS });
	CHECK(Ready(work, false));
	CHECK(!Ready(work, true));  // a fight: nothing started
	CHECK(!work.Started());
	REQUIRE(work.Step(std::vector<Mod>{ ARMS_CLAWS }).has_value());  // the saws go on
	CHECK(work.Started());
	CHECK(Ready(work, true));  // the claws' removal follows, fight or not
	CHECK(!work.Step(std::vector<Mod>{ ARMS_SAWS }).has_value());
	CHECK(work.done);
	CHECK(!work.Started());
	// Never while it isn't mirrored here, loaded, alive and the game running, swap or not.
	ActorMods::Work started;
	started.Want(std::vector<Mod>{ ARMS_SAWS });
	REQUIRE(started.Step(std::vector<Mod>{}).has_value());
	CHECK(ActorMods::Ready(started, true, true, false, false, false));
	CHECK(!ActorMods::Ready(started, false, true, false, false, false));
	CHECK(!ActorMods::Ready(started, true, false, false, false, false));
	CHECK(!ActorMods::Ready(started, true, true, true, false, false));
	CHECK(!ActorMods::Ready(started, true, true, false, false, true));
}

TEST("actor mods: a game that runs a robot says its build only when it is a build, and a change its player made as one")
{
	// A call of ours on it still in flight: nothing said, nothing kept, until it lands.
	std::optional<std::vector<Mod>> first;
	auto                            said = ActorMods::Say(true, false, first, std::vector<Mod>{ ARMS_CLAWS });
	CHECK(!said.read);
	CHECK(!first.has_value());
	// Read with no mods (its instance not there yet): nothing said, nothing kept, so the real build read next isn't a change.
	said = ActorMods::Say(false, false, first, std::vector<Mod>{});
	CHECK(!said.read);
	CHECK(!first.has_value());
	// Our copy is a build (it matched, or we never had another's): said, and kept as this run's first.
	said = ActorMods::Say(false, false, first, std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL });
	CHECK(said.read);
	CHECK(!said.changed);
	REQUIRE(first.has_value());
	// The same build in another order: no change.
	said = ActorMods::Say(false, false, first, std::vector<Mod>{ LEGS_WHEEL, ARMS_SAWS });
	CHECK(said.read);
	CHECK(!said.changed);
	// Our player at the robot workbench: a change, which replaces the session's.
	said = ActorMods::Say(false, false, first, std::vector<Mod>{ ARMS_CLAWS, LEGS_WHEEL });
	CHECK(said.read);
	CHECK(said.changed);
}

TEST("actor mods: a robot taken over mid-swap keeps the build read then, and a workbench change made after is said and never undone")
{
	// Our copy is half way through the session's build (the saws went on, the claws are still to come off) and a call is in
	// flight when our game takes it over.
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL });
	REQUIRE(work.Step(std::vector<Mod>{ ARMS_CLAWS, LEGS_WHEEL }).has_value());
	std::optional<std::vector<Mod>> first;
	CHECK(!ActorMods::Say(true, true, first, std::vector<Mod>{ ARMS_CLAWS, LEGS_WHEEL }).read);
	CHECK(!first.has_value());
	// The call lands: half and half, nobody's build, so not said; but kept as what we took over.
	const std::vector<Mod> half{ ARMS_CLAWS, ARMS_SAWS, LEGS_WHEEL };
	CHECK(!work.Check(half));
	auto said = ActorMods::Say(false, work.Open(), first, half);
	CHECK(!said.read);
	REQUIRE(first.has_value());
	CHECK(!ActorMods::Say(false, work.Open(), first, std::vector<Mod>{ LEGS_WHEEL, ARMS_SAWS, ARMS_CLAWS }).read);  // as it was
	// Our player rebuilds it at the robot workbench: a change of ours, said as one with our work still open.
	const std::vector<Mod> rebuilt{ HEAD_EYEBOT, ARMS_SAWS, LEGS_WHEEL };
	CHECK(!work.Check(rebuilt));
	said = ActorMods::Say(false, work.Open(), first, rebuilt);
	CHECK(said.read);
	CHECK(said.changed);
	work.Mine(rebuilt);
	CHECK(!work.Open());
	CHECK(work.Check(rebuilt));
	// Said as changed from then on, so the server takes it whoever kept the build before.
	said = ActorMods::Say(false, work.Open(), first, rebuilt);
	CHECK(said.read);
	CHECK(said.changed);
	// Mirrored again later (another game runs it, with our build as the session's): nothing to undo.
	CHECK(!work.Want(std::vector<Mod>{ LEGS_WHEEL, HEAD_EYEBOT, ARMS_SAWS }));
	work.Again();
	CHECK(!work.Step(rebuilt).has_value());
	CHECK(work.done);
	CHECK(work.calls == 0);
}

TEST("actor mods: a robot taken over that already is the session's build is said; one that isn't stays unsaid")
{
	ActorMods::Work work;
	work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL });
	CHECK(!work.Check(std::vector<Mod>{ ARMS_CLAWS, LEGS_WHEEL }));  // still stock here: open, nothing said
	CHECK(work.Open());
	CHECK(work.Check(std::vector<Mod>{ LEGS_WHEEL, ARMS_SAWS }));  // our world built it the same: nothing to do
	CHECK(!work.Open());
	// Given up on, and then found the build after all: done.
	ActorMods::Work stuck;
	stuck.Want(std::vector<Mod>{ HEAD_EYEBOT });
	while (stuck.Step(std::vector<Mod>{})) {}
	REQUIRE(stuck.gaveUp);
	CHECK(stuck.Check(std::vector<Mod>{ HEAD_EYEBOT }));
	CHECK(!stuck.gaveUp);
}
