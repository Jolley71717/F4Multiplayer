// The session's rules (Protocol::SessionRules) as the host's ini sets them (src/game/HostRules.h), VATS in
// real time by them (src/game/RealTimeVats.h), and the tutorial hints held off in a session (src/Background.h).

#include "Test.h"

#include "Background.h"
#include "game/HostRules.h"
#include "game/RealTimeVats.h"

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

TEST("session rules: VATS runs at full speed in a session with the rule on, but Jet's slow motion stays")
{
	CHECK(RealTimeVats::TimeMultiplier(true, true, true, 0.04f, 0.0f) == 1.0f);   // aiming (the game's 0.04)
	CHECK(RealTimeVats::TimeMultiplier(true, true, true, 0.001f, 0.0f) == 1.0f);  // a shot playing out
	CHECK(RealTimeVats::TimeMultiplier(true, true, true, 1.0f, 0.0f) < 0.0f);     // already: nothing to do
	CHECK(RealTimeVats::TimeMultiplier(true, true, true, 0.04f, 0.5f) < 0.0f);    // on Jet
	CHECK(RealTimeVats::TimeMultiplier(true, true, false, 0.5f, 0.0f) < 0.0f);    // no VATS: the console's sgtm, a bow
	CHECK(RealTimeVats::TimeMultiplier(true, false, true, 0.04f, 0.0f) < 0.0f);   // the host keeps the slow motion
	CHECK(RealTimeVats::TimeMultiplier(false, true, true, 0.04f, 0.0f) < 0.0f);   // alone
}

TEST("session rules: VATS's own slowdown is not taken for Jet; the one from before VATS came up is")
{
	// The game slows VATS's aiming through the same "magic" slowdown as Jet (0.04 while aiming).
	RealTimeVats::Clock clock;
	CHECK(clock.Frame(true, true, false, 1.0f, 0.0f) < 0.0f);     // walking about
	CHECK(clock.Frame(true, true, true, 0.04f, 0.04f) == 1.0f);   // VATS up: its own slowdown, set back
	CHECK(clock.Frame(true, true, true, 0.04f, 0.04f) == 1.0f);   // and every frame it comes back
	CHECK(clock.Frame(true, true, true, 1.0f, 0.04f) < 0.0f);     // held at full speed: nothing to do
	CHECK(clock.Frame(true, true, false, 1.0f, 0.0f) < 0.0f);     // VATS down

	// On Jet before VATS: VATS is left slowed, as the player took it for.
	CHECK(clock.Frame(true, true, false, 0.5f, 0.5f) < 0.0f);
	CHECK(clock.SlowdownBeforeVats() == 0.5f);
	CHECK(clock.Frame(true, true, true, 0.04f, 0.04f) < 0.0f);
	// Jet worn off, then VATS again: real time.
	CHECK(clock.Frame(true, true, false, 1.0f, 0.0f) < 0.0f);
	CHECK(clock.Frame(true, true, true, 0.04f, 0.04f) == 1.0f);
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
