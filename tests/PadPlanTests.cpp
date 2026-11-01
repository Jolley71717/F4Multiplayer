// The dev channel's virtual gamepad (src/game/PadPlan.h): stick and trigger values as XInput hands them over, what the
// pad holds and for how long, the pad and walkto commands' parsing, and the walkto steering and its progress.

#include "Test.h"

#include "game/PadPlan.h"

using namespace std::chrono_literals;
using PadPlan::Clock;

namespace
{
	using Axes = std::pair<std::int16_t, std::int16_t>;

	const auto START = Clock::time_point{} + 1h;

	PadPlan::Command ParseOk(std::string_view a_line)
	{
		auto parsed = PadPlan::Parse(a_line);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			Test::Fail(__FILE__, __LINE__, std::format("'{}' did not parse: {}", a_line, *error));
			throw Test::Abort{};
		}
		return std::get<PadPlan::Command>(parsed);
	}

	bool Refused(std::string_view a_line)
	{
		const auto parsed = PadPlan::Parse(a_line);
		return std::holds_alternative<std::string>(parsed) && std::get<std::string>(parsed).starts_with("error: ");
	}

	PadPlan::WalkCommand WalkOk(std::string_view a_line)
	{
		auto parsed = PadPlan::ParseWalk(a_line);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			Test::Fail(__FILE__, __LINE__, std::format("'{}' did not parse: {}", a_line, *error));
			throw Test::Abort{};
		}
		return std::get<PadPlan::WalkCommand>(parsed);
	}

	bool WalkRefused(std::string_view a_line)
	{
		return std::holds_alternative<std::string>(PadPlan::ParseWalk(a_line));
	}

	float Length(Axes a_stick)
	{
		return std::hypot(static_cast<float>(a_stick.first), static_cast<float>(a_stick.second));
	}

	bool Near(float a_a, float a_b, float a_within = 0.01f)
	{
		return std::abs(a_a - a_b) <= a_within;
	}
}

TEST("virtual pad: stick middle is zero")
{
	CHECK(PadPlan::Stick(0.0f, 0.0f, PadPlan::LEFT_DEADZONE) == Axes(0, 0));
	CHECK(PadPlan::Stick(std::nanf(""), 0.0f, PadPlan::LEFT_DEADZONE) == Axes(0, 0));
}

TEST("virtual pad: stick full push is full travel")
{
	CHECK(PadPlan::Stick(0.0f, 1.0f, PadPlan::LEFT_DEADZONE) == Axes(0, 32767));
	CHECK(PadPlan::Stick(-1.0f, 0.0f, PadPlan::RIGHT_DEADZONE) == Axes(-32767, 0));
	// More than full is full.
	CHECK(PadPlan::Stick(0.0f, -3.0f, PadPlan::LEFT_DEADZONE) == Axes(0, -32767));
}

// The smallest push lies just past the dead zone, so the game reads it; half a push is half of the travel past it.
TEST("virtual pad: stick small push is past the dead zone")
{
	const auto tiny = PadPlan::Stick(0.001f, 0.0f, PadPlan::LEFT_DEADZONE);
	CHECK(tiny.first > PadPlan::LEFT_DEADZONE);
	CHECK(tiny.first < PadPlan::LEFT_DEADZONE + 100);
	const auto half = PadPlan::Stick(0.0f, 0.5f, PadPlan::LEFT_DEADZONE);
	CHECK(half.first == 0);
	CHECK(Near(static_cast<float>(half.second), PadPlan::LEFT_DEADZONE + 0.5f * (32767 - PadPlan::LEFT_DEADZONE), 1.0f));
}

// A diagonal keeps its direction: the dead zone is added along the stick, not to each axis.
TEST("virtual pad: stick diagonal keeps its direction")
{
	const auto stick = PadPlan::Stick(0.3f, 0.3f, PadPlan::LEFT_DEADZONE);
	CHECK(stick.first == stick.second);
	const float pushed = std::hypot(0.3f, 0.3f);
	CHECK(Near(Length(stick), PadPlan::LEFT_DEADZONE + pushed * (32767 - PadPlan::LEFT_DEADZONE), 2.0f));
	// A full diagonal is the edge of the stick's circle, not past it.
	CHECK(Near(Length(PadPlan::Stick(1.0f, 1.0f, PadPlan::LEFT_DEADZONE)), 32767.0f, 2.0f));
}

TEST("virtual pad: trigger is past its threshold")
{
	CHECK(PadPlan::Trigger(0.0f) == 0);
	CHECK(PadPlan::Trigger(-1.0f) == 0);
	CHECK(PadPlan::Trigger(0.01f) > PadPlan::TRIGGER_THRESHOLD);
	CHECK(PadPlan::Trigger(1.0f) == 255);
	CHECK(PadPlan::Trigger(5.0f) == 255);
}

