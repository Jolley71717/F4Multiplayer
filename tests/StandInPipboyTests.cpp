// A friend in their Pip-Boy or another menu: the kind of menu sent for them (game/MenuKinds.h), the check of the wrist
// their stand-in plays (game/StandInPipboy.h), and the icon over its head (game/MenuIconPlace.h).

#include "Test.h"

#include "Protocol.h"
#include "game/MenuIconPlace.h"
#include "game/MenuKinds.h"
#include "game/OpenMenus.h"
#include "game/StandInPipboy.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <string_view>

namespace
{
	using namespace std::chrono_literals;
	using OpenMenus::Book;
	using enum OpenMenus::Menu;
	namespace MK = Protocol::MenuKind;

	std::uint8_t KindWith(std::initializer_list<std::string_view> a_open)
	{
		Book book;
		for (const auto name : a_open) {
			book.Seen(name, true);
		}
		return MenuKinds::Of(book);
	}
}

TEST("menu kinds: no menu is none, and each menu friends can see us in has its kind")
{
	CHECK(KindWith({}) == MK::kNone);
	CHECK(KindWith({ "PipboyMenu" }) == MK::kPipboy);
	CHECK(KindWith({ "PauseMenu" }) == MK::kPaused);
	CHECK(KindWith({ "Console" }) == MK::kPaused);
	CHECK(KindWith({ "SleepWaitMenu" }) == MK::kPaused);
	CHECK(KindWith({ "SitWaitMenu" }) == MK::kPaused);
	CHECK(KindWith({ "LevelUpMenu" }) == MK::kPaused);
	CHECK(KindWith({ "TerminalMenu" }) == MK::kTerminal);
	CHECK(KindWith({ "TerminalHolotapeMenu" }) == MK::kTerminal);
	CHECK(KindWith({ "HolotapeMenu" }) == MK::kTerminal);
	CHECK(KindWith({ "ContainerMenu" }) == MK::kLooting);
	CHECK(KindWith({ "BarterMenu" }) == MK::kLooting);
	CHECK(KindWith({ "ExamineMenu" }) == MK::kCrafting);
	CHECK(KindWith({ "CookingMenu" }) == MK::kCrafting);
	CHECK(KindWith({ "PowerArmorModMenu" }) == MK::kCrafting);
	CHECK(KindWith({ "WorkshopMenu" }) == MK::kCrafting);
	CHECK(KindWith({ "DialogueMenu" }) == MK::kTalking);
	CHECK(KindWith({ "LockpickingMenu" }) == MK::kOther);
	CHECK(KindWith({ "MessageBoxMenu" }) == MK::kOther);
	CHECK(KindWith({ "BookMenu" }) == MK::kOther);
	CHECK(KindWith({ "LooksMenu" }) == MK::kOther);
}

TEST("menu kinds: VATS is a fight, not a menu")
{
	CHECK(KindWith({ "VATSMenu" }) == MK::kNone);
	CHECK(KindWith({ "VATSMenu", "PipboyMenu" }) == MK::kPipboy);
}

TEST("menu kinds: with several open, the one that says most wins (paused over the Pip-Boy over a terminal over the rest)")
{
	CHECK(KindWith({ "PipboyMenu", "PauseMenu" }) == MK::kPaused);
	CHECK(KindWith({ "PipboyMenu", "Console" }) == MK::kPaused);
	CHECK(KindWith({ "PipboyMenu", "TerminalMenu" }) == MK::kPipboy);
	CHECK(KindWith({ "PipboyMenu", "MessageBoxMenu" }) == MK::kPipboy);
	CHECK(KindWith({ "TerminalMenu", "ContainerMenu" }) == MK::kTerminal);
	CHECK(KindWith({ "ContainerMenu", "WorkshopMenu" }) == MK::kLooting);  // a workbench's own box, in workshop mode
	CHECK(KindWith({ "WorkshopMenu", "MessageBoxMenu" }) == MK::kCrafting);
	CHECK(KindWith({ "DialogueMenu", "BarterMenu" }) == MK::kLooting);  // trading out of a conversation
	CHECK(KindWith({ "DialogueMenu", "MessageBoxMenu" }) == MK::kTalking);
}

TEST("menu kinds: a closed menu no longer counts, and every kind sent is one the protocol takes")
{
	Book book;
	book.Seen("PipboyMenu", true);
	book.Seen("PauseMenu", true);
	CHECK(MenuKinds::Of(book) == MK::kPaused);
	book.Seen("PauseMenu", false);
	CHECK(MenuKinds::Of(book) == MK::kPipboy);
	book.Seen("PipboyMenu", false);
	CHECK(MenuKinds::Of(book) == MK::kNone);
	for (std::size_t i = 0; i < OpenMenus::NAMES.size(); ++i) {
		const auto kind = KindWith({ OpenMenus::NAMES[i] });
		CHECK(kind <= MK::kMax);
	}
	static_assert(MenuKinds::Of([](OpenMenus::Menu a_menu) { return a_menu == kPipboy; }) == MK::kPipboy);
}

