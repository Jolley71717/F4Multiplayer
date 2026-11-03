// Menus a friend's quest stage opens here by itself, closed again (game/CatchUpMenus.h): a dismissed companion's new home
// and a featured item's inspection, which paused a joiner's catch-up until the player answered them.
#include "Test.h"

#include "game/CatchUpMenus.h"
#include "game/MenuMessages.h"
#include "game/OpenMenus.h"

#include <chrono>
#include <utility>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using CatchUpMenus::Action;
	using CatchUpMenus::Close;
	using CatchUpMenus::Guard;
	using CatchUpMenus::Menu;
	using Clock = CatchUpMenus::Clock;

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	constexpr std::pair<Action, Action> NONE{ Action::kNone, Action::kNone };

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

	// A frame as the game side looks: the inspection, then the vignette, each with its own state. The actions, the
	// inspection's first.
	std::pair<Action, Action> Frame(Guard& a_guard, bool a_examine, bool a_vignette, Clock::time_point a_now)
	{
		const auto examine = a_guard.Watch(Menu::kExamine, a_examine, a_now);
		return { examine, a_guard.Watch(Menu::kVignette, a_vignette, a_now) };
	}
}

TEST("catch-up menus: the settlement list is closed by its own cancel, the inspection and its vignette by a hide")
{
	CHECK(CatchUpMenus::HowClosed(Menu::kCaravan) == Close::kCancel);
	CHECK(CatchUpMenus::HowClosed(Menu::kExamine) == Close::kHide);
	CHECK(CatchUpMenus::HowClosed(Menu::kVignette) == Close::kHide);
}

TEST("catch-up menus: the companion list a friend's stage opens is cancelled once, after it settles, and counted as cancelled")
{
	Guard guard;
	guard.StageSet(T0);  // Getting a Clue's 520: Nick becomes our companion, ours is dismissed
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s) == Action::kNone);  // seen opening
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE - 1ms) == Action::kNone);
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE) == Action::kClose);
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + 1s + CatchUpMenus::SETTLE + 100ms) == Action::kNone);  // sent once
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 2s) == Action::kNone);
	CHECK(guard.sent == 1);
	CHECK(guard.cancelled == 1);
	CHECK(guard.hidden == 0);
	CHECK(guard.gaveUp == 0);
}

TEST("catch-up menus: a featured item's inspection a stage opens is hidden, and the hide takes its vignette along")
{
	Guard guard;
	guard.StageSet(T0);  // the Broadsider given
	std::vector<Action> examine, vignette;
	for (auto t = T0 + 200ms; t <= T0 + 1s; t += 100ms) {
		const auto [e, v] = Frame(guard, true, true, t);
		if (e != Action::kNone) {
			examine.push_back(e);
		}
		if (v != Action::kNone) {
			vignette.push_back(v);
		}
	}
	CHECK(examine == std::vector{ Action::kClose });
	CHECK(vignette.empty());  // held back while the inspection is up
	CHECK(Frame(guard, false, false, T0 + 1100ms) == NONE);
	CHECK(guard.sent == 1);
	CHECK(guard.hidden == 1);
	CHECK(guard.cancelled == 0);
	CHECK(guard.left == 0);
}

TEST("catch-up menus: a vignette still up once the inspection was hidden is hidden too, SETTLE after the inspection went")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(Frame(guard, true, true, T0 + 100ms) == NONE);
	CHECK(Frame(guard, true, true, T0 + 100ms + CatchUpMenus::SETTLE).first == Action::kClose);
	const auto gone = T0 + 1s;
	CHECK(Frame(guard, false, true, gone) == NONE);
	CHECK(Frame(guard, false, true, gone + CatchUpMenus::SETTLE - 1ms).second == Action::kNone);  // not on the inspection's heels
	CHECK(Frame(guard, false, true, gone + CatchUpMenus::SETTLE).second == Action::kClose);
	CHECK(Frame(guard, false, false, gone + 1s) == NONE);
	CHECK(guard.hidden == 2);
}

TEST("catch-up menus: a vignette over an inspection left to the player is theirs too, however long they read it")
{
	// Our player picks up a featured item of their own during a session; a friend's stage was set a moment before.
	Guard guard;
	guard.StageSet(T0);
	guard.PlayerActed(T0 + 1s);  // the pickup
	for (auto t = T0 + 1s; t <= T0 + 20s; t += 100ms) {
		CHECK(Frame(guard, true, true, t) == NONE);
	}
	// They press Enter after 20 s: the inspection goes, the vignette lingers a moment. Not judged afresh.
	for (auto t = T0 + 20s; t <= T0 + 25s; t += 100ms) {
		CHECK(Frame(guard, false, true, t) == NONE);
	}
	CHECK(guard.sent == 0);
	CHECK(guard.left == 2);
}

TEST("catch-up menus: a vignette over an inspection whose hide wasn't taken is left with it")
{
	Guard guard;
	guard.StageSet(T0);
	std::vector<Action> examine;
	for (auto t = T0; t <= T0 + 10s; t += 100ms) {
		const auto [e, v] = Frame(guard, true, true, t);
		CHECK(v == Action::kNone);
		if (e != Action::kNone) {
			examine.push_back(e);
		}
	}
	CHECK(examine == (std::vector{ Action::kClose, Action::kGaveUp }));
	for (auto t = T0 + 10s; t <= T0 + 15s; t += 100ms) {
		CHECK(Frame(guard, false, true, t).second == Action::kNone);
	}
	CHECK(guard.gaveUp == 1);
	CHECK(guard.sent == 1);
}

