#include "Test.h"
#include "game/ArmorClimb.h"

using namespace std::chrono_literals;
using ArmorClimb::Climb;
using ArmorClimb::Step;

namespace
{
	const ArmorClimb::TimePoint start{ 1000s };

	// The engine's furniture states (SIT_SLEEP_STATE).
	constexpr std::int32_t normal = ArmorClimb::SIT_NORMAL;
	constexpr std::int32_t wantToSit = 1;
	constexpr std::int32_t waitingForSitAnim = 2;
	constexpr std::int32_t sitting = 3;
	constexpr std::int32_t wantToStand = 4;

	// A climb run in steps of a_step game time, a_steps times, with the engine's view fixed; returns the
	// last step.
	Step Run(Climb& a_climb, ArmorClimb::TimePoint& a_now, std::chrono::milliseconds a_step, int a_steps, bool a_engineIn, std::int32_t a_sit)
	{
		Step step = Step::kWait;
		for (int i = 0; i < a_steps; ++i) {
			a_now += a_step;
			a_climb.Tick(a_now, false);
			step = a_climb.Next(a_engineIn, a_sit);
		}
		return step;
	}
}

TEST("armor climb: in once the engine has the stand-in in its armor and it stands free")
{
	Climb climb{ true, start, normal };
	climb.Tick(start, false);
	CHECK(climb.Next(true, normal) == Step::kDone);

	auto  now = start;
	Climb late{ true, start, normal };
	CHECK(Run(late, now, 100ms, 40, false, sitting) == Step::kWait);  // four seconds in the frame's animation
	CHECK(Run(late, now, 100ms, 1, true, normal) == Step::kDone);
}

TEST("armor climb: still climbing in until both, however long the first part takes")
{
	auto  now = start;
	Climb climb{ true, start, normal };
	CHECK(Run(climb, now, 100ms, 10, false, wantToSit) == Step::kWait);  // lining up with the frame
	CHECK(Run(climb, now, 100ms, 30, true, sitting) == Step::kWait);     // in, but still in the frame's animation
	CHECK(Run(climb, now, 100ms, 50, false, sitting) == Step::kWait);    // a slow game: longer than the old fixed six seconds
	CHECK(climb.elapsed == 9s);
}

TEST("armor climb: out once the engine has taken the armor off and it stands free")
{
	auto  now = start;
	Climb climb{ false, start, normal };
	CHECK(Run(climb, now, 100ms, 20, true, wantToStand) == Step::kWait);  // getting out
	CHECK(Run(climb, now, 100ms, 1, true, normal) == Step::kWait);        // still in the armor
	CHECK(Run(climb, now, 100ms, 1, false, wantToStand) == Step::kWait);  // out of the armor, not of the frame yet
	CHECK(Run(climb, now, 100ms, 1, false, normal) == Step::kDone);
}

TEST("armor climb: a climb that never finishes is cut short at the limit")
{
	auto  now = start;
	Climb climb{ true, start, normal };
	CHECK(Run(climb, now, 100ms, 149, false, sitting) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 1, false, sitting) == Step::kCutShort);

	now = start;
	Climb finished{ true, start, normal };
	Run(finished, now, 100ms, 160, false, sitting);
	CHECK(Run(finished, now, 100ms, 1, true, normal) == Step::kDone);  // finished counts, however late
}

TEST("armor climb: a climb the engine never starts is given up early")
{
	// The museum report: in=false sit=0 for fifteen seconds, twice, while the friend stood in their armor.
	auto  now = start;
	Climb climb{ true, start, normal };
	CHECK(Run(climb, now, 100ms, 59, false, normal) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 1, false, normal) == Step::kCutShort);
	CHECK(climb.elapsed == ArmorClimb::START_LIMIT);

	// One the engine has begun (its furniture state left normal) is given the whole limit.
	now = start;
	Climb begun{ true, start, normal };
	CHECK(Run(begun, now, 100ms, 10, false, wantToSit) == Step::kWait);
	CHECK(Run(begun, now, 100ms, 100, false, normal) == Step::kWait);
	CHECK(Run(begun, now, 100ms, 40, false, normal) == Step::kCutShort);

	// Out of a frame the engine never starts taking it out of.
	now = start;
	Climb out{ false, start, normal };
	CHECK(Run(out, now, 100ms, 60, true, normal) == Step::kCutShort);
}