TEST("virtual pad: button names")
{
	CHECK(PadPlan::ButtonMask("a") == std::optional<std::uint16_t>(0x1000));
	CHECK(PadPlan::ButtonMask("rb") == std::optional<std::uint16_t>(0x0200));
	CHECK(!PadPlan::ButtonMask("z"));
	CHECK(PadPlan::ButtonList(0x1000 | 0x0001) == "up+a");
	CHECK(PadPlan::ButtonList(0) == "none");
}

TEST("virtual pad: new pad is neutral")
{
	const PadPlan::Pad pad;
	CHECK(pad.State(START) == PadPlan::Gamepad{});
}

TEST("virtual pad: stick held until set again")
{
	PadPlan::Pad pad;
	pad.SetStick(false, 0.0f, 1.0f, std::nullopt);
	CHECK(pad.State(START + 1h).leftY == 32767);
	pad.SetStick(false, 0.0f, 0.0f, std::nullopt);
	CHECK(pad.State(START).leftY == 0);
}

TEST("virtual pad: timed stick goes back to the middle")
{
	PadPlan::Pad pad;
	pad.SetStick(true, 1.0f, 0.0f, START + 500ms);
	CHECK(pad.State(START + 499ms).rightX == 32767);
	CHECK(pad.State(START + 500ms).rightX == 0);
	CHECK(pad.State(START).leftX == 0);  // the other stick untouched
}

TEST("virtual pad: trigger holds and lets go")
{
	PadPlan::Pad pad;
	pad.SetTrigger(true, 1.0f, START + 200ms);
	pad.SetTrigger(false, 0.5f, std::nullopt);
	CHECK(pad.State(START).rightTrigger == 255);
	CHECK(pad.State(START + 300ms).rightTrigger == 0);
	CHECK(pad.State(START + 300ms).leftTrigger > 0);
}

// A press while a button is down keeps it down until the later of the two.
TEST("virtual pad: button presses overlap")
{
	PadPlan::Pad pad;
	pad.PressButton(0x1000, START + 1s);
	pad.PressButton(0x1000 | 0x2000, START + 100ms);
	CHECK(pad.State(START + 50ms).buttons == (0x1000 | 0x2000));
	CHECK(pad.State(START + 500ms).buttons == 0x1000);
	CHECK(pad.State(START + 1s).buttons == 0);
}

TEST("virtual pad: neutral lets go of everything")
{
	PadPlan::Pad pad;
	pad.SetStick(false, 1.0f, 1.0f, std::nullopt);
	pad.SetTrigger(false, 1.0f, std::nullopt);
	pad.PressButton(0xF000, START + 1h);
	pad.Neutral();
	CHECK(pad.State(START) == PadPlan::Gamepad{});
}

// The packet number moves when, and only when, what is handed over changes.
TEST("virtual pad: packet moves on change")
{
	PadPlan::Packets packets;
	PadPlan::Gamepad state;
	const auto       first = packets.Number(state);
	CHECK(packets.Number(state) == first);
	state.leftY = 100;
	const auto second = packets.Number(state);
	CHECK(second != first);
	CHECK(packets.Number(state) == second);
}

TEST("virtual pad: parse status")
{
	CHECK(ParseOk("").kind == PadPlan::Command::Kind::kStatus);
	CHECK(ParseOk("status").kind == PadPlan::Command::Kind::kStatus);
	CHECK(ParseOk("off").kind == PadPlan::Command::Kind::kOff);
	CHECK(ParseOk("unplug").kind == PadPlan::Command::Kind::kUnplug);
	CHECK(Refused("off now"));
}

TEST("virtual pad: parse stick")
{
	const auto held = ParseOk("stick l 0 1");
	CHECK(held.kind == PadPlan::Command::Kind::kStick);
	CHECK(!held.right);
	CHECK(held.y == 1.0f);
	CHECK(!held.hold);
	const auto timed = ParseOk("  stick right -0.5   0.25 1500 ");
	CHECK(timed.right);
	CHECK(timed.x == -0.5f);
	CHECK(timed.y == 0.25f);
	CHECK(timed.hold == std::optional{ std::chrono::milliseconds(1500) });
	CHECK(Refused("stick"));
	CHECK(Refused("stick l 0"));
	CHECK(Refused("stick m 0 1"));
	CHECK(Refused("stick l 1.5 0"));
	CHECK(Refused("stick l x 0"));
	CHECK(Refused("stick l 0 1 -5"));
	CHECK(Refused("stick l 0 1 100 7"));
	CHECK(Refused("stick l nan 0"));
}