TEST("catch-up menus: a vignette with no inspection under it is judged as a menu of its own")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(Up(guard, Menu::kVignette, T0, T0 + 1s) == std::vector{ Action::kClose });
	CHECK(guard.Watch(Menu::kVignette, false, T0 + 2s) == Action::kNone);
	CHECK(guard.hidden == 1);
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

	// ... but long enough before, the inspection is the stage's again.
	Guard earlier;
	earlier.StageSet(T0 + 10s);
	earlier.PlayerActed(T0 + 10s - CatchUpMenus::PLAYER_QUIET - 1ms);
	CHECK(Up(earlier, Menu::kExamine, T0 + 10s, T0 + 11s) == std::vector{ Action::kClose });
}

TEST("catch-up menus: the settlement list after the player's own conversation is theirs for the stage's whole window, as slow scripts open it")
{
	static_assert(CatchUpMenus::PLAYER_QUIET_SCRIPTED >= CatchUpMenus::AFTER_STAGE);
	CHECK(CatchUpMenus::PlayerQuiet(Menu::kCaravan) == Clock::duration{ CatchUpMenus::AFTER_STAGE });
	CHECK(CatchUpMenus::PlayerQuiet(Menu::kExamine) == Clock::duration{ CatchUpMenus::PLAYER_QUIET });
	CHECK(CatchUpMenus::PlayerQuiet(Menu::kVignette) == Clock::duration{ CatchUpMenus::PLAYER_QUIET });

	// A companion dismissed in a conversation that ended 20 s before the list opened, on a game behind on its scripts,
	// while a friend's stage was set 25 s before (live play).
	Guard slow;
	slow.StageSet(T0);
	slow.PlayerActed(T0 + 5s);
	CHECK(Up(slow, Menu::kCaravan, T0 + 25s, T0 + 30s).empty());
	CHECK(slow.left == 1);

	// The inspection in the same spot is the stage's: the engine opens the player's own on the spot.
	Guard examine;
	examine.StageSet(T0);
	examine.PlayerActed(T0 + 5s);
	CHECK(Up(examine, Menu::kExamine, T0 + 25s, T0 + 26s) == std::vector{ Action::kClose });

	// A list long after the player's last act is the stage's.
	Guard quiet;
	quiet.StageSet(T0 + 40s);
	quiet.PlayerActed(T0);
	CHECK(Up(quiet, Menu::kCaravan, T0 + 41s, T0 + 42s) == std::vector{ Action::kClose });
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
	CHECK(guard.LastPlayer() == T0 + 2s);
	CHECK(Up(guard, Menu::kExamine, T0 + 3s, T0 + 5s).empty());
}

TEST("catch-up menus: a close the menu doesn't take is given up once, the menu left to the player and not counted as closed")
{
	Guard guard;
	guard.StageSet(T0);
	const auto actions = Up(guard, Menu::kCaravan, T0, T0 + 20s);
	CHECK(actions == (std::vector{ Action::kClose, Action::kGaveUp }));
	CHECK(guard.gaveUp == 1);
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 21s) == Action::kNone);
	CHECK(guard.cancelled == 0);  // the player answered it, not us
	CHECK(guard.hidden == 0);
}

TEST("catch-up menus: a close that couldn't be sent leaves the menu to the player, uncounted as sent, and never asks again")
{
	Guard guard;
	guard.StageSet(T0);
	CHECK(guard.Watch(Menu::kCaravan, true, T0) == Action::kNone);
	CHECK(guard.Watch(Menu::kCaravan, true, T0 + CatchUpMenus::SETTLE) == Action::kClose);
	guard.NotSent(Menu::kCaravan);  // the list took no input
	CHECK(guard.sent == 0);
	CHECK(guard.gaveUp == 1);
	CHECK(Up(guard, Menu::kCaravan, T0 + 1s, T0 + 10s).empty());
	CHECK(guard.Watch(Menu::kCaravan, false, T0 + 11s) == Action::kNone);
	CHECK(guard.cancelled == 0);
	guard.NotSent(Menu::kCaravan);  // nothing pending: nothing changes
	CHECK(guard.gaveUp == 1);

	// An inspection's hide not sent: its vignette is left with it.
	Guard examine;
	examine.StageSet(T0);
	CHECK(Frame(examine, true, true, T0) == NONE);
	CHECK(Frame(examine, true, true, T0 + CatchUpMenus::SETTLE).first == Action::kClose);
	examine.NotSent(Menu::kExamine);
	CHECK(Frame(examine, false, true, T0 + 1s).second == Action::kNone);
	CHECK(Frame(examine, false, true, T0 + 5s).second == Action::kNone);
	CHECK(examine.left == 1);
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

TEST("catch-up menus: the menus are watched by the game's own names, which the open-menu book knows; a hide is refused exactly for the ones closed by cancel")
{
	for (std::size_t i = 0; i < CatchUpMenus::NAMES.size(); ++i) {
		const auto name = CatchUpMenus::NAMES[i];
		bool       known = false;
		for (const auto book : OpenMenus::NAMES) {
			known = known || OpenMenus::SameName(name, book);
		}
		CHECK(known);
		const bool cancel = CatchUpMenus::HowClosed(static_cast<Menu>(i)) == Close::kCancel;
		CHECK((MenuMessages::Refusal(name, true) != nullptr) == cancel);
	}
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kCaravan)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kCaravan)]);
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kExamine)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kExamine)]);
	CHECK(OpenMenus::NAMES[static_cast<std::size_t>(OpenMenus::Menu::kVignette)] == CatchUpMenus::NAMES[static_cast<std::size_t>(Menu::kVignette)]);
}
