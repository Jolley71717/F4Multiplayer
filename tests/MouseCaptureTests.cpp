#include "Test.h"

#include "game/MouseCapture.h"

#include <vector>

namespace
{
	using MouseCapture::Registration;
	constexpr std::uintptr_t kGame = 0x1234;
	constexpr std::uintptr_t kOther = 0x5678;
	const auto               gone = [](std::uintptr_t) { return false; };  // no window but the game's is live
	const auto               live = [](std::uintptr_t) { return true; };   // every window is live
}

TEST("mouse capture: a game without a mouse registration gets one for its window")
{
	const std::vector<Registration> none;
	const auto                      fix = MouseCapture::Fix(none, kGame, gone);
	CHECK(fix.has_value());
	CHECK(fix && fix->page == 1 && fix->usage == 2 && fix->flags == 0 && fix->target == kGame);

	const std::vector<Registration> keysOnly{ { 1, 6, 0, kGame } };  // a keyboard: not the mouse
	CHECK(MouseCapture::Fix(keysOnly, kGame, gone).has_value());

	const std::vector<Registration> removed{ { 1, 2, MouseCapture::kRemove, kGame } };
	CHECK(MouseCapture::Fix(removed, kGame, gone).has_value());
}

TEST("mouse capture: a registration for a window that is gone is moved to the game's, keeping its flags")
{
	const std::vector<Registration> other{ { 1, 6, 0, kGame }, { 1, 2, 0x30, kOther } };
	const auto                      fix = MouseCapture::Fix(other, kGame, gone);
	CHECK(fix && fix->target == kGame && fix->flags == 0x30);
}

TEST("mouse capture: a registration for another live window of the game is left alone")
{
	// the window found for the game may not be the one it registered: never take the mouse off a live one
	const std::vector<Registration> other{ { 1, 2, 0x30, kOther } };
	CHECK(!MouseCapture::Fix(other, kGame, live));
	std::uintptr_t asked = 0;
	CHECK(!MouseCapture::Fix(other, kGame, [&](std::uintptr_t a_window) { asked = a_window; return true; }));
	CHECK(asked == kOther);  // it is the registered target whose life is asked
}

TEST("mouse capture: a working registration is left alone")
{
	const std::vector<Registration> game{ { 1, 2, 0x30, kGame } };
	CHECK(!MouseCapture::Fix(game, kGame, gone));
	const std::vector<Registration> focus{ { 1, 2, 0, 0 } };  // follows the keyboard focus
	CHECK(!MouseCapture::Fix(focus, kGame, gone));
	CHECK(!MouseCapture::Fix(std::vector<Registration>{}, 0, gone));  // no window found: nothing to register for
}

TEST("mouse capture: registrations described for a report")
{
	CHECK(MouseCapture::Describe(std::vector<Registration>{}, kGame) == "none");
	const std::vector<Registration> list{ { 1, 2, 0x30, kGame }, { 1, 6, 0, 0 }, { 1, 2, 0, kOther } };
	CHECK(MouseCapture::Describe(list, kGame) == "1:2 flags=0x30 to=game,1:6 flags=0x0 to=focus,1:2 flags=0x0 to=other");
}
