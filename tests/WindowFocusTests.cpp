#include "Test.h"

#include "game/WindowFocus.h"

#include <set>

using WindowFocus::Outcome;
using WindowFocus::Spot;

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
	tally.Record(Outcome::kDropped, "key 09", "powershell [ConsoleWindowClass]");
	tally.Record(Outcome::kDropped, "click", "WerFault [#32770] (kept on top)");
	CHECK(tally.Dropped() == 2);
	CHECK(tally.Describe() == "sent=2 broughtFront=1 dropped=2 lastDropped='click' over WerFault [#32770] (kept on top)");
}

TEST("window focus: text fits a one-line answer")
{
	CHECK(WindowFocus::Printable("Fallout4") == "Fallout4");
	CHECK(WindowFocus::Printable("it's\ttwo\nlines") == "it?s?two?lines");
	CHECK(WindowFocus::Printable(std::string(100, 'x')).size() == 60);
	CHECK(WindowFocus::Printable("").empty());
	CHECK(WindowFocus::Printable("caf\xC3\xA9") == "caf??");
}

TEST("window focus: a window is named by its process and class, never its title")
{
	// the process is its image's file name without .exe; a window kept on top of every other one says so
	CHECK(WindowFocus::WindowName("C:\\Windows\\System32\\WerFault.exe", 4242, "#32770", true) == "WerFault [#32770] (kept on top)");
	CHECK(WindowFocus::WindowName("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.EXE", 7, "ConsoleWindowClass", false) == "powershell [ConsoleWindowClass]");
	CHECK(WindowFocus::WindowName("/mnt/c/app.exe", 7, "X", false) == "app [X]");
	// an image that could not be read (a protected process): the pid stands in
	CHECK(WindowFocus::WindowName("", 1234, "Shell_TrayWnd", false) == "pid 1234 [Shell_TrayWnd]");
	// a name with no .exe, and one of only ".exe", are kept whole
	CHECK(WindowFocus::WindowName("csrss", 1, "C", false) == "csrss [C]");
	CHECK(WindowFocus::WindowName(".exe", 1, "C", false) == ".exe [C]");
	// the class is made printable, like everything in an answer
	CHECK(WindowFocus::WindowName("x.exe", 1, "it's\n", false) == "x [it?s?]");
}

TEST("window focus: the points a click is tried at are the middle first, then a grid off the edges")
{
	const WindowFocus::Rect client{ 100, 50, 1100, 550 };  // 1000 x 500
	const auto              spots = WindowFocus::ClickSpots(client);
	CHECK(spots.size() == 25);
	CHECK((spots[0] == std::pair<long, long>{ 600, 300 }));
	std::set<std::pair<long, long>> distinct;
	for (const auto& [x, y] : spots) {
		CHECK(x >= 300 && x <= 900);  // 20% in from every edge at least: a taskbar over the bottom edge, a border beside it
		CHECK(y >= 150 && y <= 450);
		CHECK(WindowFocus::Contains(client, x, y));
		distinct.insert({ x, y });
	}
	CHECK(distinct.size() == 25);
}

TEST("window focus: a click's cursor stays on its own free point, moves to a free one, or the click is refused")
{
	const WindowFocus::Rect client{ 0, 0, 2560, 1440 };
	const auto              free = [](long, long) { return true; };
	// the cursor's own point on the client area with the game under it: kept
	auto found = WindowFocus::FindSpot(client, 1919, 360, free);
	CHECK(found.spot == Spot::kKept);
	CHECK(found.x == 1919 && found.y == 360);
	// the cursor off the client area (another screen, beside a smaller game window): the middle, which is free
	found = WindowFocus::FindSpot(client, 2560, 360, free);
	CHECK(found.spot == Spot::kMoved);
	CHECK(found.x == 1280 && found.y == 720);
	found = WindowFocus::FindSpot(client, -5, 360, free);
	CHECK(found.spot == Spot::kMoved);
	// an error box over the middle of the screen (and the cursor): the first grid point outside it
	const WindowFocus::Rect box{ 1000, 500, 1560, 940 };
	const auto              outsideBox = [&](long a_x, long a_y) { return !WindowFocus::Contains(box, a_x, a_y); };
	found = WindowFocus::FindSpot(client, 1280, 720, outsideBox);
	CHECK(found.spot == Spot::kMoved);
	CHECK(!WindowFocus::Contains(box, found.x, found.y));
	CHECK(WindowFocus::Contains(client, found.x, found.y));
	CHECK(found.x == 896 && found.y == 720);  // 35% across, 50% down: the first of the grid after the middle
	// a window over every point: refused, the cursor left where it was
	int asked = 0;
	const auto none = [&](long, long) { ++asked; return false; };
	found = WindowFocus::FindSpot(client, 1280, 720, none);
	CHECK(found.spot == Spot::kCovered);
	CHECK(found.x == 1280 && found.y == 720);
	CHECK(asked == 26);  // the cursor's point, then every grid point
	// the cursor outside the client area is never kept, whatever is under it
	asked = 0;
	found = WindowFocus::FindSpot(client, 3000, 3000, none);
	CHECK(found.spot == Spot::kCovered);
	CHECK(asked == 25);
	// minimised, then restored without being moved back: parked off every screen (the caller refuses before any search)
	const WindowFocus::Rect parked{ -32000, -32000, -29440, -30560 };
	CHECK(!WindowFocus::Contains(parked, 1919, 360));
}
