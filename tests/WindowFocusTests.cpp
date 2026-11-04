#include "Test.h"

#include "game/WindowFocus.h"

using WindowFocus::Outcome;

TEST("window focus: input goes when the game is in front, or once it has been brought there")
{
	CHECK(WindowFocus::Decide(true, false) == Outcome::kSent);
	CHECK(WindowFocus::Decide(true, true) == Outcome::kSent);
	CHECK(WindowFocus::Decide(false, true) == Outcome::kSentAfterFront);
	CHECK(WindowFocus::Decide(false, false) == Outcome::kDropped);
}

TEST("window focus: the tally counts what was sent and dropped, and names the last one dropped")
{
	WindowFocus::Tally tally;
	CHECK(tally.Describe() == "sent=0 broughtFront=0 dropped=0");
	tally.Record(Outcome::kSent, "key 57");
	tally.Record(Outcome::kSentAfterFront, "key 45");
	CHECK(tally.Describe() == "sent=2 broughtFront=1 dropped=0");
	tally.Record(Outcome::kDropped, "key 09", "pid 4242 'Administrator: Windows PowerShell'");
	tally.Record(Outcome::kDropped, "click", "pid 77 'Notepad'");
	CHECK(tally.Dropped() == 2);
	CHECK(tally.Describe() == "sent=2 broughtFront=1 dropped=2 lastDropped='click' over pid 77 'Notepad'");
}

TEST("window focus: a window title fits a one-line answer")
{
	CHECK(WindowFocus::Printable("Fallout4") == "Fallout4");
	CHECK(WindowFocus::Printable("it's\ttwo\nlines") == "it?s?two?lines");
	CHECK(WindowFocus::Printable(std::string(100, 'x')).size() == 60);
	CHECK(WindowFocus::Printable("").empty());
	CHECK(WindowFocus::Printable("caf\xC3\xA9") == "caf??");
}

TEST("window focus: a click outside the game's window is moved to its middle")
{
	const WindowFocus::Rect screen{ 0, 0, 2560, 1440 };
	CHECK(WindowFocus::Contains(screen, 1919, 360));
	CHECK(WindowFocus::Contains(screen, 0, 0));
	CHECK(!WindowFocus::Contains(screen, 2560, 360));
	CHECK(!WindowFocus::Contains(screen, -5, 360));
	CHECK((WindowFocus::Middle(screen) == std::pair<long, long>{ 1280, 720 }));
	// minimised, then restored without being moved back: parked off every screen, where no click lands
	const WindowFocus::Rect parked{ -32000, -32000, -29440, -30560 };
	CHECK(!WindowFocus::Contains(parked, 1919, 360));
	CHECK((WindowFocus::Middle(parked) == std::pair<long, long>{ -30720, -31280 }));
}
