// The friend bot (src/game/FriendBotPlan.h): the dev command's words, the load door the host took, the enemy fought, the
// right stick's aim, and the states the bot goes through as the host walks, goes through a door, fights and waits.

#include "Test.h"

#include "game/FriendBotPlan.h"

using namespace std::chrono_literals;
using FriendBotPlan::Action;
using FriendBotPlan::State;

namespace
{
	const auto START = FriendBotPlan::Clock::time_point{} + 1h;

	constexpr FriendBotPlan::Place HALL{ 0x0001F00D, 0 };
	constexpr FriendBotPlan::Place OUTSIDE{ 0, 0x0000003C };
	constexpr FriendBotPlan::Place CELLAR{ 0x0002BEEF, 0 };

	FriendBotPlan::Command ParseOk(std::string_view a_line)
	{
		auto parsed = FriendBotPlan::Parse(a_line);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			Test::Fail(__FILE__, __LINE__, std::format("'{}' did not parse: {}", a_line, *error));
			throw Test::Abort{};
		}
		return std::get<FriendBotPlan::Command>(parsed);
	}

	bool Refused(std::string_view a_line)
	{
		const auto parsed = FriendBotPlan::Parse(a_line);
		return std::holds_alternative<std::string>(parsed) && std::get<std::string>(parsed).starts_with("error: ");
	}

	// In the hall with the host, both in the game, the host's stand-in 500 units off.
	FriendBotPlan::Facts Together()
	{
		FriendBotPlan::Facts f;
		f.inGame = true;
		f.hostKnown = true;
		f.ourPlace = HALL;
		f.hostPlace = HALL;
		f.hostHere = true;
		f.hostActor = 0xFF001234;
		f.hostAt = { 500.0f, 0.0f, 0.0f };
		f.us = { 0.0f, 0.0f, 0.0f };
		return f;
	}

	FriendBotPlan::Bot Following()
	{
		FriendBotPlan::Bot bot;
		bot.TurnOn(0);
		return bot;
	}
}

TEST("friend bot: the command's words")
{
	CHECK(ParseOk("").kind == FriendBotPlan::Command::Kind::kStatus);
	CHECK(ParseOk("status").kind == FriendBotPlan::Command::Kind::kStatus);
	const auto on = ParseOk("on");
	CHECK(on.kind == FriendBotPlan::Command::Kind::kOn);
	CHECK(on.player == 0);
	CHECK(ParseOk("on 2").player == 2);
	CHECK(ParseOk("  on   7 ").player == 7);
	CHECK(ParseOk("off").kind == FriendBotPlan::Command::Kind::kOff);
	CHECK(ParseOk("wait").kind == FriendBotPlan::Command::Kind::kWait);
	CHECK(ParseOk("follow").kind == FriendBotPlan::Command::Kind::kFollow);
	CHECK(ParseOk("mode travel").mode == FriendBotPlan::Mode::kTravel);
	CHECK(ParseOk("mode follow").mode == FriendBotPlan::Mode::kFollow);
	CHECK(Refused("on 0"));
	CHECK(Refused("on x"));
	CHECK(Refused("on 2 3"));
	CHECK(Refused("off now"));
	CHECK(Refused("mode"));
	CHECK(Refused("mode fly"));
	CHECK(Refused("teleport"));
}

TEST("friend bot: the door the host took leads where they are now, nearest to where they were last seen")
{
	const std::vector<FriendBotPlan::Door> doors{
		{ 0x10, { 0.0f, 0.0f, 0.0f }, CELLAR },
		{ 0x11, { 2000.0f, 0.0f, 0.0f }, OUTSIDE },
		{ 0x12, { 100.0f, 0.0f, 0.0f }, OUTSIDE },
	};
	const auto pick = FriendBotPlan::PickDoor(doors, OUTSIDE, { 50.0f, 10.0f, 0.0f });
	REQUIRE(pick.has_value());
	CHECK(doors[*pick].ref == 0x12);
	// The cellar door is nearer, but the host is outside.
	CHECK(doors[*FriendBotPlan::PickDoor(doors, OUTSIDE, { 0.0f, 0.0f, 0.0f })].ref == 0x12);
}

