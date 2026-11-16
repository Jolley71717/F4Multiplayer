// The mods on an NPC's own reference (a robot's parts from the robot workbench), given to its copy in a friend's game
// (src/game/ActorMods.h): what is taken off and attached, in which order, and when our game stops trying.

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
}

TEST("actor mods: the copy loses what the owner's robot lacks, then gains what it lacks")
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
	const auto first = ActorMods::Next(plan);
	REQUIRE(first.has_value());
	CHECK(first->remove);
	CHECK(first->mod.mod == ARMS_CLAWS.mod);
	CHECK(ActorMods::Diff(want, want).Empty());
	CHECK(!ActorMods::Next(ActorMods::Diff(want, want)).has_value());
	// A mod named twice is one change.
	const std::vector<Mod> twice{ HEAD_EYEBOT, HEAD_EYEBOT };
	CHECK(ActorMods::Diff({}, twice).attach.size() == 1);
	CHECK(ActorMods::Diff(twice, {}).remove.size() == 1);
}

TEST("actor mods: one call at a time until the copy matches, within a budget")
{
	ActorMods::Work work;
	CHECK(work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL }));
	CHECK(!work.Want(std::vector<Mod>{ ARMS_SAWS, LEGS_WHEEL }));  // the same report again changes nothing
	std::vector<Mod> copy{ ARMS_CLAWS, LEGS_WHEEL };
	auto             call = work.Step(copy);
	REQUIRE(call.has_value());
	CHECK(call->remove);
	CHECK(work.budget == 6);  // two changes: twice each, and two over
	std::erase_if(copy, [](const Mod& a_mod) { return a_mod.mod == ARMS_CLAWS.mod; });
	call = work.Step(copy);
	REQUIRE(call.has_value());
	CHECK(!call->remove);
	CHECK(call->mod.mod == ARMS_SAWS.mod);
	copy.push_back(ARMS_SAWS);
	CHECK(!work.Step(copy).has_value());
	CHECK(work.done);
	CHECK(!work.gaveUp);
	// Mirrored again (a load brought the copy back as a save had it): looked at afresh.
	copy = { ARMS_CLAWS, LEGS_WHEEL };
	CHECK(!work.Step(copy).has_value());  // done: not looked at until told
	work.Again();
	CHECK(work.Step(copy).has_value());
}

TEST("actor mods: a part the engine won't attach is given up on, until the owner's robot changes")
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
	CHECK(!work.Step(copy).has_value());
	CHECK(work.Want(std::vector<Mod>{ HEAD_EYEBOT, PAINT_RED }));
	CHECK(!work.gaveUp);
	CHECK(work.Step(copy).has_value());
}

TEST("actor mods: a copy is changed only while mirrored, loaded, alive, calm and the game running")
{
	CHECK(ActorMods::Ready(true, true, false, false, false));
	CHECK(!ActorMods::Ready(false, true, false, false, false));
	CHECK(!ActorMods::Ready(true, false, false, false, false));
	CHECK(!ActorMods::Ready(true, true, true, false, false));
	CHECK(!ActorMods::Ready(true, true, false, true, false));
	CHECK(!ActorMods::Ready(true, true, false, false, true));
}
