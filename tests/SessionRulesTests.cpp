// The session's rules (Protocol::SessionRules) as the host's ini sets them (src/game/HostRules.h), VATS in
// real time by them (src/game/RealTimeVats.h), and the tutorial hints held off in a session (src/Background.h).

#include "Test.h"

#include "Background.h"
#include "game/HostRules.h"
#include "game/RealTimeVats.h"

#include <limits>
#include <string>
#include <string_view>

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
	CHECK(rules.vatsMode == Protocol::SessionRules::VATS_REAL_TIME);
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
	settings.vatsMode = Protocol::SessionRules::VATS_SLOWED_FRIENDS;
	const auto rules = HostRules::FromSettings(settings);
	CHECK(!rules.revive);
	CHECK(rules.bleedOutSeconds == 120);
	CHECK(rules.helpTenths == 35);
	CHECK(rules.reviveHealth == 50);
	CHECK(rules.maxRevives == 3);
	CHECK(rules.reviveWindowMinutes == 20);
	CHECK(rules.downCooldownSeconds == 90);
	CHECK(rules.reviveOutsideCombat);
	CHECK(rules.vatsMode == Protocol::SessionRules::VATS_SLOWED_FRIENDS);

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
	CHECK(RealTimeVats::TimeMultiplier(true, 0, true, 0.04f, 0.0f) == 1.0f);   // aiming (the game's 0.04)
	CHECK(RealTimeVats::TimeMultiplier(true, 0, true, 0.001f, 0.0f) == 1.0f);  // a shot playing out
	CHECK(RealTimeVats::TimeMultiplier(true, 0, true, 1.0f, 0.0f) < 0.0f);     // already: nothing to do
	CHECK(RealTimeVats::TimeMultiplier(true, 0, true, 0.04f, 0.5f) < 0.0f);    // on Jet
	CHECK(RealTimeVats::TimeMultiplier(true, 0, false, 0.5f, 0.0f) < 0.0f);    // no VATS: the console's sgtm, a bow
	CHECK(RealTimeVats::TimeMultiplier(true, 1, true, 0.04f, 0.0f) < 0.0f);   // the host keeps the slow motion
	CHECK(RealTimeVats::TimeMultiplier(false, 0, true, 0.04f, 0.0f) < 0.0f);   // alone
}

TEST("session rules: VATS's own slowdown is not taken for Jet; the one from before VATS came up is")
{
	// The game slows VATS's aiming through the same "magic" slowdown as Jet (0.04 while aiming).
	RealTimeVats::Clock clock;
	CHECK(clock.Frame(true, 0, false, 1.0f, 0.0f) < 0.0f);     // walking about
	CHECK(clock.Frame(true, 0, true, 0.04f, 0.04f) == 1.0f);   // VATS up: its own slowdown, set back
	CHECK(clock.Frame(true, 0, true, 0.04f, 0.04f) == 1.0f);   // and every frame it comes back
	CHECK(clock.Frame(true, 0, true, 1.0f, 0.04f) < 0.0f);     // held at full speed: nothing to do
	CHECK(clock.Frame(true, 0, false, 1.0f, 0.0f) < 0.0f);     // VATS down

	// On Jet before VATS: VATS is left slowed, as the player took it for.
	CHECK(clock.Frame(true, 0, false, 0.5f, 0.5f) < 0.0f);
	CHECK(clock.SlowdownBeforeVats() == 0.5f);
	CHECK(clock.Frame(true, 0, true, 0.04f, 0.04f) < 0.0f);
	// Jet worn off, then VATS again: real time.
	CHECK(clock.Frame(true, 0, false, 1.0f, 0.0f) < 0.0f);
	CHECK(clock.Frame(true, 0, true, 0.04f, 0.04f) == 1.0f);
}

