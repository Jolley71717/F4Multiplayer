#include "Test.h"
#include "game/ArmorClimb.h"

using namespace std::chrono_literals;
using ArmorClimb::Climb;
using ArmorClimb::Step;

namespace
{
	const ArmorClimb::TimePoint start{ 1000s };

	// A climb run in steps of a_step game time, a_steps times, with the engine's view fixed; returns the
	// last step.
	Step Run(Climb& a_climb, ArmorClimb::TimePoint& a_now, std::chrono::milliseconds a_step, int a_steps, bool a_engineIn, bool a_settled)
	{
		Step step = Step::kWait;
		for (int i = 0; i < a_steps; ++i) {
			a_now += a_step;
			a_climb.Tick(a_now, false);
			step = a_climb.Next(a_engineIn, a_settled);
		}
		return step;
	}
}

TEST("armor climb: in once the engine has the stand-in in its armor and it stands free")
{
	Climb climb{ true, start };
	climb.Tick(start, false);
	CHECK(climb.Next(true, true) == Step::kDone);

	auto  now = start;
	Climb late{ true, start };
	CHECK(Run(late, now, 100ms, 40, false, false) == Step::kWait);  // four seconds in the frame's animation
	CHECK(Run(late, now, 100ms, 1, true, true) == Step::kDone);
}

TEST("armor climb: still climbing in until both, however long the first part takes")
{
	auto  now = start;
	Climb climb{ true, start };
	CHECK(Run(climb, now, 100ms, 10, false, false) == Step::kWait);  // lining up with the frame
	CHECK(Run(climb, now, 100ms, 30, true, false) == Step::kWait);   // in, but still in the frame's animation
	CHECK(Run(climb, now, 100ms, 50, false, false) == Step::kWait);  // a slow game: longer than the old fixed six seconds
	CHECK(climb.elapsed == 9s);
}

TEST("armor climb: out once the engine has taken the armor off and it stands free")
{
	auto  now = start;
	Climb climb{ false, start };
	CHECK(Run(climb, now, 100ms, 20, true, false) == Step::kWait);  // getting out
	CHECK(Run(climb, now, 100ms, 1, true, true) == Step::kWait);    // still in the armor
	CHECK(Run(climb, now, 100ms, 1, false, false) == Step::kWait);  // out of the armor, not of the frame yet
	CHECK(Run(climb, now, 100ms, 1, false, true) == Step::kDone);
}

TEST("armor climb: a climb that never finishes is cut short at the limit")
{
	auto  now = start;
	Climb climb{ true, start };
	CHECK(Run(climb, now, 100ms, 149, false, false) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 1, false, false) == Step::kCutShort);

	now = start;
	Climb finished{ true, start };
	Run(finished, now, 100ms, 160, false, false);
	CHECK(Run(finished, now, 100ms, 1, true, true) == Step::kDone);  // finished counts, however late
}

TEST("armor climb: a climb the engine never starts is given up early")
{
	// The museum report: in=false sit=0 for fifteen seconds, twice, while the friend stood in their armor.
	auto  now = start;
	Climb climb{ true, start };
	CHECK(Run(climb, now, 100ms, 59, false, true) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 1, false, true) == Step::kCutShort);
	CHECK(climb.elapsed == ArmorClimb::START_LIMIT);

	// One the engine has begun (its furniture state left normal) is given the whole limit.
	now = start;
	Climb begun{ true, start };
	CHECK(Run(begun, now, 100ms, 10, false, false) == Step::kWait);
	CHECK(Run(begun, now, 100ms, 100, false, true) == Step::kWait);
	CHECK(Run(begun, now, 100ms, 40, false, true) == Step::kCutShort);

	// Out of a frame the engine never starts taking it out of.
	now = start;
	Climb out{ false, start };
	CHECK(Run(out, now, 100ms, 60, true, true) == Step::kCutShort);
}

TEST("armor climb: in the armor but never back to the normal furniture state is done after the grace")
{
	auto  now = start;
	Climb climb{ true, start };
	CHECK(Run(climb, now, 100ms, 10, false, false) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 80, true, false) == Step::kWait);  // in from 1.1 s on
	CHECK(Run(climb, now, 100ms, 1, true, false) == Step::kDone);
	CHECK(climb.elapsed == 1100ms + ArmorClimb::SETTLE_GRACE);

	// The grace starts again if the engine lets go of it meanwhile.
	now = start;
	Climb flip{ true, start };
	CHECK(Run(flip, now, 100ms, 50, true, false) == Step::kWait);
	CHECK(Run(flip, now, 100ms, 1, false, false) == Step::kWait);
	CHECK(Run(flip, now, 100ms, 50, true, false) == Step::kWait);
}

TEST("armor climb: paused game time is not climbing time, nor is a hitch")
{
	auto  now = start;
	Climb climb{ true, start };
	// The owner opens the Pip-Boy as the climb starts: a minute in the menu.
	for (int i = 0; i < 600; ++i) {
		now += 100ms;
		climb.Tick(now, true);
		CHECK(climb.Next(false, false) == Step::kWait);
	}
	CHECK(climb.elapsed == 0s);
	// A load or a hitch counts as one short step, and so does the first update after the menu if the
	// game stopped calling meanwhile.
	now += 30s;
	climb.Tick(now, false);
	CHECK(climb.elapsed == ArmorClimb::MAX_STEP);
	CHECK(climb.Next(false, false) == Step::kWait);
	// A clock that does not move (two updates in one tick) adds nothing.
	climb.Tick(now, false);
	CHECK(climb.elapsed == ArmorClimb::MAX_STEP);
}

TEST("armor climb: our copy of a pre-placed frame is hidden only when it is the one they wear")
{
	using ArmorClimb::ChooseOriginal;
	using ArmorClimb::Original;
	using ArmorClimb::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;  // a frame placed by a plugin
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .loaded = true };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);
	// A frame made in their game, or none: they wear a copy of nothing we have.
	CHECK(ChooseOriginal(0, base, here) == Original::kNone);
	CHECK(ChooseOriginal(0xFF001234, base, here) == Original::kNone);
	CHECK(ChooseOriginal(museum, 0, here) == Original::kNone);
	// Not here, deleted, one of ours made at runtime, or another frame by that ID.
	auto facts = here;
	facts.found = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	facts = here;
	facts.created = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	facts = here;
	facts.base = 0x00140F1A;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	// Our own player's suit, or one somebody here is in, is never hidden.
	facts = here;
	facts.ours = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
	facts = here;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	// Far away: looked at again once it loads.
	facts = here;
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotLoaded);
}