namespace
{
	using StandInPipboy::Do;
	using StandInPipboy::Facts;
	using StandInPipboy::Track;
	using StandInPipboy::REPLAY;

	const StandInPipboy::Clock::time_point T0{ std::chrono::seconds(1000) };

	Facts In(std::uint32_t a_standIn = 0xFF001234) { return { .inPipboy = true, .standIn = a_standIn, .ready = true }; }
	Facts Out(std::uint32_t a_standIn = 0xFF001234) { return { .inPipboy = false, .standIn = a_standIn, .ready = true }; }
}

TEST("stand-in Pip-Boy: a friend opening it raises their stand-in's at once, and closing it mid-way lowers it")
{
	Track track;
	CHECK(StandInPipboy::Frame(track, Out(), T0) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, In(), T0) == Do::kRaise);
	CHECK(StandInPipboy::Frame(track, In(), T0 + 16ms) == Do::kNothing);  // once, not each frame
	CHECK(StandInPipboy::Frame(track, In(), T0 + 2s) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, Out(), T0 + 3s) == Do::kLower);
	CHECK(StandInPipboy::Frame(track, Out(), T0 + 3s + 16ms) == Do::kNothing);
	CHECK(track.raises == 1);
	CHECK(track.lowers == 1);
}

TEST("stand-in Pip-Boy: it is raised again each time the last one ends, for as long as they stay in it")
{
	Track track;
	CHECK(StandInPipboy::Frame(track, In(), T0) == Do::kRaise);
	CHECK(StandInPipboy::Frame(track, In(), T0 + REPLAY - 1ms) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, In(), T0 + REPLAY) == Do::kRaise);
	CHECK(StandInPipboy::Frame(track, In(), T0 + REPLAY + 1s) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, In(), T0 + 2 * REPLAY) == Do::kRaise);
	CHECK(track.raises == 3);
	// Closed just as the last one ended: nothing to end.
	CHECK(StandInPipboy::Frame(track, Out(), T0 + 3 * REPLAY) == Do::kNothing);
	CHECK(track.lowers == 0);
}

TEST("stand-in Pip-Boy: a stand-in that can't play it is left alone, and raises afresh once it can")
{
	Track track;
	auto  sitting = In();
	sitting.ready = false;
	CHECK(StandInPipboy::Frame(track, sitting, T0) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, In(), T0 + 1s) == Do::kRaise);
	// Sat down (or got into power armor) mid-way: nothing is ended on it, and closing then lowers nothing.
	CHECK(StandInPipboy::Frame(track, sitting, T0 + 2s) == Do::kNothing);
	auto sittingOut = Out();
	sittingOut.ready = false;
	CHECK(StandInPipboy::Frame(track, sittingOut, T0 + 3s) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, Out(), T0 + 4s) == Do::kNothing);
	CHECK(track.lowers == 0);
	// Up again and still in it: raised at once.
	CHECK(StandInPipboy::Frame(track, In(), T0 + 5s) == Do::kRaise);
}

TEST("stand-in Pip-Boy: a stand-in placed afresh starts with nothing played on it, and with none here nothing is played")
{
	Track track;
	CHECK(StandInPipboy::Frame(track, In(0), T0) == Do::kNothing);
	CHECK(StandInPipboy::Frame(track, In(1), T0) == Do::kRaise);
	CHECK(StandInPipboy::Frame(track, In(2), T0 + 1s) == Do::kRaise);  // a new stand-in mid-raise raises its own
	CHECK(StandInPipboy::Frame(track, Out(3), T0 + 2s) == Do::kNothing);  // and one placed afresh has nothing to lower
	CHECK(StandInPipboy::Frame(track, In(3), T0 + 3s) == Do::kRaise);
	CHECK(StandInPipboy::Frame(track, Out(0), T0 + 4s) == Do::kNothing);  // gone: nothing to play on
	CHECK(StandInPipboy::Frame(track, Out(3), T0 + 4s + 16ms) == Do::kNothing);
}

TEST("stand-in Pip-Boy: each sex plays its own idle")
{
	CHECK(std::string_view(StandInPipboy::RaiseFor(false)) == "IdlePipboyExamineMale");
	CHECK(std::string_view(StandInPipboy::RaiseFor(true)) == "IdlePipboyExamineFemale");
	CHECK(std::string_view(StandInPipboy::LOWER) == "IdleStop");
}

namespace
{
	using MenuIconPlace::Glyph;
	using MenuIconPlace::Screen;
	using MenuIconPlace::Spot;
	using MenuIconPlace::Stage;

