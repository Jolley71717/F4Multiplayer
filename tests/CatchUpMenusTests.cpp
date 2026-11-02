// Menus a friend's quest stage opens here by itself, closed again (game/CatchUpMenus.h): a dismissed companion's new home
// and a featured item's inspection, which paused a joiner's catch-up until the player answered them.
#include "Test.h"

#include "game/CatchUpMenus.h"
#include "game/MenuMessages.h"
#include "game/OpenMenus.h"

#include <chrono>

namespace
{
	using namespace std::chrono_literals;
	using CatchUpMenus::Action;
	using CatchUpMenus::Guard;
	using CatchUpMenus::Menu;
	using Clock = CatchUpMenus::Clock;

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	// Watches a_menu up from a_from to a_to, a look every 100 ms; the actions it got, in order (kNone left out).
	std::vector<Action> Up(Guard& a_guard, Menu a_menu, Clock::time_point a_from, Clock::time_point a_to)
	{
		std::vector<Action> out;
		for (auto t = a_from; t <= a_to; t += 100ms) {
			if (const auto action = a_guard.Watch(a_menu, true, t); action != Action::kNone) {
				out.push_back(action);
			}
		}
		return out;
	}
}

TEST("catch-up menus: the companion list a friend's stage opens is closed once, after it settles")
{
	Guard guard;
	guard.StageSet(T0);  // Getting a Clue's 520: Nick becomes our companion, ours is dismissed
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s) == Action::kNone);  // seen opening
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE - 1ms) == Action::kNone);
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE) == Action::kClose);
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE + 100ms) == Action::kNone);  // sent once
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 2s) == Action::kNone);
	CHECK(guard.sent == 1);
	CHECK(guard.closed == 1);
	CHECK(guard.gaveUp == 0);
}

TEST("catch-up menus: a featured item's inspection a stage opens is closed, and its vignette is judged as a menu of its own")
{
	Guard guard;
	guard.StageSet(T0);  // the Broadsider given
	CHECK(Up(guard, Menu::kExamine, T0 + 200ms, T0 + 1s) == std::vector{ Action::kClose });
	CHECK(guard.Watch(Menu::kExamine, false, T0 + 1100ms) == Action::kNone);
	// The vignette still up once the inspection went: closed too.
	CHECK(Up(guard, Menu::kVignette, T0 + 1100ms, T0 + 2s) == std::vector{ Action::kClose });
	CHECK(guard.Watch(Menu::kVignette, false, T0 + 2100ms) == Action::kNone);
	CHECK(guard.closed == 2);
}

TEST("catch-up menus: a menu with no friend's stage before it is the player's")
{
	Guard guard;
	CHECK(Up(guard, Menu::kExamine, T0, T0 + 10s).empty());
	CHECK(guard.left == 1);
	CHECK(guard.sent == 0);
}

TEST("catch-up menus: a menu that opens long after the last stage is the player's; one at the edge of the window is the stage's")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(Up(guard, Menu::kCaravan, T0 + CatchUpMenus::AFTER_STAGE + 1s, T0 + CatchUpMenus::AFTER_STAGE + 3s).empty());
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + CatchUpMenus::AFTER_STAGE + 4s) == Action::kNone);

	Guard edge;
	edge.StageSet(T0);
	CHECK(Up(edge, Menu::kCaravan, T0 + CatchUpMenus::AFTER_STAGE, T0 + CatchUpMenus::AFTER_STAGE + 1s) == std::vector{ Action::kClose });
}

TEST("catch-up menus: a stage set after the menu opened didn't open it")
{
	Guard guard;
	CHECK(guard.Watch(Menu::kExamine, true, T0) == Action::kNone);
	guard.StageSet(T0 + 2s);
	CHECK(Up(guard, Menu::kExamine, T0 + 2s, T0 + 5s).empty());
	CHECK(guard.left == 1);
}

TEST("catch-up menus: the player's own activation just before, or a menu of theirs, keeps the menu theirs")
{
	// A workbench (or a bobblehead) activated during a catch-up.
	Guard workbench;
	workbench.StageSet(T0);
	workbench.PlayerActed(T0 + 1s);
	CHECK(Up(workbench, Menu::kExamine, T0 + 1200ms, T0 + 5s).empty());

	// A companion dismissed in a conversation that ended a moment before the list opened.
	Guard talk;
	talk.StageSet(T0);
	talk.PlayerActed(T0 + 3s);
	CHECK(Up(talk, Menu::kCaravan, T0 + 3s + CatchUpMenus::PLAYER_QUIET, T0 + 10s).empty());

	// ... but long enough before, it is the stage's again.
	Guard earlier;
	earlier.StageSet(T0 + 10s);
	earlier.PlayerActed(T0);
	CHECK(Up(earlier, Menu::kCaravan, T0 + 10s + 1ms, T0 + 11s) == std::vector{ Action::kClose });
}

TEST("catch-up menus: the player's activation heard just after the menu opened (before it settles) keeps it theirs")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(guard.Watch(Menu::kExamine, true, T0 + 1s) == Action::kNone);
	guard.PlayerActed(T0 + 1200ms);  // the activation's event came after the menu's
	CHECK(Up(guard, Menu::kExamine, T0 + 1100ms, T0 + 5s).empty());
}

TEST("catch-up menus: an older activation never moves the latest one back")
{
	Guard guard;
	guard.StageSet(T0);
	guard.PlayerActed(T0 + 2s);
	guard.PlayerActed(T0 - 1h);  // a late, older one
	CHECK(Up(guard, Menu::kExamine, T0 + 3s, T0 + 5s).empty());
}

TEST("catch-up menus: a close the menu doesn't take is given up once, and the menu is left to the player")
{
	Guard guard;
	guard.StageSet(T0);
	const auto actions = Up(guard, Menu::kExamine, T0, T0 + 20s);
	CHECK(actions == (std::vector{ Action::kClose, Action::kGaveUp }));
	CHECK(guard.gaveUp == 1);
	CHECK(guard.closed == 0);
	CHECK(guard.Watch(Menu::kExamine, false, T0 + 21s) == Action::kNone);
	CHECK(guard.closed == 0);  // the player closed it, not us
}

TEST("catch-up menus: each opening is judged afresh")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(Up(guard, Menu::kCaravan, T0, T0 + 1s) == std::vector{ Action::kClose });
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 2s) == Action::kNone);
	// Opened again by the player later, with no stage since: theirs.
	CHECK(Up(guard, Menu::kCaravan, T0 + 60s, T0 + 62s).empty());
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 63s) == Action::kNone);
	// A new stage, a new list (a second companion change): closed.
	guard.StageSet(T0 + 70s);
	CHECK(Up(guard, Menu::kCaravan, T0 + 71s, T0 + 72s) == std::vector{ Action::kClose });
	CHECK(guard.sent == 2);
}

TEST("catch-up menus: the menus are watched and closed by the game's own names, which the open-menu book knows and no rule refuses")
{
	for (const auto name : CatchUpMenus::NAMES) {
		bool known = false;
		for (const auto book : OpenMenus::NAMES) {
			known = known || OpenMenus::SameName(name, book);
		}
		CHECK(known);
		CHECK(MenuMessages::Refusal(name, true) == nullptr);
	}
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kCaravan)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kCaravan)]);
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kExamine)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kExamine)]);
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kVignette)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kVignette)]);
}
