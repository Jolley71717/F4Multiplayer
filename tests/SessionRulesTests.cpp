// The session's rules (Protocol::SessionRules) as the host's ini sets them (src/game/HostRules.h), which menus run in
// real time by them (src/game/RealTimeMenus.h), and the tutorial hints held off in a session (src/Background.h).

#include "Test.h"

#include "Background.h"
#include "game/HostRules.h"
#include "game/RealTimeMenus.h"

#include <set>

TEST("session rules: an ini with none of the keys hosts with the defaults, which are the mod's behaviour before rules")
{
	const Config::Settings settings;
	const auto             rules = HostRules::FromSettings(settings);
	CHECK(rules == Protocol::SessionRules{});
	CHECK(rules.revive);
	CHECK(rules.bleedOutSeconds == 45);
	CHECK(rules.helpTenths == 20);
	CHECK(rules.reviveHealth == 30);
	CHECK(rules.maxRevives == 0);
	CHECK(rules.reviveWindowMinutes == 0);
	CHECK(rules.downCooldownSeconds == 0);
	CHECK(!rules.reviveOutsideCombat);
	CHECK(rules.realTimeVats);
	CHECK(rules.realTimePipboy);
	CHECK(rules.realTimeMenus);
}

TEST("session rules: the host's keys each set their rule, within the limits")
{
	Config::Settings settings;
	settings.revive = false;
	settings.bleedOutSeconds = 120;
	settings.reviveSeconds = 3.5f;
	settings.reviveHealth = 50;
	settings.maxRevives = 3;
	settings.reviveWindowMinutes = 20;
	settings.reviveCooldownSeconds = 90;
	settings.reviveOutOfCombat = true;
	settings.realTimeVats = false;
	settings.realTimePipboy = false;
	settings.realTimeMenus = false;
	const auto rules = HostRules::FromSettings(settings);
	CHECK(!rules.revive);
	CHECK(rules.bleedOutSeconds == 120);
	CHECK(rules.helpTenths == 35);
	CHECK(rules.reviveHealth == 50);
	CHECK(rules.maxRevives == 3);
	CHECK(rules.reviveWindowMinutes == 20);
	CHECK(rules.downCooldownSeconds == 90);
	CHECK(rules.reviveOutsideCombat);
	CHECK(!rules.realTimeVats);
	CHECK(!rules.realTimePipboy);
	CHECK(!rules.realTimeMenus);

	settings.bleedOutSeconds = 1;
	settings.reviveHealth = 400;
	settings.maxRevives = 1000;
	settings.reviveWindowMinutes = 100000;
	settings.reviveCooldownSeconds = 100000;
	const auto wild = HostRules::FromSettings(settings);
	CHECK(wild.bleedOutSeconds == Protocol::SessionRules::MIN_BLEED_OUT);
	CHECK(wild.reviveHealth == 100);
	CHECK(wild.maxRevives == 255);
	CHECK(wild.reviveWindowMinutes == Protocol::SessionRules::MAX_REVIVE_WINDOW);
	CHECK(wild.downCooldownSeconds == Protocol::SessionRules::MAX_DOWN_COOLDOWN);
	CHECK(!HostRules::Describe(wild).empty());
}

TEST("session rules: the Pip-Boy and the menus run in real time only in a session, each group by its own rule")
{
	using RealTimeMenus::Group;
	Protocol::SessionRules rules;
	CHECK(RealTimeMenus::GroupOf("PipboyMenu") == Group::kPipboy);
	CHECK(RealTimeMenus::GroupOf("LevelUpMenu") == Group::kPipboy);
	CHECK(RealTimeMenus::GroupOf("containermenu") == Group::kMenus);  // the game's names are case-blind
	CHECK(RealTimeMenus::GroupOf("TerminalMenu") == Group::kMenus);
	CHECK(RealTimeMenus::GroupOf("MessageBoxMenu") == Group::kMenus);
	// Never: the pause menu, the console, waiting, the looks menu, the loading and main menus.
	for (const auto name : { "PauseMenu", "Console", "SleepWaitMenu", "LooksMenu", "LoadingMenu", "MainMenu", "HUDMenu", "VATSMenu", "" }) {
		CHECK(RealTimeMenus::GroupOf(name) == Group::kNone);
	}

	CHECK(!RealTimeMenus::RealTime(Group::kPipboy, false, rules));  // alone: as the game always was
	CHECK(!RealTimeMenus::RealTime(Group::kMenus, false, rules));
	CHECK(RealTimeMenus::RealTime(Group::kPipboy, true, rules));
	CHECK(RealTimeMenus::RealTime(Group::kMenus, true, rules));
	CHECK(!RealTimeMenus::RealTime(Group::kNone, true, rules));
	rules.realTimePipboy = false;
	CHECK(!RealTimeMenus::RealTime(Group::kPipboy, true, rules));
	CHECK(RealTimeMenus::RealTime(Group::kMenus, true, rules));
	rules.realTimeMenus = false;
	rules.realTimePipboy = true;
	CHECK(RealTimeMenus::RealTime(Group::kPipboy, true, rules));
	CHECK(!RealTimeMenus::RealTime(Group::kMenus, true, rules));
}

