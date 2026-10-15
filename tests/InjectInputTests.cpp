#include "Test.h"

#include "game/InjectInput.h"

using namespace std::chrono_literals;
using InjectInput::Device;
using InjectInput::Press;
using InjectInput::Releases;

namespace
{
	const auto START = InjectInput::Clock::time_point{} + 1h;

	Press ParseOk(std::string_view a_line)
	{
		auto parsed = InjectInput::Parse(a_line);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			Test::Fail(__FILE__, __LINE__, std::format("'{}' did not parse: {}", a_line, *error));
			throw Test::Abort{};
		}
		return std::get<Press>(parsed);
	}

	bool Refused(std::string_view a_line)
	{
		return std::holds_alternative<std::string>(InjectInput::Parse(a_line));
	}

	// The release plan as the input command and the frame use it: a press of the line now, and the
	// releases that go before it.
	std::vector<Press> PressNow(Releases& a_releases, std::string_view a_line, InjectInput::Clock::time_point a_now)
	{
		return a_releases.Pressed(ParseOk(a_line), a_now);
	}

	std::vector<std::string> Names(const std::vector<Press>& a_presses)
	{
		std::vector<std::string> out;
		for (const auto& press : a_presses) {
			out.push_back(press.userEvent);
		}
		return out;
	}
}

TEST("inject input: a bare user event is a short key press the control map names")
{
	const auto press = ParseOk("Activate");
	CHECK(press.userEvent == "Activate");
	CHECK(press.device == Device::kKeyboard);
	CHECK(!press.id.has_value());
	CHECK(press.hold == InjectInput::DEFAULT_HOLD);
}

TEST("inject input: options come in any order, with extra spaces")
{
	const auto press = ParseOk("  Up   id=0xC8 hold=250  device=key ");
	CHECK(press.userEvent == "Up");
	CHECK(press.id == 0xC8u);
	CHECK(press.hold == 250ms);
	const auto click = ParseOk("Accept device=mouse id=0");
	CHECK(click.device == Device::kMouse);
	CHECK(click.id == 0u);
	CHECK(ParseOk("Cancel device=pad hold=0").device == Device::kGamepad);
}

TEST("inject input: a malformed line is refused with what is wrong")
{
	CHECK(Refused(""));
	CHECK(Refused("   "));
	CHECK(Refused("hold=100"));  // no event
	CHECK(Refused("Up hold=abc"));
	CHECK(Refused("Up hold=-5"));
	CHECK(Refused("Up hold=10001"));  // a stuck key for minutes is never a test
	CHECK(Refused("Up hold="));
	CHECK(Refused("Up device=joystick"));
	CHECK(Refused("Up id=zz"));
	CHECK(Refused("Up id="));
	CHECK(Refused("Up hold=10 hold=20"));  // said twice: which one?
	CHECK(Refused("Up Down"));  // one event a line
	const auto parsed = InjectInput::Parse("Up hold=abc");
	REQUIRE(std::holds_alternative<std::string>(parsed));
	CHECK(std::get<std::string>(parsed).find("hold") != std::string::npos);
}

TEST("inject input: a press is released once its hold is over, not before")
{
	Releases releases;
	CHECK(PressNow(releases, "Activate hold=100", START).empty());
	CHECK(releases.Waiting() == 1);
	CHECK(releases.Due(START + 50ms).empty());
	CHECK(releases.Due(START + 99ms).empty());
	const auto due = releases.Due(START + 116ms);  // the first frame after the hold
	REQUIRE(due.size() == 1);
	CHECK(due[0].userEvent == "Activate");
	CHECK(due[0].hold == 100ms);  // the release says how long it was held
	CHECK(releases.Waiting() == 0);
	CHECK(releases.Due(START + 1s).empty());  // released once only
}

TEST("inject input: presses held together are released in the order their holds end")
{
	Releases releases;
	CHECK(PressNow(releases, "Forward hold=500", START).empty());
	CHECK(PressNow(releases, "Activate hold=100", START + 10ms).empty());
	CHECK(PressNow(releases, "Up hold=50", START + 20ms).empty());
	// A slow frame: all three are over by the time it comes.
	CHECK((Names(releases.Due(START + 2s)) == std::vector<std::string>{ "Up", "Activate", "Forward" }));
}

TEST("inject input: pressing a held button again releases it first")
{
	Releases releases;
	CHECK(PressNow(releases, "Up hold=1000", START).empty());
	CHECK(PressNow(releases, "Down hold=1000", START).empty());
	// The same event on another device is another button: left alone.
	CHECK(PressNow(releases, "Up device=pad hold=1000", START).empty());
	const auto first = PressNow(releases, "Up hold=100", START + 200ms);
	REQUIRE(first.size() == 1);
	CHECK(first[0].userEvent == "Up");
	CHECK(first[0].device == Device::kKeyboard);
	CHECK(first[0].hold == 1000ms);
	CHECK(releases.Waiting() == 3);
	// The new press keeps its own, shorter hold.
	const auto due = releases.Due(START + 300ms);
	REQUIRE(due.size() == 1);
	CHECK(due[0].userEvent == "Up");
	CHECK(due[0].device == Device::kKeyboard);
	CHECK((Names(releases.Due(START + 1s)) == std::vector<std::string>{ "Down", "Up" }));
}

TEST("inject input: a button pressed with its code is another button than the same event's control map key")
{
	Releases releases;
	CHECK(PressNow(releases, "Up id=11 hold=1000", START).empty());
	CHECK(PressNow(releases, "Up id=C8 hold=1000", START).empty());
	CHECK(releases.Waiting() == 2);
	CHECK(PressNow(releases, "Up id=11 hold=10", START + 1ms).size() == 1);
}