TEST("virtual pad: parse button")
{
	const auto tap = ParseOk("button a");
	CHECK(tap.kind == PadPlan::Command::Kind::kButton);
	CHECK(tap.buttons == 0x1000);
	CHECK(!tap.hold);
	const auto chord = ParseOk("button lb+rb 2000");
	CHECK(chord.buttons == (0x0100 | 0x0200));
	CHECK(chord.hold == std::optional{ std::chrono::milliseconds(2000) });
	CHECK(Refused("button"));
	CHECK(Refused("button q"));
	CHECK(Refused("button a+"));
	CHECK(Refused("button a 999999999"));
}

TEST("virtual pad: parse trigger")
{
	const auto pull = ParseOk("trigger r 1 300");
	CHECK(pull.kind == PadPlan::Command::Kind::kTrigger);
	CHECK(pull.right);
	CHECK(pull.x == 1.0f);
	CHECK(pull.hold == std::optional{ std::chrono::milliseconds(300) });
	CHECK(Refused("trigger r 1.5"));
	CHECK(Refused("trigger r -0.1"));
	CHECK(Refused("trigger r"));
	CHECK(Refused("jump"));
}

TEST("virtual pad: bearing and angles")
{
	CHECK(Near(PadPlan::Bearing(0.0f, 1.0f), 0.0f));
	CHECK(Near(PadPlan::Bearing(1.0f, 0.0f), 90.0f));
	CHECK(Near(PadPlan::Bearing(0.0f, -1.0f), 180.0f));
	CHECK(Near(PadPlan::Bearing(-1.0f, 0.0f), 270.0f));
	CHECK(Near(PadPlan::AngleDiff(10.0f, 350.0f), 20.0f));
	CHECK(Near(PadPlan::AngleDiff(350.0f, 10.0f), -20.0f));
	CHECK(Near(PadPlan::AngleDiff(90.0f, 90.0f), 0.0f));
}

// Facing the target: straight ahead, no turn.
TEST("virtual pad: steer ahead")
{
	const auto sticks = PadPlan::Steer(0.0f, 1000.0f, 0.0f, {});
	CHECK(Near(sticks.leftX, 0.0f));
	CHECK(Near(sticks.leftY, 1.0f));
	CHECK(Near(sticks.rightX, 0.0f));
}

// The target to the right (east while facing north): push right and turn right; to the left, the other way.
TEST("virtual pad: steer sideways")
{
	const auto right = PadPlan::Steer(1000.0f, 0.0f, 0.0f, {});
	CHECK(Near(right.leftX, 1.0f));
	CHECK(Near(right.leftY, 0.0f));
	CHECK(right.rightX > 0.0f);
	const auto left = PadPlan::Steer(-1000.0f, 0.0f, 0.0f, {});
	CHECK(Near(left.leftX, -1.0f));
	CHECK(left.rightX < 0.0f);
	// Facing east, a target to the north is to the left.
	const auto north = PadPlan::Steer(0.0f, 1000.0f, 90.0f, {});
	CHECK(Near(north.leftX, -1.0f));
	CHECK(north.rightX < 0.0f);
}

TEST("virtual pad: steer behind turns the short way")
{
	const auto sticks = PadPlan::Steer(1.0f, -1000.0f, 0.0f, {});
	CHECK(sticks.leftY < -0.99f);
	CHECK(Near(sticks.rightX, PadPlan::SteerSettings{}.turnMax));  // all the way, to the right (the target is a hair east)
}

TEST("virtual pad: steer eases off near the target")
{
	const PadPlan::SteerSettings settings{ .speed = 1.0f, .radius = 100.0f };
	const auto                   far = PadPlan::Steer(0.0f, 1000.0f, 0.0f, settings);
	const auto                   near = PadPlan::Steer(0.0f, 150.0f, 0.0f, settings);
	const auto                   edge = PadPlan::Steer(0.0f, 100.5f, 0.0f, settings);
	CHECK(Near(far.leftY, 1.0f));
	CHECK(near.leftY < far.leftY);
	CHECK(near.leftY > settings.slowest);
	CHECK(Near(edge.leftY, settings.slowest, 0.01f));
	const auto arrived = PadPlan::Steer(0.0f, 99.0f, 0.0f, settings);
	CHECK(arrived.leftX == 0.0f && arrived.leftY == 0.0f && arrived.rightX == 0.0f);
}