	bool Near(float a_lhs, float a_rhs) { return std::abs(a_lhs - a_rhs) < 0.01f; }
}

TEST("menu icon: the Pip-Boy, paused and any other menu each have their glyph, and no menu or one not known has none")
{
	CHECK(MenuIconPlace::GlyphOf(MK::kNone) == Glyph::kNone);
	CHECK(MenuIconPlace::GlyphOf(MK::kPipboy) == Glyph::kPipboy);
	CHECK(MenuIconPlace::GlyphOf(MK::kPaused) == Glyph::kPause);
	for (const auto kind : { MK::kLooting, MK::kTerminal, MK::kCrafting, MK::kTalking, MK::kOther }) {
		CHECK(MenuIconPlace::GlyphOf(kind) == Glyph::kBusy);
	}
	CHECK(MenuIconPlace::GlyphOf(MK::kMax + 1) == Glyph::kNone);
	CHECK(MenuIconPlace::GlyphOf(0xFF) == Glyph::kNone);
}

TEST("menu icon: a spot on the screen lands on the stage, y from the bottom, at full size up close")
{
	const Stage stage;
	const auto  middle = MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, 300.0f, stage);
	CHECK(middle.shown);
	CHECK(Near(middle.x, 640.0f));
	CHECK(Near(middle.y, 360.0f));
	CHECK(Near(middle.scale, 1.0f));
	const auto topLeft = MenuIconPlace::Place({ 0.0f, 1.0f, 0.5f }, 0.0f, stage);
	CHECK(topLeft.shown);
	CHECK(Near(topLeft.x, 0.0f));
	CHECK(Near(topLeft.y, 0.0f));
	// A stage wider than the screen (a wide monitor shows less of it each side, or more): its visible part.
	const auto wide = MenuIconPlace::Place({ 0.25f, 0.25f, 0.5f }, 100.0f, Stage{ -160.0f, 0.0f, 1440.0f, 720.0f });
	CHECK(Near(wide.x, 240.0f));
	CHECK(Near(wide.y, 540.0f));
}

TEST("menu icon: smaller further off, down to a floor, and off beyond the furthest")
{
	const Stage stage;
	CHECK(Near(MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, MenuIconPlace::NEAR, stage).scale, 1.0f));
	CHECK(Near(MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, 2 * MenuIconPlace::NEAR, stage).scale, 0.5f));
	CHECK(Near(MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, MenuIconPlace::MAX_DISTANCE, stage).scale, MenuIconPlace::MIN_SCALE));
	CHECK(!MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, MenuIconPlace::MAX_DISTANCE + 1.0f, stage).shown);
	CHECK(!MenuIconPlace::Place({ 0.5f, 0.5f, 0.9f }, -1.0f, stage).shown);
}

TEST("menu icon: off behind the camera, off the screen, and off for a projection that is not a number")
{
	const Stage stage;
	constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
	constexpr auto inf = std::numeric_limits<float>::infinity();
	for (const Screen screen : { Screen{ 0.5f, 0.5f, 0.0f }, Screen{ 0.5f, 0.5f, -0.3f }, Screen{ 0.5f, 0.5f, 1.0f }, Screen{ 0.5f, 0.5f, 1.7f },
			 Screen{ -0.01f, 0.5f, 0.5f }, Screen{ 1.01f, 0.5f, 0.5f }, Screen{ 0.5f, -0.01f, 0.5f }, Screen{ 0.5f, 1.01f, 0.5f },
			 Screen{ nan, 0.5f, 0.5f }, Screen{ 0.5f, inf, 0.5f }, Screen{ 0.5f, 0.5f, nan } }) {
		CHECK(!MenuIconPlace::Place(screen, 300.0f, stage).shown);
	}
	CHECK(!MenuIconPlace::Place({ 0.5f, 0.5f, 0.5f }, nan, stage).shown);
}

TEST("menu icon: written to the HUD again only once it moved or changed size enough")
{
	const Spot was{ .shown = true, .x = 100.0f, .y = 100.0f, .scale = 1.0f };
	CHECK(!MenuIconPlace::Moved(was, was));
	CHECK(!MenuIconPlace::Moved(was, { .shown = true, .x = 100.2f, .y = 99.8f, .scale = 0.99f }));
	CHECK(MenuIconPlace::Moved(was, { .shown = true, .x = 100.6f, .y = 100.0f, .scale = 1.0f }));
	CHECK(MenuIconPlace::Moved(was, { .shown = true, .x = 100.0f, .y = 99.4f, .scale = 1.0f }));
	CHECK(MenuIconPlace::Moved(was, { .shown = true, .x = 100.0f, .y = 100.0f, .scale = 0.97f }));
}