TEST("friend bot: with no door to where the host is, the nearest load door near their last spot, or none")
{
	const std::vector<FriendBotPlan::Door> doors{
		{ 0x10, { 300.0f, 0.0f, 0.0f }, CELLAR },
		{ 0x11, { 900.0f, 0.0f, 0.0f }, OUTSIDE },
	};
	const FriendBotPlan::Place elsewhere{ 0x00ABCDEF, 0 };
	const auto                 pick = FriendBotPlan::PickDoor(doors, elsewhere, { 0.0f, 0.0f, 0.0f });
	REQUIRE(pick.has_value());
	CHECK(doors[*pick].ref == 0x10);
	CHECK(!FriendBotPlan::PickDoor(doors, elsewhere, { 5000.0f, 0.0f, 0.0f }).has_value());
	CHECK(!FriendBotPlan::PickDoor({}, OUTSIDE, {}).has_value());
}

TEST("friend bot: it fights the nearest live hostile in combat it can see, near one of the two")
{
	using C = FriendBotPlan::Candidate;
	const std::vector<C> candidates{
		{ 0x01, 200.0f, 200.0f, true, true, false, true },     // hostile, asleep: not fighting
		{ 0x02, 300.0f, 300.0f, true, false, true, true },     // in combat, not hostile to us
		{ 0x03, 150.0f, 150.0f, false, true, true, true },     // dead
		{ 0x04, 100.0f, 100.0f, true, true, true, false },     // behind a wall
		{ 0x05, 900.0f, 900.0f, true, true, true, true },
		{ 0x06, 3500.0f, 2500.0f, true, true, true, true },    // far from us, near the host
		{ 0x07, 5000.0f, 5000.0f, true, true, true, true },    // far from both
	};
	const auto pick = FriendBotPlan::PickTarget(candidates);
	REQUIRE(pick.has_value());
	CHECK(candidates[*pick].ref == 0x05);
	const std::vector<C> onlyFar{ candidates[5], candidates[6] };
	CHECK(onlyFar[*FriendBotPlan::PickTarget(onlyFar)].ref == 0x06);
	CHECK(!FriendBotPlan::PickTarget({ candidates[0], candidates[1], candidates[2], candidates[3], candidates[6] }).has_value());
}

TEST("friend bot: the right stick turns toward the target and fires once on it")
{
	const auto right = FriendBotPlan::AimAt(45.0f, 0.0f);
	CHECK(right.rightX > 0.79f);
	CHECK(right.rightY == 0.0f);
	CHECK(!right.fire);
	const auto left = FriendBotPlan::AimAt(-15.0f, 0.0f);
	CHECK(left.rightX < -0.39f);
	CHECK(left.rightX > -0.41f);
	// Below the aim: the game's pitch grows looking down, so the stick goes down (negative).
	CHECK(FriendBotPlan::AimAt(0.0f, 10.0f).rightY < 0.0f);
	CHECK(FriendBotPlan::AimAt(0.0f, -10.0f).rightY > 0.0f);
	const auto on = FriendBotPlan::AimAt(2.0f, -3.0f);
	CHECK(on.fire);
	CHECK(on.rightX > 0.0f);
	const auto still = FriendBotPlan::AimAt(0.2f, 0.1f);
	CHECK(still.rightX == 0.0f);
	CHECK(still.rightY == 0.0f);
	CHECK(still.fire);
	CHECK(!FriendBotPlan::AimAt(std::nanf(""), 0.0f).fire);
	CHECK(FriendBotPlan::AimAt(std::nanf(""), 0.0f).rightX == 0.0f);
}

TEST("friend bot: the pitch to a target above is up (negative), below is down")
{
	CHECK(FriendBotPlan::PitchTo(100.0f, 100.0f) < -44.9f);
	CHECK(FriendBotPlan::PitchTo(-100.0f, 100.0f) > 44.9f);
	CHECK(FriendBotPlan::PitchTo(0.0f, 100.0f) == 0.0f);
	CHECK(std::isfinite(FriendBotPlan::PitchTo(50.0f, 0.0f)));
}

TEST("friend bot: off until turned on, and on it follows the host's stand-in once")
{
	FriendBotPlan::Bot bot;
	CHECK(!bot.On());
	CHECK(bot.Wait().starts_with("off"));
	CHECK(bot.Follow().starts_with("off"));
	CHECK(!bot.On());
	bot.TurnOn(0);
	bot.SetMode(FriendBotPlan::Mode::kFollow);
	const auto first = bot.Step(START, Together());
	CHECK(first.action == Action::kFollowHost);
	CHECK(first.log.starts_with("follow FF001234 on a follow package"));
	CHECK(bot.Now() == State::kFollow);
	// The same package goes on; no line.
	const auto next = bot.Step(START + 1s, Together());
	CHECK(next.action == Action::kNone);
	CHECK(next.log.empty());
	// Made again after a while (a package can end on its own), with no line.
	const auto renewed = bot.Step(START + FriendBotPlan::FOLLOW_RENEW + 1s, Together());
	CHECK(renewed.action == Action::kFollowHost);
	CHECK(renewed.log.empty());
}