TEST("virtual pad: steer speed scales the push")
{
	const auto sticks = PadPlan::Steer(0.0f, 5000.0f, 0.0f, { .speed = 0.5f });
	CHECK(Near(sticks.leftY, 0.5f));
}

TEST("virtual pad: steer small error no turn")
{
	const auto sticks = PadPlan::Steer(20.0f, 1000.0f, 0.0f, {});  // about 1 degree off
	CHECK(sticks.rightX == 0.0f);
	CHECK(sticks.leftX > 0.0f);
}

TEST("virtual pad: walk arrives")
{
	PadPlan::Walk walk{ START, 60s };
	CHECK(walk.Step(START, 1000.0f, 80.0f) == PadPlan::Walk::State::kWalking);
	CHECK(walk.Step(START + 5s, 500.0f, 80.0f) == PadPlan::Walk::State::kWalking);
	CHECK(walk.Step(START + 9s, 70.0f, 80.0f) == PadPlan::Walk::State::kArrived);
	CHECK(walk.Step(START + 10s, 900.0f, 80.0f) == PadPlan::Walk::State::kArrived);  // done is done
	CHECK(walk.Closest() == 70.0f);
}

// No ground gained: stuck, after the first check's grace.
TEST("virtual pad: walk stuck")
{
	PadPlan::Walk walk{ START, 60s };
	CHECK(walk.Step(START, 1000.0f, 80.0f) == PadPlan::Walk::State::kWalking);
	CHECK(walk.Step(START + 7s, 995.0f, 80.0f) == PadPlan::Walk::State::kWalking);  // within the grace
	CHECK(walk.Step(START + 8s, 990.0f, 80.0f) == PadPlan::Walk::State::kStuck);
}

// Steady progress is never stuck; a stop later on is.
TEST("virtual pad: walk stuck after progress")
{
	PadPlan::Walk walk{ START, 60s };
	float         distance = 2000.0f;
	auto          now = START;
	for (int i = 0; i < 20; ++i) {
		CHECK(walk.Step(now, distance, 80.0f) == PadPlan::Walk::State::kWalking);
		now += 1s;
		distance -= 50.0f;
	}
	auto state = PadPlan::Walk::State::kWalking;
	for (int i = 0; i < 10 && state == PadPlan::Walk::State::kWalking; ++i) {
		state = walk.Step(now, distance, 80.0f);
		now += 1s;
	}
	CHECK(state == PadPlan::Walk::State::kStuck);
}

TEST("virtual pad: walk times out")
{
	PadPlan::Walk walk{ START, 10s };
	float         distance = 5000.0f;
	auto          state = PadPlan::Walk::State::kWalking;
	for (auto now = START; state == PadPlan::Walk::State::kWalking; now += 500ms) {
		state = walk.Step(now, distance, 80.0f);
		distance -= 40.0f;
	}
	CHECK(state == PadPlan::Walk::State::kTimedOut);
}

TEST("virtual pad: parse walk targets")
{
	CHECK(WalkOk("").kind == PadPlan::WalkCommand::Kind::kStatus);
	CHECK(WalkOk("stop").kind == PadPlan::WalkCommand::Kind::kStop);
	const auto ref = WalkOk("0001A4C2");
	CHECK(ref.kind == PadPlan::WalkCommand::Kind::kRef);
	CHECK(ref.ref == 0x0001A4C2);
	CHECK(WalkOk("0x14").ref == 0x14);
	CHECK(WalkOk("1a4c2 0.5").ref == 0x1A4C2);
	const auto point = WalkOk("-80345.5 90210");
	CHECK(point.kind == PadPlan::WalkCommand::Kind::kPoint);
	CHECK(point.x == -80345.5f);
	CHECK(point.y == 90210.0f);
	CHECK(point.speed == 1.0f);
	CHECK(point.radius == 80.0f);
}

TEST("virtual pad: parse walk options")
{
	const auto point = WalkOk("100 200 0.5 40 30");
	CHECK(point.speed == 0.5f);
	CHECK(point.radius == 40.0f);
	CHECK(point.timeout == std::chrono::milliseconds(30000));
	const auto ref = WalkOk("0x14 0.3 120");
	CHECK(ref.speed == 0.3f);
	CHECK(ref.radius == 120.0f);
	CHECK(WalkRefused("100"));
	CHECK(WalkRefused("100 y"));
	CHECK(WalkRefused("100 200 1.5"));
	CHECK(WalkRefused("100 200 0"));
	CHECK(WalkRefused("100 200 0.5 -1"));
	CHECK(WalkRefused("100 200 0.5 40 30 9"));
	CHECK(WalkRefused("100 200 0.5 40 9999"));
	CHECK(WalkRefused("stop now"));
}