TEST("session rules: a real-time menu loses only the flags that stop the game")
{
	constexpr std::uint32_t pipboy = 0x0118'C5C1;  // pauses, uses the cursor, freeze-frame pause among others
	const auto              out = RealTimeMenus::RealTimeFlags(pipboy | RealTimeMenus::FREEZE_FRAME_PAUSE);
	CHECK((out & RealTimeMenus::PAUSES_GAME) == 0);
	CHECK((out & RealTimeMenus::FREEZE_FRAME_PAUSE) == 0);
	CHECK(out == ((pipboy | RealTimeMenus::FREEZE_FRAME_PAUSE) & ~(RealTimeMenus::PAUSES_GAME | RealTimeMenus::FREEZE_FRAME_PAUSE)));
	CHECK(RealTimeMenus::RealTimeFlags(0x0081'8400) == 0x0081'8400);  // VATS's menu stops nothing: left as it is
}

TEST("session rules: each real-time menu is one the open-menu book knows, by the same name, once")
{
	std::set<OpenMenus::Menu> seen;
	for (const auto& entry : RealTimeMenus::MENUS) {
		CHECK(entry.group != RealTimeMenus::Group::kNone);
		CHECK(OpenMenus::SameName(OpenMenus::NAMES[static_cast<std::size_t>(entry.open)], entry.menu));
		CHECK(seen.insert(entry.open).second);
	}
	// Open, any of them keeps the hotkeys off (they did while it stopped the game).
	CHECK(!RealTimeMenus::AnyOpen([](OpenMenus::Menu) { return false; }));
	CHECK(RealTimeMenus::AnyOpen([](OpenMenus::Menu a_menu) { return a_menu == OpenMenus::Menu::kContainer; }));
	CHECK(!RealTimeMenus::AnyOpen([](OpenMenus::Menu a_menu) { return a_menu == OpenMenus::Menu::kPause; }));
}

TEST("session rules: VATS runs at full speed in a session with the rule on, but Jet's slow motion stays")
{
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, true, true, 0.05f, 0.0f) == 1.0f);
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, true, true, 0.001f, 0.0f) == 1.0f);  // a shot playing out
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, true, true, 1.0f, 0.0f) < 0.0f);     // already: nothing to do
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, true, true, 0.5f, 0.8f) < 0.0f);     // on Jet
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, true, false, 0.5f, 0.0f) < 0.0f);    // no VATS: the console's sgtm, a bow
	CHECK(RealTimeMenus::VatsTimeMultiplier(true, false, true, 0.05f, 0.0f) < 0.0f);   // the host keeps the slow motion
	CHECK(RealTimeMenus::VatsTimeMultiplier(false, true, true, 0.05f, 0.0f) < 0.0f);   // alone
}

TEST("session rules: the tutorial hints are off in a session, and the player's setting is back after it")
{
	Background::Switch tutorials{ false };
	bool               show = true;
	int                changes = 0;
	const auto         frame = [&](bool a_enabled, bool a_session) {
		if (const auto value = tutorials.Update(a_enabled, a_session, show)) {
			show = *value;
			++changes;
		}
	};
	frame(true, false);
	CHECK(show);
	frame(true, true);
	CHECK(!show);
	frame(true, true);
	CHECK(changes == 1);
	frame(true, false);
	CHECK(show);
	CHECK(changes == 2);

	// A player who had them off keeps them off, in and after a session; bHideTutorials = false leaves them alone.
	show = false;
	changes = 0;
	frame(true, true);
	frame(true, false);
	CHECK(!show);
	CHECK(changes == 0);
	show = true;
	frame(false, true);
	CHECK(show);
	CHECK(changes == 0);
}
