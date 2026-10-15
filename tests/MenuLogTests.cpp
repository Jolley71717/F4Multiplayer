#include "Test.h"

#include "game/MenuLog.h"

using namespace std::chrono_literals;
using MenuLog::Log;

namespace
{
	const auto START = MenuLog::Clock::time_point{} + 1h;
}

TEST("menu log: a stress cycle's menus open and close and leave nothing open")
{
	Log  log;
	auto now = START;
	log.Event("HUDMenu", true, now);
	for (int cycle = 0; cycle < 3; ++cycle) {
		log.Event("WorkshopMenu", true, now += 1s);
		CHECK(log.IsOpen("WorkshopMenu"));
		CHECK(log.OpenFor("WorkshopMenu", now + 4s) == 4s);
		log.Event("WorkshopMenu", false, now += 6s);
		log.Event("PipboyMenu", true, now += 1s);
		log.Event("PipboyMenu", false, now += 3s);
		log.Event("DialogueMenu", true, now += 2s);
		log.Event("DialogueMenu", false, now += 5s);
	}
	CHECK(!log.IsOpen("WorkshopMenu"));
	CHECK(log.OpenFor("WorkshopMenu", now) == 0s);
	CHECK(log.Opens("WorkshopMenu") == 3);
	CHECK(log.Closes("WorkshopMenu") == 3);
	CHECK(log.Opens("PipboyMenu") == 3);
	CHECK(log.Opens("DialogueMenu") == 3);
	// The HUD is always up in play: never the menu a loop is stuck in.
	CHECK(log.Longest() == nullptr);
	CHECK(log.Describe(now) == "open=HUDMenu:54.0 longest=none events=19 counts=HUDMenu:1/0,WorkshopMenu:3/3,PipboyMenu:3/3,DialogueMenu:3/3");
}

TEST("menu log: a dialogue that never closes is the longest open, counted from its first open")
{
	Log  log;
	auto now = START;
	log.Event("HUDMenu", true, now);
	log.Event("DialogueMenu", true, now += 1s);
	const auto opened = now;
	// The game sends the open again on every exchange: the menu has still been up since the first.
	log.Event("DialogueMenu", true, now += 5s);
	log.Event("PipboyMenu", true, now += 2s);
	log.Event("PipboyMenu", false, now += 1s);
	now += 30s;
	REQUIRE(log.Longest() != nullptr);
	CHECK(log.Longest()->name == "DialogueMenu");
	CHECK(log.OpenFor("DialogueMenu", now) == now - opened);
	CHECK(log.Opens("DialogueMenu") == 2);
	CHECK(log.Describe(now) == "open=HUDMenu:39.0,DialogueMenu:38.0 longest=DialogueMenu:38.0 events=5 counts=HUDMenu:1/0,DialogueMenu:2/0,PipboyMenu:1/1");
}

TEST("menu log: of two menus stuck open, the one open longer is the longest")
{
	Log  log;
	auto now = START;
	log.Event("WorkshopMenu", true, now);
	log.Event("PipboyMenu", true, now += 10s);
	REQUIRE(log.Longest() != nullptr);
	CHECK(log.Longest()->name == "WorkshopMenu");
	log.Event("WorkshopMenu", false, now += 1s);
	REQUIRE(log.Longest() != nullptr);
	CHECK(log.Longest()->name == "PipboyMenu");
}

TEST("menu log: a close the watch never saw open counts, and the menu is closed")
{
	Log  log;
	auto now = START;
	// The watch began on the main menu, which closes when a save loads.
	log.Event("MainMenu", false, now);
	CHECK(!log.IsOpen("MainMenu"));
	CHECK(log.Opens("MainMenu") == 0);
	CHECK(log.Closes("MainMenu") == 1);
	CHECK(log.Describe(now) == "open=none longest=none events=1 counts=MainMenu:0/1");
}

TEST("menu log: names past the cap are counted, not kept")
{
	Log  log;
	auto now = START;
	for (std::size_t i = 0; i < MenuLog::MAX_MENUS + 3; ++i) {
		log.Event(std::format("Menu{}", i), true, now);
	}
	CHECK(log.IsOpen("Menu0"));
	CHECK(!log.IsOpen(std::format("Menu{}", MenuLog::MAX_MENUS)));
	CHECK(log.Describe(now).find(" untracked=3 ") != std::string::npos);
	// A menu already kept is still followed.
	log.Event("Menu0", false, now += 1s);
	CHECK(!log.IsOpen("Menu0"));
}

TEST("menu log: unknown menus are neither open nor counted")
{
	const Log log;
	CHECK(!log.IsOpen("WorkshopMenu"));
	CHECK(log.OpenFor("WorkshopMenu", START) == 0s);
	CHECK(log.Opens("WorkshopMenu") == 0);
	CHECK(log.Longest() == nullptr);
	CHECK(log.Describe(START) == "open=none longest=none events=0 counts=none");
}