TEST("friend bot: a new stand-in (the mod respawned it) gets a new package at once")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto f = Together();
	f.hostActor = 0xFF005678;
	CHECK(bot.Step(START + 1s, f).action == Action::kFollowHost);
}

TEST("friend bot: travel is the default mode, and follow says it is unsafe")
{
	FriendBotPlan::Bot bot;
	CHECK(bot.Package() == FriendBotPlan::Mode::kTravel);
	bot.TurnOn(0);
	CHECK(bot.Step(START, Together()).log.starts_with("follow FF001234 on a travel package"));
	CHECK(bot.SetMode(FriendBotPlan::Mode::kFollow).find("unsafe") != std::string::npos);
	CHECK(bot.SetMode(FriendBotPlan::Mode::kTravel) == "mode travel");
}

TEST("friend bot: travel mode makes a package again as the host moves")
{
	auto bot = Following();
	bot.SetMode(FriendBotPlan::Mode::kTravel);
	auto f = Together();
	const auto first = bot.Step(START, f);
	CHECK(first.action == Action::kFollowHost);
	CHECK(first.log.find("travel package") != std::string::npos);
	f.hostAt.x += 100.0f;
	CHECK(bot.Step(START + 500ms, f).action == Action::kNone);
	f.hostAt.x += 200.0f;
	CHECK(bot.Step(START + 1s, f).action == Action::kFollowHost);
	CHECK(bot.Step(START + 1500ms, f).action == Action::kNone);
	CHECK(bot.Step(START + 1500ms + FriendBotPlan::TRAVEL_RENEW, f).action == Action::kFollowHost);
}

TEST("friend bot: a menu, a conversation, a loading screen or being down lets go, and it resumes by itself")
{
	auto bot = Following();
	bot.Step(START, Together());
	for (const auto& [set, why] : std::vector<std::pair<void (*)(FriendBotPlan::Facts&), std::string_view>>{
			 { [](FriendBotPlan::Facts& f) { f.menu = true; }, "a menu is open" },
			 { [](FriendBotPlan::Facts& f) { f.dialogue = true; }, "a conversation" },
			 { [](FriendBotPlan::Facts& f) { f.loading = true; }, "a loading screen" },
			 { [](FriendBotPlan::Facts& f) { f.down = true; }, "our player is down" },
			 { [](FriendBotPlan::Facts& f) { f.inGame = false; }, "not in a game" },
		 }) {
		auto f = Together();
		set(f);
		const auto paused = bot.Step(START + 2s, f);
		CHECK(paused.action == Action::kRelease);
		CHECK(paused.log == std::format("paused: {}", why));
		CHECK(bot.Now() == State::kPaused);
		CHECK(bot.Pause() == why);
		// Still paused: nothing more, no line.
		const auto again = bot.Step(START + 3s, f);
		CHECK(again.action == Action::kNone);
		CHECK(again.log.empty());
		const auto back = bot.Step(START + 4s, Together());
		CHECK(back.action == Action::kFollowHost);
		CHECK(bot.Now() == State::kFollow);
	}
}

TEST("friend bot: waits when told, and follows again when told")
{
	auto bot = Following();
	bot.Step(START, Together());
	bot.Wait();
	const auto wait = bot.Step(START + 1s, Together());
	CHECK(wait.action == Action::kRelease);
	CHECK(wait.log == "waiting");
	CHECK(bot.Now() == State::kWait);
	auto far = Together();
	far.hostAt = { 5000.0f, 0.0f, 0.0f };
	CHECK(bot.Step(START + 2s, far).action == Action::kNone);
	bot.Follow();
	CHECK(bot.Step(START + 3s, far).action == Action::kFollowHost);
}

TEST("friend bot: off lets go once")
{
	auto bot = Following();
	bot.Step(START, Together());
	bot.TurnOff();
	CHECK(bot.Step(START + 1s, Together()).action == Action::kRelease);
	CHECK(bot.Step(START + 2s, Together()).action == Action::kNone);
	CHECK(bot.Now() == State::kOff);
}

