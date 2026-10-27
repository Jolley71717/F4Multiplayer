// The session's rules (Protocol::SessionRules) as the host's ini sets them (src/game/HostRules.h), VATS in
// real time by them (src/game/RealTimeVats.h), and the tutorial hints held off in a session (src/Background.h).

#include "Test.h"

#include "Background.h"
#include "RulesSummary.h"
#include "Text.h"
#include "game/DownRules.h"
#include "game/HostRules.h"
#include "game/RealTimeVats.h"

#include <algorithm>
#include <vector>

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
	CHECK(!rules.downedCrawl);
	CHECK(rules.realTimeVats);
	CHECK(!rules.shareGuestBuilds);  // a guest's earlier building stays in their own game
	CHECK(!rules.sharedStorage);     // each game keeps its own copy of storage, as in 0.9.2
	CHECK(!rules.containerGifts);    // a weapon put in a chest stays its player's own, as in 0.9.2
}

TEST("session rules: crawling while down is off by default, and the host's bDownedCrawl turns it on for the downed rules")
{
	Config::Settings settings;
	CHECK(!HostRules::FromSettings(settings).downedCrawl);
	CHECK(!DownRules::Rules::From(HostRules::FromSettings(settings)).crawl);
	settings.downedCrawl = true;
	const auto rules = HostRules::FromSettings(settings);
	CHECK(rules.downedCrawl);
	CHECK(DownRules::Rules::From(rules).crawl);
	CHECK(HostRules::Describe(rules).find("crawl=true") != std::string::npos);
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
	settings.shareGuestBuilds = true;
	settings.sharedStorage = true;
	settings.containerGifts = true;
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
	CHECK(rules.shareGuestBuilds);
	CHECK(rules.sharedStorage);
	CHECK(rules.containerGifts);
	CHECK(HostRules::Describe(rules).find("sharedStorage=true") != std::string::npos);
	CHECK(HostRules::Describe(rules).find("containerGifts=true") != std::string::npos);

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

TEST("session rules: default rules have no summary")
{
	Text::Use("en", {});
	CHECK(RulesSummary::Of(Protocol::SessionRules{}).empty());
	CHECK(HostRules::Summary(Protocol::SessionRules{}).empty());
	// VATS is not a rule for being down; a window with no limit limits nothing.
	Protocol::SessionRules quiet;
	quiet.realTimeVats = false;
	quiet.reviveWindowMinutes = 10;
	CHECK(RulesSummary::Of(quiet).empty());
}

TEST("session rules: a revive limit per fight is summarised as revives per fight")
{
	Text::Use("en", {});
	Protocol::SessionRules rules;
	rules.maxRevives = 3;
	const std::vector<RulesSummary::Part> fight{ { "rules.revives_fight", { { "n", "3" } } } };
	CHECK(RulesSummary::Of(rules) == fight);
	CHECK(HostRules::Summary(rules) == "Host's rules: revive limit 3 per fight");
	rules.reviveWindowMinutes = 10;
	const std::vector<RulesSummary::Part> window{ { "rules.revives_window", { { "n", "3" }, { "minutes", "10" } } } };
	CHECK(RulesSummary::Of(rules) == window);
}

TEST("session rules: revive off is summarised alone")
{
	Text::Use("en", {});
	Protocol::SessionRules rules;
	rules.revive = false;
	rules.maxRevives = 3;
	rules.bleedOutSeconds = 20;
	rules.reviveOutsideCombat = true;
	const std::vector<RulesSummary::Part> off{ { "rules.revive_off", {} } };
	CHECK(RulesSummary::Of(rules) == off);
	CHECK(HostRules::Summary(rules) == "Host's rules: no revives, a lethal hit kills");
}

TEST("session rules: every rule that differs is in the summary, in order, within the limits, each in the English table")
{
	Text::Use("en", {});
	Protocol::SessionRules rules;
	rules.maxRevives = 1;
	rules.reviveWindowMinutes = 10;
	rules.bleedOutSeconds = 20;
	rules.helpTenths = 25;
	rules.reviveHealth = 10;
	rules.downCooldownSeconds = 5;
	rules.reviveOutsideCombat = true;
	CHECK(HostRules::Summary(rules) == "Host's rules: revive limit 1 per 10 min, 20 s to bleed out, 2.5 s to help a friend up, up with 10% health, "
	                                   "a lethal hit within 5 s of a revive kills, no helping up during a fight");
	for (const auto& part : RulesSummary::Of(rules)) {
		CHECK(std::ranges::any_of(Text::English(), [&](const auto& a_entry) { return a_entry.key == part.key; }));
	}
	// Whole seconds show no tenths; what the wire could carry beyond the limits is told as it applies.
	Protocol::SessionRules wild;
	wild.helpTenths = 50;
	wild.bleedOutSeconds = 1;
	CHECK(HostRules::Summary(wild) == "Host's rules: 5 s to bleed out, 5 s to help a friend up");
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