TEST("session rules: of the three VATS modes only real time sets the game's slow motion back, and an unknown mode is real time")
{
	using Rules = Protocol::SessionRules;
	CHECK(RealTimeVats::RunsInRealTime(Rules::VATS_REAL_TIME));
	CHECK(!RealTimeVats::RunsInRealTime(Rules::VATS_SLOW_MOTION));
	CHECK(!RealTimeVats::RunsInRealTime(Rules::VATS_SLOWED_FRIENDS));
	CHECK(RealTimeVats::RunsInRealTime(77));  // a mode this build does not know (Clamped makes it the default)
	CHECK(!RealTimeVats::SlowsFriends(Rules::VATS_REAL_TIME));
	CHECK(!RealTimeVats::SlowsFriends(Rules::VATS_SLOW_MOTION));
	CHECK(RealTimeVats::SlowsFriends(Rules::VATS_SLOWED_FRIENDS));

	// The slow motion is the game's own in both of the other modes: nothing is set.
	CHECK(RealTimeVats::TimeMultiplier(true, Rules::VATS_REAL_TIME, true, 0.04f, 0.0f) == 1.0f);
	CHECK(RealTimeVats::TimeMultiplier(true, Rules::VATS_SLOW_MOTION, true, 0.04f, 0.0f) < 0.0f);
	CHECK(RealTimeVats::TimeMultiplier(true, Rules::VATS_SLOWED_FRIENDS, true, 0.04f, 0.0f) < 0.0f);
	CHECK(RealTimeVats::TimeMultiplier(true, Rules::VATS_SLOWED_FRIENDS, true, 0.001f, 0.0f) < 0.0f);

	// Their names, for the log.
	CHECK(std::string_view{ RealTimeVats::ModeName(Rules::VATS_REAL_TIME) } == "real time");
	CHECK(std::string_view{ RealTimeVats::ModeName(Rules::VATS_SLOW_MOTION) } == "slow motion");
	CHECK(std::string_view{ RealTimeVats::ModeName(Rules::VATS_SLOWED_FRIENDS) } == "slowed friends");
}

TEST("session rules: bRealTimeVats is the older form of the first two modes, and iVatsMode is kept within the three")
{
	using Rules = Protocol::SessionRules;
	CHECK(RealTimeVats::ModeOfFlag(true) == Rules::VATS_REAL_TIME);
	CHECK(RealTimeVats::ModeOfFlag(false) == Rules::VATS_SLOW_MOTION);

	Config::Settings settings;
	for (std::uint32_t mode = 0; mode <= Rules::MAX_VATS_MODE; ++mode) {
		settings.vatsMode = mode;
		CHECK(HostRules::FromSettings(settings).vatsMode == mode);
	}
	settings.vatsMode = 9;
	CHECK(HostRules::FromSettings(settings).vatsMode == Rules::MAX_VATS_MODE);
	CHECK(HostRules::Describe(HostRules::FromSettings(settings)).find("vatsMode=slowed friends") != std::string::npos);
	CHECK(HostRules::Describe(Rules{}).find("vatsMode=real time") != std::string::npos);
}