TEST("friend bot: a fight comes first, goes on briefly with no target, then it follows again")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto f = Together();
	f.target = 0x000ABCDE;
	const auto fight = bot.Step(START + 1s, f);
	CHECK(fight.action == Action::kFight);
	CHECK(fight.log == "fight 000ABCDE");
	CHECK(bot.Now() == State::kFight);
	CHECK(bot.Step(START + 1100ms, f).log.empty());
	f.target = 0x000ABCDF;
	CHECK(bot.Step(START + 1200ms, f).log == "fight 000ABCDF");
	// Lost from sight: the fight goes on for FIGHT_GRACE, then the bot follows.
	const auto lost = bot.Step(START + 2s, Together());
	CHECK(lost.action == Action::kFight);
	const auto after = bot.Step(START + 1200ms + FriendBotPlan::FIGHT_GRACE + 100ms, Together());
	CHECK(after.action == Action::kFollowHost);
	CHECK(bot.Now() == State::kFollow);
}

TEST("friend bot: the host went through a door: it asks for the door, walks to it, uses it, and follows on the far side")
{
	auto bot = Following();
	bot.Step(START, Together());
	// The host's stand-in was last at (500, 0) in the hall; now their state says outside.
	auto gone = Together();
	gone.hostPlace = OUTSIDE;
	gone.hostHere = false;
	const auto ask = bot.Step(START + 1s, gone);
	CHECK(ask.action == Action::kPickDoor);
	CHECK(ask.log == "paused: looking for the load door the host took");
	CHECK(bot.LastSeen().x == 500.0f);
	// Not asked again at once.
	CHECK(bot.Step(START + 2s, gone).action == Action::kNone);
	// The glue found it.
	gone.door = FriendBotPlan::Door{ 0x00DD0001, { 600.0f, 0.0f, 0.0f }, OUTSIDE };
	const auto go = bot.Step(START + 3s, gone);
	CHECK(go.action == Action::kTravelToDoor);
	CHECK(go.log.starts_with("door 00DD0001 to where the host is"));
	CHECK(bot.Now() == State::kDoor);
	gone.door.reset();
	CHECK(bot.Step(START + 4s, gone).action == Action::kNone);
	// At the door: let go and activate it.
	gone.us = { 520.0f, 0.0f, 0.0f };
	const auto use = bot.Step(START + 5s, gone);
	CHECK(use.action == Action::kUseDoor);
	CHECK(use.log == "activate door 00DD0001 (try 1 of 3)");
	// The loading screen: paused, the door kept.
	auto loading = gone;
	loading.loading = true;
	CHECK(bot.Step(START + 6s, loading).action == Action::kRelease);
	REQUIRE(bot.DoorTaken().has_value());
	// Outside with the host: following again, the door forgotten.
	auto outside = Together();
	outside.ourPlace = OUTSIDE;
	outside.hostPlace = OUTSIDE;
	outside.hostActor = 0xFF009999;
	CHECK(bot.Step(START + 12s, outside).action == Action::kFollowHost);
	CHECK(!bot.DoorTaken().has_value());
}

TEST("friend bot: a door that does not take it through is tried three times, then it stops and says so")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto gone = Together();
	gone.hostPlace = OUTSIDE;
	gone.hostHere = false;
	gone.us = { 590.0f, 0.0f, 0.0f };
	gone.door = FriendBotPlan::Door{ 0x00DD0002, { 600.0f, 0.0f, 0.0f }, OUTSIDE };
	CHECK(bot.Step(START + 1s, gone).action == Action::kTravelToDoor);
	gone.door.reset();
	auto t = START + 2s;
	for (int i = 1; i <= FriendBotPlan::DOOR_TRIES; ++i) {
		CHECK(bot.Step(t, gone).action == Action::kUseDoor);
		CHECK(bot.Step(t + 1s, gone).action == Action::kNone);
		t += FriendBotPlan::DOOR_RETRY;
	}
	const auto stop = bot.Step(t, gone);
	CHECK(stop.action == Action::kRelease);
	CHECK(stop.log == "paused: door 00DD0002 did not take us through after 3 tries");
	CHECK(bot.Now() == State::kPaused);
}

TEST("friend bot: the host somewhere it never saw them: it waits for them instead of looking for a door")
{
	auto bot = Following();
	auto f = Together();
	f.hostPlace = OUTSIDE;
	f.hostHere = false;
	const auto d = bot.Step(START, f);
	CHECK(d.action == Action::kRelease);
	CHECK(d.log == "paused: the host is elsewhere and was never seen here");
	CHECK(bot.Step(START + 1s, f).action == Action::kNone);
}