TEST("armor climb: begun is counted from the furniture state the stand-in had when it was sent")
{
	// An earlier climb called off half way left the stand-in waiting for its sit animation: this attempt is no
	// sooner begun for that, and is given up at START_LIMIT if the engine never moves it on.
	auto  now = start;
	Climb stale{ true, start, waitingForSitAnim };
	CHECK(Run(stale, now, 100ms, 59, false, waitingForSitAnim) == Step::kWait);
	CHECK(!stale.begun);
	CHECK(Run(stale, now, 100ms, 1, false, waitingForSitAnim) == Step::kCutShort);
	CHECK(stale.elapsed == ArmorClimb::START_LIMIT);

	// The engine moving it on from that state, either way, is this attempt begun: the whole limit.
	now = start;
	Climb onward{ true, start, waitingForSitAnim };
	CHECK(Run(onward, now, 100ms, 10, false, waitingForSitAnim) == Step::kWait);
	CHECK(Run(onward, now, 100ms, 1, false, sitting) == Step::kWait);
	CHECK(onward.begun);
	CHECK(Run(onward, now, 100ms, 100, false, sitting) == Step::kWait);
	CHECK(Run(onward, now, 100ms, 39, false, sitting) == Step::kCutShort);

	now = start;
	Climb reset{ true, start, sitting };
	CHECK(Run(reset, now, 100ms, 1, false, normal) == Step::kWait);  // back to normal: the engine took it up again
	CHECK(reset.begun);
	CHECK(Run(reset, now, 100ms, 70, false, normal) == Step::kWait);

	// From the normal state the rule is the one it always was: any other state is a sign of it.
	now = start;
	Climb fresh{ true, start, normal };
	CHECK(Run(fresh, now, 100ms, 1, false, normal) == Step::kWait);
	CHECK(!fresh.begun);
	CHECK(Run(fresh, now, 100ms, 1, false, wantToSit) == Step::kWait);
	CHECK(fresh.begun);

	// Getting there is a sign of it whatever the state.
	now = start;
	Climb there{ true, start, sitting };
	CHECK(Run(there, now, 100ms, 1, true, sitting) == Step::kWait);
	CHECK(there.begun);
}

TEST("armor climb: in the armor but never back to the normal furniture state is done after the grace")
{
	auto  now = start;
	Climb climb{ true, start, normal };
	CHECK(Run(climb, now, 100ms, 10, false, sitting) == Step::kWait);
	CHECK(Run(climb, now, 100ms, 80, true, sitting) == Step::kWait);  // in from 1.1 s on
	CHECK(Run(climb, now, 100ms, 1, true, sitting) == Step::kDone);
	CHECK(climb.elapsed == 1100ms + ArmorClimb::SETTLE_GRACE);

	// The grace starts again if the engine lets go of it meanwhile.
	now = start;
	Climb flip{ true, start, normal };
	CHECK(Run(flip, now, 100ms, 50, true, sitting) == Step::kWait);
	CHECK(Run(flip, now, 100ms, 1, false, sitting) == Step::kWait);
	CHECK(Run(flip, now, 100ms, 50, true, sitting) == Step::kWait);
}

TEST("armor climb: paused game time is not climbing time, nor is a hitch")
{
	auto  now = start;
	Climb climb{ true, start, normal };
	// The owner opens the Pip-Boy as the climb starts: a minute in the menu.
	for (int i = 0; i < 600; ++i) {
		now += 100ms;
		climb.Tick(now, true);
		CHECK(climb.Next(false, sitting) == Step::kWait);
	}
	CHECK(climb.elapsed == 0s);
	// A load or a hitch counts as one short step, and so does the first update after the menu if the
	// game stopped calling meanwhile.
	now += 30s;
	climb.Tick(now, false);
	CHECK(climb.elapsed == ArmorClimb::MAX_STEP);
	CHECK(climb.Next(false, sitting) == Step::kWait);
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
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 12.0f };

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

TEST("armor climb: our copy of a pre-placed frame is hidden only where they got into theirs")
{
	using ArmorClimb::ChooseOriginal;
	using ArmorClimb::Original;
	using ArmorClimb::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 0.0f };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);
	// Up to the radius: they were lined up on it, or about to be.
	auto facts = here;
	facts.fromSpot = ArmorClimb::ORIGINAL_RADIUS;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.fromSpot = 180.0f;  // as far as a player activates something
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	// Ours was moved (our player walked off in it and left it elsewhere, say): theirs is not ours.
	facts.fromSpot = ArmorClimb::ORIGINAL_RADIUS + 1.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	facts.fromSpot = 5000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// They got in somewhere else altogether (another cell or worldspace): no distance to compare.
	facts.fromSpot.reset();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// Once seen elsewhere (or moved, or used, while hidden) in this wear, it stays shown even back on the spot.
	facts = here;
	facts.moved = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// Not loaded: where it stands is looked at once it is.
	facts = here;
	facts.loaded = false;
	facts.fromSpot.reset();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotLoaded);
	// Occupied, or our player's own, whatever the distance.
	facts = here;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	facts = here;
	facts.ours = true;
	facts.fromSpot = 5000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
}

TEST("armor climb: our copy is hidden only when it is a power armor frame of the base they sent")
{
	using ArmorClimb::ChooseOriginal;
	using ArmorClimb::Original;
	using ArmorClimb::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 0.0f };

	// A reference that is furniture but no frame (a chair, a terminal), even of the base they sent: never hidden.
	auto facts = here;
	facts.frame = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotFrame);
	// A frame of another base than the one they sent: not theirs.
	CHECK(ChooseOriginal(museum, base + 1, here) == Original::kMissing);
	facts.base = 0x00012345;
	CHECK(ChooseOriginal(museum, 0x00012345, facts) == Original::kNotFrame);
}