TEST("session rules: friends' stand-ins play at the game's slowdown in VATS in the mode with slowed friends, and only then")
{
	using Rules = Protocol::SessionRules;
	constexpr auto slowed = Rules::VATS_SLOWED_FRIENDS;
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, 0.04f, 0.0f) == 0.04f);    // aiming
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, 0.001f, 0.0f) == 0.001f);  // a shot playing out
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, 1.0f, 0.0f) == 1.0f);      // VATS just up, not slowed yet
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, 0.04f, 0.5f) == 1.0f);     // on Jet: left as the game has it
	CHECK(RealTimeVats::FriendsRate(true, slowed, false, 0.5f, 0.0f) == 1.0f);     // no VATS: the console's sgtm, a bow
	CHECK(RealTimeVats::FriendsRate(false, slowed, true, 0.04f, 0.0f) == 1.0f);    // alone: nobody to slow
	CHECK(RealTimeVats::FriendsRate(true, Rules::VATS_REAL_TIME, true, 0.04f, 0.0f) == 1.0f);
	CHECK(RealTimeVats::FriendsRate(true, Rules::VATS_SLOW_MOTION, true, 0.04f, 0.0f) == 1.0f);  // friends keep moving: the very complaint
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, -0.5f, 0.0f) == 0.0f);     // never backwards
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, 2.0f, 0.0f) == 1.0f);      // never faster (a faster world is not a slowdown)
	CHECK(RealTimeVats::FriendsRate(true, slowed, true, std::numeric_limits<float>::quiet_NaN(), 0.0f) == 1.0f);

	// The clock remembers Jet from before VATS, as for real time.
	RealTimeVats::Clock clock;
	clock.Frame(true, slowed, false, 1.0f, 0.0f);
	CHECK(clock.Frame(true, slowed, true, 0.04f, 0.04f) < 0.0f);  // slow motion left to the game
	CHECK(clock.FriendsRate(true, slowed, true, 0.04f) == 0.04f);
	clock.Frame(true, slowed, false, 0.3f, 0.3f);
	clock.Frame(true, slowed, true, 0.04f, 0.04f);
	CHECK(clock.FriendsRate(true, slowed, true, 0.04f) == 1.0f);
}

TEST("session rules: slowed friends fall behind by what the slowdown takes out of each frame, no further than they are kept, and catch up after")
{
	RealTimeVats::Playback playback;
	CHECK(playback.Lag() == 0.0f);

	// Played at the game's 0.04 for a second, in 60 fps frames: 96% of it is lag.
	for (int i = 0; i < 60; ++i) {
		playback.Advance(1.0f / 60.0f, 0.04f);
	}
	CHECK(playback.Lag() > 0.95f);
	CHECK(playback.Lag() < 0.97f);

	// At the normal rate nothing is added: it closes, faster than real time and never below none.
	const float before = playback.Lag();
	playback.Advance(0.1f, 1.0f);
	CHECK(playback.Lag() < before);
	CHECK(playback.Lag() > before - 0.1f * RealTimeVats::Playback::CATCH_UP);  // not faster than CATCH_UP
	for (int i = 0; i < 600; ++i) {
		playback.Advance(1.0f / 60.0f, 1.0f);
	}
	CHECK(playback.Lag() == 0.0f);
	playback.Advance(1.0f, 1.0f);
	CHECK(playback.Lag() == 0.0f);

	// A long VATS: the most it falls behind is what the states are kept for.
	for (int i = 0; i < 60 * 30; ++i) {
		playback.Advance(1.0f / 60.0f, 0.04f);
	}
	CHECK(playback.Lag() == RealTimeVats::Playback::MAX_LAG);

	// A frozen game (rate 0) takes out the whole frame; the slowdown can never add more than the time that went by.
	playback.Reset();
	playback.Advance(0.2f, 0.0f);
	CHECK(playback.Lag() == 0.2f);
	playback.Advance(0.2f, -3.0f);
	CHECK(playback.Lag() == 0.4f);

	// A stall (a loading screen, a hitch) is no slowdown to follow beyond one frame's worth, and no time is no change.
	playback.Reset();
	playback.Advance(30.0f, 0.0f);
	CHECK(playback.Lag() == RealTimeVats::Playback::MAX_STEP);
	playback.Advance(0.0f, 0.0f);
	playback.Advance(-1.0f, 0.0f);
	playback.Advance(std::numeric_limits<float>::quiet_NaN(), 0.0f);
	CHECK(playback.Lag() == RealTimeVats::Playback::MAX_STEP);

	// A rate that is not a number is no slowdown: the lag closes as at any normal rate.
	playback.Advance(0.1f, std::numeric_limits<float>::quiet_NaN());
	CHECK(playback.Lag() < RealTimeVats::Playback::MAX_STEP);

	// Out of the session, or the mode off: none at once.
	playback.Reset();
	CHECK(playback.Lag() == 0.0f);
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