TEST("friend bot: no door found: asked again every few seconds, with one line")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto gone = Together();
	gone.hostPlace = OUTSIDE;
	gone.hostHere = false;
	CHECK(bot.Step(START + 1s, gone).action == Action::kPickDoor);
	CHECK(bot.Step(START + 2s, gone).action == Action::kNone);
	const auto again = bot.Step(START + 1s + FriendBotPlan::DOOR_RETRY, gone);
	CHECK(again.action == Action::kPickDoor);
	CHECK(again.log.empty());
}

TEST("friend bot: no stand-in yet in the same place: paused until it comes")
{
	auto bot = Following();
	auto f = Together();
	f.hostHere = false;
	const auto d = bot.Step(START, f);
	CHECK(d.action == Action::kRelease);
	CHECK(d.log == "paused: the host's stand-in is not here");
	CHECK(bot.Step(START + 1s, Together()).action == Action::kFollowHost);
	auto none = Together();
	none.hostKnown = false;
	none.hostHere = false;
	CHECK(bot.Step(START + 2s, none).log == "paused: no host in the session");
}

TEST("friend bot: after a save let go of the package, the next frame makes another")
{
	auto bot = Following();
	bot.Step(START, Together());
	CHECK(bot.Step(START + 1s, Together()).action == Action::kNone);
	bot.Renew();
	CHECK(bot.Step(START + 2s, Together()).action == Action::kFollowHost);
}

TEST("friend bot: a reference our package names lets go of the player before it goes")
{
	constexpr std::uint32_t STAND_IN = 0xFF000F81;
	constexpr std::uint32_t DOOR = 0x000022CB;
	CHECK(FriendBotPlan::LetGoBefore(true, STAND_IN, STAND_IN, STAND_IN));    // a follow package's target and place
	CHECK(FriendBotPlan::LetGoBefore(true, 0, DOOR, DOOR));                   // a travel package's door
	CHECK(!FriendBotPlan::LetGoBefore(true, STAND_IN, STAND_IN, 0xFF000F99)); // someone else's stand-in
	CHECK(!FriendBotPlan::LetGoBefore(false, STAND_IN, STAND_IN, STAND_IN));  // not driving: nothing to let go of
	CHECK(!FriendBotPlan::LetGoBefore(true, 0, 0, 0));
}

TEST("friend bot: a travel package that ends short of the door uses it from there, once it has stopped near it")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto gone = Together();
	gone.hostPlace = OUTSIDE;
	gone.hostHere = false;
	gone.us = { 400.0f, 0.0f, 0.0f };
	gone.door = FriendBotPlan::Door{ 0x00DD0003, { 600.0f, 0.0f, 0.0f }, OUTSIDE };
	CHECK(bot.Step(START + 1s, gone).action == Action::kTravelToDoor);
	gone.door.reset();
	// Still on its way (it moved): no use yet.
	CHECK(bot.Step(START + 2s, gone).action == Action::kNone);
	gone.us.x = 432.0f;  // 168 units short of the door, as at Valentine's office
	CHECK(bot.Step(START + 4s, gone).action == Action::kNone);
	gone.us.x = 436.0f;  // less than DOOR_STALL_MOVE since the last move
	CHECK(bot.Step(START + 6s, gone).action == Action::kNone);
	const auto use = bot.Step(START + 4s + FriendBotPlan::DOOR_STALL, gone);
	CHECK(use.action == Action::kUseDoor);
	CHECK(use.log == "activate door 00DD0003 (try 1 of 3)");
}

TEST("friend bot: stopped far from the door, it keeps travelling instead")
{
	auto bot = Following();
	bot.Step(START, Together());
	auto gone = Together();
	gone.hostPlace = OUTSIDE;
	gone.hostHere = false;
	gone.us = { -400.0f, 0.0f, 0.0f };
	gone.door = FriendBotPlan::Door{ 0x00DD0004, { 600.0f, 0.0f, 0.0f }, OUTSIDE };
	CHECK(bot.Step(START + 1s, gone).action == Action::kTravelToDoor);
	gone.door.reset();
	for (auto t = START + 2s; t < START + 1s + FriendBotPlan::FOLLOW_RENEW; t += 1s) {
		CHECK(bot.Step(t, gone).action == Action::kNone);
	}
	CHECK(bot.Step(START + 1s + FriendBotPlan::FOLLOW_RENEW, gone).action == Action::kTravelToDoor);
}
