// Downed and revive (src/game/DownRules.h, which Downed.cpp runs every frame): the player's own down state
// driven across whole sequences against a model of the game's side of it, and two players helping each other
// up through the real server, their status and Revive messages encoded and decoded as the plugin sends them.

#include "Test.h"
#include "sim/Sim.h"

#include "game/DownRules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <span>

namespace
{
	using DownRules::Clock;
	using Phase = DownRules::Self::Phase;
	using namespace std::chrono_literals;

	constexpr auto FRAME = 100ms;

	// What the game does with our player, as far as being down goes: a lethal hit puts an essential character
	// down and kills anyone else; with the "stay down" flag off the game stands a down character up a few
	// seconds later; a console kill takes a few frames (or, with killIgnored, never takes).
	struct Body
	{
		bool essential = false;  // our flag on the character
		bool stayDown = false;
		bool down = false;
		bool dead = false;
		bool god = false;
		bool killIgnored = false;
		int  killIn = -1;      // frames until a pending kill takes
		int  killFrames = 2;   // how long a kill takes
		int  standUpIn = -1;   // frames until the game stands them up
		int  standUpFrames = 30;
		bool fullHealth = false;  // the game stood them up with full health
		int  trims = 0;
		bool putDownIgnored = false;  // the engine's put-down does not take
		int  putDowns = 0;

		// The game stands them up on a path the stay-down flag does not hold back.
		void StoodUpByGame()
		{
			if (down && !dead) {
				down = false;
			}
		}

		void LethalHit()
		{
			if (god || dead) {
				return;
			}
			if (essential) {
				down = true;
			} else {
				dead = true;
			}
		}
	};

	// One player's game: the plugin's rules and the model of the game they act on.
	struct Game
	{
		DownRules::Self   self;
		DownRules::Helper helper;
		Body              body;
		bool              withFriends = true;
		bool              paused = false;  // the pause menu is open
		bool              fighting = false;  // in combat
		bool              console = false;   // the console is open (or just was)
		int               downNotices = 0;
		int               kills = 0;
		int               lastRevives = 0;
		int               revivesBack = 0;
		int               stoodUp = 0;
		int               backDown = 0;
		int               stayedUp = 0;
		int               upByCommand = 0;
		std::vector<int>  reminders;

		void GameFrame()
		{
			if (body.killIn > 0 && --body.killIn == 0) {
				body.killIn = -1;
				if (!body.essential) {
					body.dead = true;
					body.down = false;
				}
			}
			if (body.down && !body.dead && !body.stayDown && body.standUpIn < 0) {
				body.standUpIn = body.standUpFrames;
			}
			if (body.standUpIn > 0 && --body.standUpIn == 0) {
				body.standUpIn = -1;
				if (body.down && !body.dead && !body.stayDown) {
					body.down = false;
					body.fullHealth = true;
				}
			}
		}

		DownRules::Step Frame(Clock::time_point a_now)
		{
			// The game's own frame first: a pending kill, standing up. A menu stops all of it.
			if (!paused) {
				GameFrame();
			}

			// As Downed.cpp reads it: the life state, and the game's IsDead, which is true while down as well.
			const std::int32_t     lifeState = body.down ? DownRules::LIFE_ESSENTIAL_DOWN : body.dead ? DownRules::LIFE_DEAD : 0;
			const DownRules::Facts facts{ .enabled = withFriends, .essential = body.essential, .down = body.down, .dead = DownRules::Dead(lifeState, body.dead || body.down), .paused = paused, .inCombat = fighting, .console = console };
			const auto             step = self.Frame(facts, a_now);
			if (step.essential) {
				body.essential = *step.essential;
			}
			if (step.stayDown) {
				body.stayDown = *step.stayDown;
				if (*step.stayDown) {
					body.standUpIn = -1;
				}
			}
			if (step.putDown) {
				++body.putDowns;
				if (!body.putDownIgnored && !body.dead) {
					body.down = true;
				}
			}
			stoodUp += step.stoodUp;
			backDown += step.backDown;
			stayedUp += step.stayedUp;
			upByCommand += step.upByCommand;
			if (step.trimHealth) {
				body.fullHealth = false;
				++body.trims;
			}
			downNotices += step.wentDown;
			revivesBack += step.revivesBack;
			if (step.secondsLeft >= 0) {
				reminders.push_back(step.secondsLeft);
			}
			if (step.kill) {
				++kills;
				if (!body.killIgnored) {
					body.killIn = body.killFrames;
				}
			}
			return step;
		}

		// As Downed::ApplyRevive: a friend's help stands us up only once per down.
		bool Revived(Clock::time_point a_now)
		{
			if (!self.Revive(a_now)) {
				return false;
			}
			body.stayDown = false;
			lastRevives += self.AfterRevive(a_now).lastRevive;
			return true;
		}

		// Frames for a while; the time is moved on.
		void Run(Clock::time_point& a_now, Clock::duration a_for)
		{
			for (const auto end = a_now + a_for; a_now < end; a_now += FRAME) {
				Frame(a_now);
			}
		}
	};

	Clock::time_point Start()
	{
		return Clock::time_point{} + 1h;
	}
}

// ---- the player's own down state ---------------------------------------------------------------------

TEST("downed: a lethal hit with friends knocks the player down once, and a friend's help stands them up hurt")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	CHECK(game.body.essential);  // with friends the character is essential
	game.body.LethalHit();
	game.Run(now, 3s);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.self.WaitsForHelp());
	CHECK(game.body.stayDown);  // the game does not stand them up by itself
	CHECK(!game.body.dead);
	CHECK(game.downNotices == 1);  // down while already down says it once
	CHECK(game.self.downs == 1);

	REQUIRE(game.Revived(now));
	CHECK(game.self.Now() == Phase::kGettingUp);
	CHECK(game.self.IsDown());          // still on the ground: the stand-in crouches
	CHECK(!game.self.WaitsForHelp());   // friends are told they are being helped up
	game.Run(now, 5s);
	CHECK(!game.body.down);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.trims == 1);  // the full health the game gave is cut back
	CHECK(game.kills == 0);
	CHECK(game.self.revives == 1);
}

TEST("downed: a second help-up, from the same friend or another, does nothing")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.Revived(now));
	game.Run(now, 1s);
	CHECK(!game.Revived(now));  // the same friend again, 5 s later
	CHECK(!game.Revived(now));  // another friend
	CHECK(game.self.revives == 1);
	game.Run(now, 5s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(!game.Revived(now));  // up already: no help to take
	CHECK(game.body.trims == 1);
}

TEST("downed: bleeding out kills the player instead of putting them down again")
{
	// The kill did not take by the next frame: the old code made the character essential again, the kill
	// put them down instead, and "You're down!" came back every 45 s.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, DownRules::BLEEDOUT_TIME + 1s);
	CHECK(game.kills == 1);
	CHECK(game.body.dead);
	CHECK(!game.body.essential);
	CHECK(game.downNotices == 1);
	CHECK(game.self.bleedOuts == 1);
	CHECK((game.reminders == std::vector<int>{ 30, 10 }));
	game.Run(now, 10s);  // the death screen: nothing more
	CHECK(game.kills == 1);
	CHECK(game.downNotices == 1);
	CHECK(!game.body.essential);

	// The save that loads after the death is a fresh start.
	game.body = {};
	CHECK(!game.self.Loaded(false, now));
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.essential);
}

TEST("downed: a bleed-out kill that does not take is tried again, and never puts the player down again")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.body.killIgnored = true;
	game.body.standUpFrames = 100000;  // the game leaves them down
	game.Run(now, DownRules::BLEEDOUT_TIME + 5s);
	CHECK(game.self.Now() == Phase::kBledOut);
	CHECK(game.downNotices == 1);
	CHECK(!game.body.essential);
	CHECK(game.kills >= 3);  // once, then every KILL_RETRY
	CHECK(game.self.killRetries == static_cast<std::uint32_t>(game.kills - 1));

	game.body.killIgnored = false;
	game.Run(now, 3s);
	CHECK(game.body.dead);
	CHECK(game.downNotices == 1);
}

TEST("downed: a bleed-out the game ends by standing the player up starts over as up")
{
	// With essential off and the stay-down flag off, the game itself stands them up: from then on they can be
	// knocked down again as usual.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.body.killIgnored = true;
	game.body.standUpFrames = 5;
	game.Run(now, DownRules::BLEEDOUT_TIME + 2s);
	CHECK(!game.body.down);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.essential);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.downNotices == 2);
}

TEST("downed: giving up, or the last friend leaving, bleeds out at once")
{
	for (const bool leave : { false, true }) {
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		if (leave) {
			game.withFriends = false;
		} else {
			game.self.GiveUp();
		}
		game.Run(now, 1s);
		CHECK(game.kills == 1);
		CHECK(game.body.dead);
		CHECK(game.downNotices == 1);
	}
}

TEST("downed: giving up while being helped up is ignored")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	game.Revived(now);
	game.self.GiveUp();
	game.Run(now, 5s);
	CHECK(game.kills == 0);
	CHECK(game.self.Now() == Phase::kUp);
}

TEST("downed: helped, but the game never stands the player up: they die after GET_UP_TIME, and stay dead")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	game.Revived(now);
	game.body.standUpFrames = 100000;
	game.Run(now, DownRules::GET_UP_TIME - 1s);
	CHECK(game.kills == 0);
	CHECK(game.body.essential);  // essential until they stand, even after a save cleared it
	game.Run(now, 2s);
	CHECK(game.kills == 1);
	game.Run(now, 5s);
	CHECK(game.body.dead);
	CHECK(game.downNotices == 1);
}

TEST("downed: helped, then the pause menu for a minute: the player is not killed, and stands up after it")
{
	// The game stands nobody up while paused; the get-up clock ran on, and the player was killed in the menu.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	game.paused = true;
	game.Run(now, 60s);
	CHECK(game.kills == 0);
	CHECK(game.self.Now() == Phase::kGettingUp);
	CHECK(game.body.down);
	game.paused = false;
	game.Run(now, 5s);
	CHECK(game.kills == 0);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(!game.body.down);
	CHECK(game.body.trims == 1);
}

TEST("downed: helped and never stood up, with a pause in between: only the time out of the menu counts toward GET_UP_TIME")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	game.body.standUpFrames = 100000;
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	game.paused = true;
	game.Run(now, DownRules::GET_UP_TIME * 3);
	game.paused = false;
	game.Run(now, DownRules::GET_UP_TIME - 5s - 1s);
	CHECK(game.kills == 0);
	game.Run(now, 2s);
	CHECK(game.kills == 1);
	game.Run(now, 5s);
	CHECK(game.body.dead);
}

TEST("downed: down and waiting for help, the pause menu does not stop the bleed-out (friends' time goes on)")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	game.paused = true;
	game.Run(now, DownRules::BLEEDOUT_TIME);
	CHECK(game.kills == 1);
	CHECK(game.self.Now() == Phase::kBledOut);
	game.paused = false;
	game.Run(now, 3s);
	CHECK(game.body.dead);
}

TEST("downed: a save cleared the flags while down; they are set again next frame")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	game.body.essential = false;  // Downed::OnBeforeSave
	game.body.stayDown = false;
	game.Frame(now);
	CHECK(game.body.essential);
	CHECK(game.body.stayDown);
	CHECK(game.self.Now() == Phase::kDown);
}

TEST("downed: the game standing the player up while they wait for help puts them down again, and a friend can still help")
{
	// Seen 2026-10-19: downed in a pile of clutter, the host was stood up by the game, and their revive state was lost.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 5s);
	game.body.StoodUpByGame();
	game.Frame(now);
	now += FRAME;
	CHECK(game.stoodUp == 1);
	CHECK(game.body.putDowns == 1);
	CHECK(game.body.down);  // put down again
	CHECK(game.body.stayDown);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.self.WaitsForHelp());
	game.Frame(now);
	now += FRAME;
	CHECK(game.backDown == 1);
	CHECK(game.downNotices == 1);  // the same down: no second "You're down!"
	CHECK(game.self.downs == 1);
	CHECK(game.self.standUps == 1);

	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.trims == 1);
	CHECK(game.kills == 0);
}

TEST("downed: stood up again and again while down, the bleed-out time runs on from the first down")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	const auto downAt = now;
	for (int frame = 0; now - downAt < DownRules::BLEEDOUT_TIME + 3s; ++frame) {
		if (frame % 10 == 5) {
			game.body.StoodUpByGame();  // every second, a pile of clutter jostling the player
		}
		game.Frame(now);
		now += FRAME;
	}
	CHECK(game.self.standUps >= 40);
	CHECK(game.self.bleedOuts == 1);
	CHECK(game.body.dead);
	CHECK(game.self.revives == 0);
	CHECK(game.downNotices == 1);
	CHECK(game.stayedUp == 0);
}

TEST("downed: a put-down the game never takes is tried every half second, and after 5 s the player is left up")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.putDownIgnored = true;
	game.body.StoodUpByGame();
	game.Run(now, DownRules::PUT_DOWN_GIVE_UP + 1s);
	CHECK(game.stayedUp == 1);
	CHECK(game.body.putDowns == 10);  // 5 s, every 500 ms
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(!game.self.WaitsForHelp());
	CHECK(!game.body.stayDown);
	CHECK(!game.body.dead);
	CHECK(game.kills == 0);
	// Up again: a lethal hit knocks them down as usual.
	game.body.putDownIgnored = false;
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.downNotices == 2);
}

TEST("downed: stood up as the last friend leaves, the player is left up, not killed")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.withFriends = false;
	game.body.StoodUpByGame();
	game.Run(now, 2s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.putDowns == 0);
	CHECK(!game.body.dead);
	CHECK(game.kills == 0);
}

TEST("downed: dying for a moment while down is waited out, with no put-down and no second down")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.down = false;  // a hit the game is still turning into a down
	game.body.dead = true;
	game.Run(now, 1s);
	CHECK(game.body.putDowns == 0);
	CHECK(game.self.Now() == Phase::kDown);
	game.body.dead = false;
	game.body.down = true;
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.downNotices == 1);
	CHECK(game.backDown == 1);
}

TEST("downed: a real death while down is up (dead) after the wait, and the next life starts over")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.down = false;
	game.body.dead = true;
	game.Run(now, DownRules::PUT_DOWN_GIVE_UP + 1s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.putDowns == 0);
	CHECK(!game.self.IsDown());
}

TEST("downed: a save loaded with the player down stands them up as if helped")
{
	Game game;
	auto now = Start();
	game.body.down = true;
	CHECK(game.self.Loaded(true, now));
	game.body.essential = true;  // as Downed::OnGameLoaded does
	game.Run(now, 5s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.kills == 0);
	CHECK(game.downNotices == 0);
}

TEST("downed: with god mode on the player is never down, and friends are never told so")
{
	Game game;
	game.body.god = true;
	auto now = Start();
	for (int i = 0; i < 50; ++i) {
		game.body.LethalHit();
		game.Frame(now);
		now += FRAME;
	}
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(!game.self.IsDown());
	CHECK(!game.self.WaitsForHelp());
	CHECK(game.downNotices == 0);
}

TEST("downed: alone, the character is not essential and a lethal hit kills")
{
	Game game;
	game.withFriends = false;
	auto now = Start();
	game.Run(now, 1s);
	CHECK(!game.body.essential);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.body.dead);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.downNotices == 0);
}

// ---- helping a friend up -------------------------------------------------------------------------------

TEST("downed: helping takes 2 s within range, starts over on stepping away, and waits before helping again")
{
	DownRules::Helper helper;
	auto              now = Start();
	helper.SetFriendDown(7, true);
	const std::vector<DownRules::Helper::Near> close{ { 7, 100.0f } };
	const std::vector<DownRules::Helper::Near> far{ { 7, DownRules::HELP_RANGE + 1.0f } };

	CHECK(helper.Frame(true, far, now).started == 0);
	CHECK(helper.Frame(true, close, now).started == 7);
	now += 1500ms;
	CHECK(helper.Frame(true, close, now).helped == 0);
	CHECK(helper.Frame(true, far, now).started == 0);  // stepped away: starts over
	now += 100ms;
	CHECK(helper.Frame(true, close, now).started == 7);
	now += 1900ms;
	CHECK(helper.Frame(true, close, now).helped == 0);
	now += 100ms;
	CHECK(helper.Frame(true, close, now).helped == 7);
	// Still told they are down (their next status is on its way): not helped again for a while.
	for (int i = 0; i < 40; ++i) {
		now += FRAME;
		CHECK(helper.Frame(true, close, now).helped == 0);
	}
	CHECK(helper.helped == 1);
}

TEST("downed: a helper who is down, dead or alone does not help, and their time next to a friend does not count")
{
	DownRules::Helper helper;
	auto              now = Start();
	helper.SetFriendDown(7, true);
	const std::vector<DownRules::Helper::Near> close{ { 7, 50.0f } };
	CHECK(helper.Frame(true, close, now).started == 7);
	now += 1s;
	for (int i = 0; i < 30; ++i) {
		now += FRAME;
		CHECK(helper.Frame(false, close, now).helped == 0);
	}
	// Able again: the 2 s start over.
	CHECK(helper.Frame(true, close, now).started == 7);
	now += 1s;
	CHECK(helper.Frame(true, close, now).helped == 0);
	now += 1s;
	CHECK(helper.Frame(true, close, now).helped == 7);
}

TEST("downed: a friend who is up, or has left, is not helped")
{
	DownRules::Helper helper;
	auto              now = Start();
	const std::vector<DownRules::Helper::Near> close{ { 7, 50.0f }, { 8, 60.0f } };
	helper.SetFriendDown(7, true);
	helper.SetFriendDown(7, false);
	helper.SetFriendDown(8, true);
	helper.Left(8);
	for (int i = 0; i < 50; ++i) {
		now += FRAME;
		const auto step = helper.Frame(true, close, now);
		CHECK(step.started == 0);
		CHECK(step.helped == 0);
	}
	CHECK(helper.FriendsDown() == 0);
}

// ---- a friend's stand-in --------------------------------------------------------------------------------

TEST("downed: a friend's stand-in that something here put down is replaced, whatever its friend is doing")
{
	using DownRules::StandInBroken;
	CHECK(!StandInBroken(0, false, false));  // alive
	CHECK(StandInBroken(DownRules::LIFE_ESSENTIAL_DOWN, false, false));
	CHECK(StandInBroken(DownRules::LIFE_BLEEDOUT, false, false));
	CHECK(StandInBroken(1, true, false));  // dying
	CHECK(StandInBroken(2, true, false));  // dead
	CHECK(StandInBroken(0, false, true));  // switched off
	CHECK(!StandInBroken(4, false, false));
}

// ---- two games through the server ----------------------------------------------------------------------

namespace
{
	// One player's whole side of it: their game (as above), what they tell the session, and what they read
	// from it, as Party and Downed do.
	struct Player
	{
		Sim::Client& client;
		Game         game;
		float        x = 0.0f;
		std::size_t  revivesRead = 0;
		int          helpedUp = 0;  // "You helped ... up"
		std::optional<Protocol::PlayerStatus> told;  // the last status sent
		std::map<std::uint32_t, Protocol::PlayerStatus> read;  // friends' statuses, as Party::ApplyStatus keeps them
		std::vector<DownRules::FriendNews>               news;  // what we were told of friends: down, died, back up

		// As Party::SampleStatus, from the game's IsDead, which is true while down and getting up as well.
		Protocol::PlayerStatus Status() const
		{
			Protocol::PlayerStatus status;
			status.health = DownRules::StatusHealth(game.body.dead || game.body.down, game.self.IsDown(), 50.0f, 100.0f);
			status.level = 10;
			status.downed = game.self.WaitsForHelp();
			return status;
		}

		[[nodiscard]] int Heard(DownRules::FriendNews a_news) const
		{
			return static_cast<int>(std::ranges::count(news, a_news));
		}
	};

	// A frame in both games, then the session settles and each reads what came: friends' statuses (Party::
	// ApplyStatus), and help-ups (Session's kRevived to Downed::ApplyRevive).
	void Tick(Sim::Session& a_session, std::span<Player*> a_players, Clock::time_point a_now)
	{
		for (const auto player : a_players) {
			player->game.Frame(a_now);
			const auto status = player->Status();
			if (player->told != status) {
				player->told = status;
				player->client.Status(status);
			}
			std::vector<DownRules::Helper::Near> near;
			for (const auto other : a_players) {
				if (other != player) {
					near.push_back({ other->client.Id(), std::abs(other->x - player->x) });
				}
			}
			const bool able = player->game.withFriends && player->game.self.Now() == Phase::kUp && !player->game.body.dead;
			if (const auto helped = player->game.helper.Frame(able, near, a_now).helped) {
				player->client.Revive(helped);
				++player->helpedUp;
			}
		}
		a_session.Settle();
		for (const auto player : a_players) {
			for (const auto& [id, seen] : player->client.Friends()) {
				if (seen.status) {
					player->game.helper.SetFriendDown(id, seen.status->downed);
					if (const auto before = player->read.find(id); before != player->read.end()) {
						const auto news = DownRules::ReadFriendStatus(before->second.downed, before->second.health, seen.status->downed, seen.status->health);
						if (news != DownRules::FriendNews::kNone) {
							player->news.push_back(news);
						}
					}
					player->read[id] = *seen.status;
				}
			}
			const auto& revivedBy = player->client.RevivedBy();
			for (; player->revivesRead < revivedBy.size(); ++player->revivesRead) {
				player->game.Revived(a_now);
			}
		}
	}

	void Play(Sim::Session& a_session, std::span<Player*> a_players, Clock::time_point& a_now, Clock::duration a_for)
	{
		for (const auto end = a_now + a_for; a_now < end; a_now += 250ms) {
			Tick(a_session, a_players, a_now);
		}
	}
}

TEST("downed: through the server, a friend next to the downed host helps them up once, and the host stands up")
{
	Sim::Session s;
	Player       host{ s.Host() };
	Player       vm{ s.Player("VM") };
	REQUIRE(host.client.Join());
	REQUIRE(vm.client.Join());
	std::array<Player*, 2> players{ &host, &vm };
	auto                   now = Start();
	host.x = 0.0f;
	vm.x = 100.0f;  // right next to them
	Play(s, players, now, 1s);

	host.game.body.LethalHit();
	Play(s, players, now, 1s);
	REQUIRE(vm.client.FriendById(host.client.Id())->status);
	CHECK(vm.client.FriendById(host.client.Id())->status->downed);

	// The friend stays next to them while the game takes its time standing the host up: one help.
	Play(s, players, now, 15s);
	CHECK(vm.helpedUp == 1);
	CHECK(host.revivesRead == 1);
	CHECK(host.game.self.revives == 1);
	CHECK(host.game.self.Now() == Phase::kUp);
	CHECK(!host.game.body.down);
	CHECK(!host.game.body.dead);
	CHECK(!vm.client.FriendById(host.client.Id())->status->downed);
	CHECK(host.game.kills == 0);
	// Helped and still on the ground, the host was told to the friend as no longer down with no health (the game
	// says dead while down): the friend read "Host died", and "is back up" never came.
	CHECK(vm.Heard(DownRules::FriendNews::kDown) == 1);
	CHECK(vm.Heard(DownRules::FriendNews::kDied) == 0);
	CHECK(vm.Heard(DownRules::FriendNews::kBackUp) == 1);

	// Down again later: helped again.
	host.game.body.LethalHit();
	Play(s, players, now, 15s);
	CHECK(vm.helpedUp == 2);
	CHECK(host.game.self.revives == 2);
	CHECK(host.game.self.Now() == Phase::kUp);
	CHECK(vm.Heard(DownRules::FriendNews::kDown) == 2);
	CHECK(vm.Heard(DownRules::FriendNews::kDied) == 0);
	CHECK(vm.Heard(DownRules::FriendNews::kBackUp) == 2);
	CHECK(vm.client.FriendById(host.client.Id())->status->health > 0);
}

TEST("downed: through the server, a downed host nobody reaches bleeds out once and stays dead; the friend fighting with god mode on is never shown down")
{
	Sim::Session s;
	Player       host{ s.Host() };
	Player       vm{ s.Player("VM") };
	REQUIRE(host.client.Join());
	REQUIRE(vm.client.Join());
	std::array<Player*, 2> players{ &host, &vm };
	auto                   now = Start();
	host.x = 0.0f;
	vm.x = 2000.0f;
	vm.game.body.god = true;
	Play(s, players, now, 1s);

	host.game.body.LethalHit();
	for (auto end = now + DownRules::BLEEDOUT_TIME + 10s; now < end; now += 250ms) {
		vm.game.body.LethalHit();  // the fight goes on around the friend
		Tick(s, players, now);
	}
	CHECK(host.game.body.dead);
	CHECK(host.game.downNotices == 1);
	CHECK(host.game.kills == 1);
	CHECK(vm.helpedUp == 0);
	CHECK(!host.client.FriendById(vm.client.Id())->status->downed);
	CHECK(!vm.game.self.IsDown());
	CHECK(vm.client.FriendById(host.client.Id())->status->health == 0);
	CHECK(!vm.client.FriendById(host.client.Id())->status->downed);
	CHECK(vm.Heard(DownRules::FriendNews::kDown) == 1);
	CHECK(vm.Heard(DownRules::FriendNews::kDied) == 1);
	CHECK(vm.Heard(DownRules::FriendNews::kBackUp) == 0);
	CHECK(host.news.empty());  // the friend with god mode on: nothing to tell
}

TEST("downed: through the server, two friends next to the downed host: only one help-up stands them up")
{
	Sim::Session s;
	Player       host{ s.Host() };
	Player       bob{ s.Player("Bob") };
	Player       carol{ s.Player("Carol") };
	REQUIRE(host.client.Join());
	REQUIRE(bob.client.Join());
	REQUIRE(carol.client.Join());
	std::array<Player*, 3> players{ &host, &bob, &carol };
	auto                   now = Start();
	bob.x = 80.0f;
	carol.x = -80.0f;
	Play(s, players, now, 1s);

	host.game.body.LethalHit();
	Play(s, players, now, 12s);
	CHECK(bob.helpedUp + carol.helpedUp >= 1);
	CHECK(bob.helpedUp + carol.helpedUp <= 2);  // both may finish in the same frame
	CHECK(host.game.self.revives == 1);
	CHECK(host.game.self.Now() == Phase::kUp);
	CHECK(host.game.body.trims == 1);
}

TEST("downed: a character down is not dead, though the game's IsDead says so; dying and dead are")
{
	CHECK(!DownRules::Dead(DownRules::LIFE_ESSENTIAL_DOWN, true));
	CHECK(!DownRules::Dead(DownRules::LIFE_BLEEDOUT, true));
	CHECK(DownRules::Dead(DownRules::LIFE_DYING, false));
	CHECK(DownRules::Dead(DownRules::LIFE_DEAD, false));
	CHECK(DownRules::Dead(0, true));
	CHECK(!DownRules::Dead(0, false));
}

// ---- the host's rules ----------------------------------------------------------------------------------

namespace
{
	// Down, helped up after a moment, and standing again.
	void DownAndUp(Game& a_game, Clock::time_point& a_now)
	{
		a_game.body.LethalHit();
		a_game.Run(a_now, 1s);
		REQUIRE(a_game.self.Now() == Phase::kDown);
		REQUIRE(a_game.Revived(a_now));
		a_game.Run(a_now, 5s);
		REQUIRE(a_game.self.Now() == Phase::kUp);
	}
}

TEST("downed rules: the session's defaults are the times and health every session had before there were rules")
{
	const auto rules = DownRules::Rules::From(Protocol::SessionRules{});
	CHECK(rules.revive);
	CHECK(rules.bleedOut == DownRules::BLEEDOUT_TIME);
	CHECK(rules.helpTime == DownRules::HELP_TIME);
	CHECK(rules.reviveHealth == 0.3f);
	CHECK(rules.maxRevives == 0);
	CHECK(rules.cooldown == Clock::duration{});
	CHECK(!rules.outsideCombat);

	// And with them nothing is limited: ten downs in a fight, each helped up.
	Game game;
	auto now = Start();
	game.self.SetRules(rules);
	game.fighting = true;
	game.Run(now, 1s);
	for (int i = 0; i < 10; ++i) {
		DownAndUp(game, now);
		CHECK(game.body.essential);
	}
	CHECK(game.self.RevivesLeft(now) == -1);
	CHECK(game.lastRevives == 0);
	CHECK(game.kills == 0);
}

TEST("downed rules: the bleed-out time is the host's, and a reminder comes only with time before it")
{
	const std::vector<std::pair<int, std::vector<int>>> cases{ { 90, { 30, 10 } }, { 45, { 30, 10 } }, { 20, { 10 } }, { 10, {} } };
	for (const auto& [seconds, expected] : cases) {
		Protocol::SessionRules session;
		session.bleedOutSeconds = static_cast<std::uint16_t>(seconds);
		Game game;
		auto now = Start();
		game.self.SetRules(DownRules::Rules::From(session));
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, std::chrono::seconds{ seconds } - 1s);
		CHECK(!game.body.dead);
		CHECK(game.self.Now() == Phase::kDown);
		game.Run(now, 2s);
		CHECK(game.body.dead);
		CHECK(game.reminders == expected);
	}
}

TEST("downed rules: out of revives for the fight, a lethal hit kills; after the fight they are back")
{
	Protocol::SessionRules session;
	session.maxRevives = 2;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.fighting = true;
	game.Run(now, 1s);
	DownAndUp(game, now);
	CHECK(game.self.RevivesLeft(now) == 1);
	CHECK(game.lastRevives == 0);
	CHECK(game.body.essential);
	DownAndUp(game, now);
	CHECK(game.self.RevivesLeft(now) == 0);
	CHECK(game.lastRevives == 1);  // "that was your last revive"
	CHECK(!game.body.essential);   // the next lethal hit kills

	// Still fighting a minute on: still none.
	game.Run(now, 60s);
	CHECK(!game.body.essential);
	CHECK(game.revivesBack == 0);

	// The fight is over: back once it has been for a while.
	game.fighting = false;
	game.Run(now, DownRules::FIGHT_OVER - 2s);
	CHECK(!game.body.essential);
	game.Run(now, 3s);
	CHECK(game.body.essential);
	CHECK(game.revivesBack == 1);
	CHECK(game.self.RevivesLeft(now) == 2);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kDown);
}

TEST("downed rules: out of revives, a lethal hit kills as it would alone")
{
	Protocol::SessionRules session;
	session.maxRevives = 1;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.fighting = true;
	game.Run(now, 1s);
	DownAndUp(game, now);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.body.dead);
	CHECK(game.downNotices == 1);
}

TEST("downed rules: a long down with no fight around does not end the fight: the revive still counts once up")
{
	Protocol::SessionRules session;
	session.maxRevives = 1;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 40s);  // down longer than FIGHT_OVER, nobody fighting
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	CHECK(!game.body.essential);
	CHECK(game.self.RevivesLeft(now) == 0);
	game.Run(now, DownRules::FIGHT_OVER);
	CHECK(game.body.essential);
}

TEST("downed rules: revives per window come back as each help-up leaves the window, fight or not")
{
	Protocol::SessionRules session;
	session.maxRevives = 2;
	session.reviveWindowMinutes = 10;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	DownAndUp(game, now);
	const auto first = now - 5s;  // helped up as DownAndUp began its last 5 s
	game.Run(now, 2min);  // no fight: a window does not care
	DownAndUp(game, now);
	CHECK(!game.body.essential);
	CHECK(game.lastRevives == 1);
	// Ten minutes after the first help-up, one is back.
	game.Run(now, first + 10min - now - 1s);
	CHECK(!game.body.essential);
	game.Run(now, 2s);
	CHECK(game.body.essential);
	CHECK(game.self.RevivesLeft(now) == 1);
	CHECK(game.revivesBack == 1);
}

TEST("downed rules: just helped up, a lethal hit kills until the cooldown is over")
{
	Protocol::SessionRules session;
	session.downCooldownSeconds = 30;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	REQUIRE(game.Revived(now));
	CHECK(game.self.AfterRevive(now).cooldownSeconds == 30);
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	CHECK(!game.body.essential);
	CHECK(game.self.InCooldown(now));
	game.Run(now, 29s);
	CHECK(game.body.essential);
	CHECK(!game.self.InCooldown(now));

	// Within it, a lethal hit is the end.
	Game again;
	now = Start();
	again.self.SetRules(DownRules::Rules::From(session));
	again.Run(now, 1s);
	DownAndUp(again, now);
	again.Run(now, 10s);
	again.body.LethalHit();
	again.Run(now, 1s);
	CHECK(again.body.dead);
}

TEST("downed rules: a new session's rules start the count over")
{
	Protocol::SessionRules session;
	session.maxRevives = 1;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.fighting = true;
	game.Run(now, 1s);
	DownAndUp(game, now);
	CHECK(!game.body.essential);
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	CHECK(game.body.essential);
}

TEST("downed rules: helping takes the host's time")
{
	Protocol::SessionRules session;
	session.helpTenths = 50;
	DownRules::Helper helper;
	helper.SetRules(DownRules::Rules::From(session));
	auto now = Start();
	helper.SetFriendDown(7, true);
	const std::vector<DownRules::Helper::Near> close{ { 7, 100.0f } };
	CHECK(helper.Frame(true, close, now).started == 7);
	now += 4900ms;
	CHECK(helper.Frame(true, close, now).helped == 0);
	now += 100ms;
	CHECK(helper.Frame(true, close, now).helped == 7);
}

TEST("downed rules: out of combat only, a helper in a fight is told once and helps after it")
{
	Protocol::SessionRules session;
	session.reviveOutsideCombat = true;
	DownRules::Helper helper;
	helper.SetRules(DownRules::Rules::From(session));
	auto now = Start();
	helper.SetFriendDown(7, true);
	const std::vector<DownRules::Helper::Near> close{ { 7, 100.0f } };
	CHECK(helper.Frame(true, close, now, true).blocked == 7);
	for (int i = 0; i < 50; ++i) {
		now += FRAME;
		const auto step = helper.Frame(true, close, now, true);
		CHECK(step.blocked == 0);  // once, not every frame
		CHECK(step.started == 0);
		CHECK(step.helped == 0);
	}
	CHECK(helper.Frame(true, close, now, false).started == 7);  // the fight is over
	now += DownRules::HELP_TIME;
	CHECK(helper.Frame(true, close, now, false).helped == 7);

	// Without the rule a fight changes nothing.
	DownRules::Helper anyTime;
	anyTime.SetFriendDown(7, true);
	CHECK(anyTime.Frame(true, close, now, true).started == 7);
	now += DownRules::HELP_TIME;
	CHECK(anyTime.Frame(true, close, now, true).helped == 7);
}

// ---- a downed friend's stand-in ------------------------------------------------------------------------

TEST("downed rules: a downed friend's stand-in lies down once it can, and gets up when they do")
{
	using Do = DownRules::StandInDown::Do;
	DownRules::StandInDown down;
	auto                   now = Start();
	// Up: nothing.
	CHECK(down.Frame(false, true, false, now) == Do::kNone);
	CHECK(!down.Laid());
	// Down, but the stand-in can't lie yet (its 3D, a frame switch on its way): it waits.
	CHECK(down.Frame(true, false, false, now) == Do::kNone);
	now += FRAME;
	CHECK(down.Frame(true, true, false, now) == Do::kLieDown);
	CHECK(down.Laid());
	CHECK(down.Lying());
	// Lying: nothing more while they are down.
	for (int i = 0; i < 50; ++i) {
		now += FRAME;
		CHECK(down.Frame(true, true, true, now) == Do::kNone);
	}
	CHECK(down.lieDowns == 1);
	// Up again: it gets up, and is ours until its life state says so.
	now += FRAME;
	CHECK(down.Frame(false, true, true, now) == Do::kGetUp);
	CHECK(down.Laid());
	now += FRAME;
	CHECK(down.Frame(false, true, true, now) == Do::kNone);
	CHECK(down.Laid());
	now += FRAME;
	CHECK(down.Frame(false, true, false, now) == Do::kNone);
	CHECK(!down.Laid());
}

TEST("downed rules: a downed friend's stand-in stood up by something here is laid down again a while later")
{
	using Do = DownRules::StandInDown::Do;
	DownRules::StandInDown down;
	auto                   now = Start();
	CHECK(down.Frame(true, true, false, now) == Do::kLieDown);
	now += FRAME;
	// The engine's own recovery stood it up: not ours any more, and laid down again only after LIE_RETRY.
	CHECK(down.Frame(true, true, false, now) == Do::kNone);
	CHECK(!down.Laid());
	CHECK(down.stoodUp == 1);
	now += DownRules::StandInDown::LIE_RETRY - 2 * FRAME;
	CHECK(down.Frame(true, true, false, now) == Do::kNone);
	now += 2 * FRAME;
	CHECK(down.Frame(true, true, false, now) == Do::kLieDown);
	CHECK(down.lieDowns == 2);
}

TEST("downed rules: a downed friend's stand-in that will not get up is placed again; one down again lies again")
{
	using Do = DownRules::StandInDown::Do;
	DownRules::StandInDown down;
	auto                   now = Start();
	CHECK(down.Frame(true, true, false, now) == Do::kLieDown);
	now += FRAME;
	CHECK(down.Frame(false, true, true, now) == Do::kGetUp);
	// Still down for GET_UP_LIMIT: replaced, standing.
	now += DownRules::StandInDown::GET_UP_LIMIT - FRAME;
	CHECK(down.Frame(false, true, true, now) == Do::kNone);
	now += FRAME;
	CHECK(down.Frame(false, true, true, now) == Do::kReplace);
	CHECK(!down.Laid());
	// Down again while getting up: lies down again at once.
	DownRules::StandInDown again;
	CHECK(again.Frame(true, true, false, now) == Do::kLieDown);
	now += FRAME;
	CHECK(again.Frame(false, true, true, now) == Do::kGetUp);
	now += FRAME;
	CHECK(again.Frame(true, true, true, now) == Do::kLieDown);
	CHECK(again.Lying());
	// A stand-in it laid down is not broken for it; any other down one still is.
	CHECK(DownRules::StandInBroken(DownRules::LIFE_ESSENTIAL_DOWN, false, false));
}

TEST("downed rules: a welcome back into the same session keeps the revives used and the cooldown; another session starts them over")
{
	constexpr std::uint64_t SESSION = 0x0000000100000000ull, OTHER = 0x0000000200000000ull;
	Protocol::SessionRules session;
	session.maxRevives = 1;
	session.downCooldownSeconds = 30;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session), SESSION);
	game.fighting = true;
	game.Run(now, 1s);
	DownAndUp(game, now);
	CHECK(!game.body.essential);
	CHECK(game.self.InCooldown(now));
	// The link drops (the session ends: the defaults, out of a session) and they join the same session again.
	game.self.SetRules(DownRules::Rules{}, 0);
	game.self.SetRules(DownRules::Rules::From(session), SESSION);
	game.Run(now, 1s);
	CHECK(!game.body.essential);  // out of revives still
	CHECK(game.self.InCooldown(now));
	CHECK(game.self.RevivesLeft(now) == 0);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.body.dead);

	// Another session: a new count.
	Game other;
	now = Start();
	other.self.SetRules(DownRules::Rules::From(session), SESSION);
	other.fighting = true;
	other.Run(now, 1s);
	DownAndUp(other, now);
	other.Run(now, 31s);
	CHECK(!other.body.essential);
	other.self.SetRules(DownRules::Rules::From(session), OTHER);
	other.Run(now, 1s);
	CHECK(other.body.essential);
	CHECK(other.self.RevivesLeft(now) == 1);
}

// ---- crawling while down (bDownedCrawl) ------------------------------------------------------------------

namespace
{
	using Crawl = DownRules::Crawl;

	bool Close(float a_value, float a_expected)
	{
		return std::fabs(a_value - a_expected) < 0.001f;
	}

	// The player down and waiting for help, in a session whose rules let them crawl (or not).
	void DownIn(Game& a_game, Clock::time_point& a_now, bool a_crawl = true)
	{
		Protocol::SessionRules session;
		session.downedCrawl = a_crawl;
		a_game.self.SetRules(DownRules::Rules::From(session));
		a_game.Run(a_now, 1s);
		a_game.body.LethalHit();
		a_game.Run(a_now, 1s);
	}

	// What Downed.cpp tells the crawl of a frame with this input and nothing in the way.
	Crawl::Facts Pushing(const DownRules::Self& a_self, float a_x, float a_y, float a_seconds = 0.05f)
	{
		return { .may = a_self.MayCrawl(), .x = a_x, .y = a_y, .seconds = a_seconds };
	}
}

TEST("downed crawl: forward input moves along the heading at crawl speed, backward at half")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	REQUIRE(game.self.Now() == Phase::kDown);
	REQUIRE(game.self.MayCrawl());

	const auto forward = Crawl::Wanted(Pushing(game.self, 0.0f, 1.0f));
	CHECK(Close(forward.forward, DownRules::CRAWL_SPEED * 0.05f));
	CHECK(forward.turn == 0.0f);
	const auto back = Crawl::Wanted(Pushing(game.self, 0.0f, -1.0f));
	CHECK(Close(back.forward, -DownRules::CRAWL_SPEED * 0.05f / 2.0f));
	// Half the stick, half the speed; a stick at rest that drifts a little is no input.
	CHECK(Close(Crawl::Wanted(Pushing(game.self, 0.0f, 0.5f)).forward, DownRules::CRAWL_SPEED * 0.05f / 2.0f));
	CHECK(!Crawl::Wanted(Pushing(game.self, 0.05f, 0.1f)).Any());
	CHECK(!Crawl::Pushed(0.05f, 0.1f));
	CHECK(Crawl::Pushed(0.0f, 1.0f));

	// Along the heading, which counts from north (y) toward east (x).
	const auto north = Crawl::Along(0.0f, 10.0f);
	CHECK(Close(north[0], 0.0f));
	CHECK(Close(north[1], 10.0f));
	const auto east = Crawl::Along(1.5707964f, 10.0f);
	CHECK(Close(east[0], 10.0f));
	CHECK(Close(east[1], 0.0f));

	// A whole bleed-out of crawling covers about 25 m, no more.
	float crawled = 0.0f;
	for (std::chrono::milliseconds left = DownRules::BLEEDOUT_TIME; left > 0ms; left -= 50ms) {
		crawled += Crawl::Wanted(Pushing(game.self, 0.0f, 1.0f)).forward;
	}
	CHECK(crawled > 1700.0f);
	CHECK(crawled < 1900.0f);
}

TEST("downed crawl: sideways input turns the body and does not move it")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	const auto right = Crawl::Wanted(Pushing(game.self, 1.0f, 0.0f));
	CHECK(Close(right.turn, DownRules::CRAWL_TURN * 0.05f));
	CHECK(right.forward == 0.0f);
	const auto left = Crawl::Wanted(Pushing(game.self, -1.0f, 0.0f));
	CHECK(Close(left.turn, -DownRules::CRAWL_TURN * 0.05f));
	CHECK(left.forward == 0.0f);
	// The heading stays within a turn.
	CHECK(Close(Crawl::Turned(6.2f, 0.2f), 6.2f + 0.2f - 6.2831853f));
	CHECK(Close(Crawl::Turned(0.1f, -0.2f), 6.2831853f - 0.1f));
	CHECK(Close(Crawl::Turned(1.0f, 0.5f), 1.5f));
}

TEST("downed crawl: nothing moves while up, getting up, bled out, paused, in the air, swimming, in furniture or knocked over")
{
	Game game;
	auto now = Start();
	Protocol::SessionRules session;
	session.downedCrawl = true;
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	CHECK(!game.self.MayCrawl());  // up
	CHECK(!Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f)).Any());

	game.body.LethalHit();
	game.Run(now, 1s);
	REQUIRE(game.self.MayCrawl());
	const auto base = Pushing(game.self, 1.0f, 1.0f);
	CHECK(Crawl::Wanted(base).Any());
	for (const auto blocked : { &Crawl::Facts::paused, &Crawl::Facts::inAir, &Crawl::Facts::swimming, &Crawl::Facts::inFurniture,
			 &Crawl::Facts::knockedOver, &Crawl::Facts::inScene }) {
		auto facts = base;
		facts.*blocked = true;
		CHECK(!Crawl::Wanted(facts).Any());
	}

	// Helped and getting up.
	REQUIRE(game.Revived(now));
	CHECK(!game.self.MayCrawl());

	// Bled out.
	Game alone;
	now = Start();
	DownIn(alone, now);
	alone.Run(now, DownRules::BLEEDOUT_TIME);
	CHECK(alone.self.Now() == Phase::kBledOut);
	CHECK(!alone.self.MayCrawl());

	// Giving up stops it at once, before the bleed-out the next frame.
	Game givingUp;
	now = Start();
	DownIn(givingUp, now);
	givingUp.self.GiveUp();
	CHECK(!givingUp.self.MayCrawl());
}

TEST("downed crawl: stood up by the game while waiting for help, no crawl until the put-down takes")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	REQUIRE(game.self.MayCrawl());
	// The game stands them up (clutter) and the engine's put-down does not take for a while.
	game.body.putDownIgnored = true;
	game.body.StoodUpByGame();
	for (int i = 0; i < 10; ++i) {
		game.Frame(now);
		now += FRAME;
		CHECK(game.self.Now() == Phase::kDown);
		CHECK(!game.self.MayCrawl());
		CHECK(!Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f)).Any());
	}
	CHECK(game.stoodUp == 1);
	// The put-down takes: down again, and crawling again.
	game.body.putDownIgnored = false;
	game.Run(now, 1s);
	REQUIRE(game.body.down);
	CHECK(game.backDown == 1);
	CHECK(game.self.MayCrawl());
}

TEST("downed crawl: the rule off means no crawl")
{
	Game game;
	auto now = Start();
	DownIn(game, now, false);
	REQUIRE(game.self.Now() == Phase::kDown);
	CHECK(!game.self.MayCrawl());
	CHECK(!Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f)).Any());
	// The default rules, as before there was a crawl: off.
	CHECK(!DownRules::Rules{}.crawl);
	CHECK(!DownRules::Rules::From(Protocol::SessionRules{}).crawl);
}

TEST("downed crawl: a friend's stand-in within help range stops the crawl, and it resumes once they go")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	auto facts = Pushing(game.self, 0.0f, 1.0f);
	facts.friendNear = true;
	CHECK(!Crawl::Wanted(facts).Any());
	facts.friendNear = false;
	CHECK(Crawl::Wanted(facts).forward > 0.0f);
}

TEST("downed crawl: a long frame moves no farther than a tenth of a second's crawl")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	const auto hitch = Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f, 2.0f));
	CHECK(Close(hitch.forward, DownRules::CRAWL_SPEED * DownRules::CRAWL_LONGEST_FRAME));
	CHECK(Close(hitch.turn, DownRules::CRAWL_TURN * DownRules::CRAWL_LONGEST_FRAME));
	// A clock that went back, or no time to tell: nothing.
	CHECK(!Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f, -1.0f)).Any());
	CHECK(!Crawl::Wanted(Pushing(game.self, 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN())).Any());
	CHECK(!Crawl::Wanted(Pushing(game.self, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity())).Any());
}

TEST("downed crawl: a step into a wall, off a ledge, or over nothing is refused; a small rise or fall snaps to the ground")
{
	using Refused = Crawl::Refused;
	constexpr float FEET = 100.0f, STEP = 2.0f;
	// Flat ground and nothing in the way.
	const auto flat = Crawl::Accept(FEET, STEP, { .groundThere = FEET, .groundAhead = FEET });
	CHECK(flat.refused == Refused::kNone);
	CHECK(flat.z == FEET);
	// A wall within the step and the body: refused, forward or back; one farther on is no matter yet.
	CHECK(Crawl::Accept(FEET, STEP, { .wall = 15.0f, .groundThere = FEET, .groundAhead = FEET }).refused == Refused::kWall);
	CHECK(Crawl::Accept(FEET, -STEP, { .wall = 15.0f, .groundThere = FEET, .groundAhead = FEET }).refused == Refused::kWall);
	CHECK(Crawl::Accept(FEET, STEP, { .wall = STEP + DownRules::CRAWL_BODY + 1.0f, .groundThere = FEET, .groundAhead = FEET }).refused == Refused::kNone);
	// Nothing under the step, or ahead of it.
	CHECK(Crawl::Accept(FEET, STEP, { .groundAhead = FEET }).refused == Refused::kNoGround);
	CHECK(Crawl::Accept(FEET, STEP, { .groundThere = FEET }).refused == Refused::kNoGround);
	CHECK(Crawl::Accept(FEET, STEP, { .groundThere = std::numeric_limits<float>::quiet_NaN(), .groundAhead = FEET }).refused == Refused::kNoGround);
	// A ledge ahead, a cliff ahead.
	CHECK(Crawl::Accept(FEET, STEP, { .groundThere = FEET, .groundAhead = FEET - 60.0f }).refused == Refused::kLedge);
	CHECK(Crawl::Accept(FEET, STEP, { .groundThere = FEET, .groundAhead = FEET + 40.0f }).refused == Refused::kClimb);
	// A stair's step up, a kerb down: onto the ground.
	const auto up = Crawl::Accept(FEET, STEP, { .groundThere = FEET + 16.0f, .groundAhead = FEET + 16.0f });
	CHECK(up.refused == Refused::kNone);
	CHECK(up.z == FEET + 16.0f);
	const auto down = Crawl::Accept(FEET, STEP, { .groundThere = FEET - 10.0f, .groundAhead = FEET - 20.0f });
	CHECK(down.refused == Refused::kNone);
	CHECK(down.z == FEET - 10.0f);

	// What the rays count as solid: the world, not our own body or any character, nor loose clutter.
	CHECK(DownRules::CrawlStops(1));   // static
	CHECK(DownRules::CrawlStops(13));  // terrain
	CHECK(DownRules::CrawlStops(27));  // invisible wall
	CHECK(DownRules::CrawlStops(26));  // a railing, a grate
	CHECK(DownRules::CrawlStops(28));  // the same, animated
	CHECK(DownRules::CrawlStops(0));   // a hit that can't be told
	CHECK(!DownRules::CrawlStops(8));   // biped
	CHECK(!DownRules::CrawlStops(30));  // character controller
	CHECK(!DownRules::CrawlStops(33));  // biped, no character controller
	CHECK(!DownRules::CrawlStops(4));   // clutter
	CHECK(!DownRules::CrawlStops(12));  // trigger

	// The count, and a refusal logged once, not every frame it goes on.
	Crawl crawl;
	CHECK(!crawl.Count(flat, STEP));
	CHECK(crawl.Count({ Refused::kWall }, STEP));
	CHECK(!crawl.Count({ Refused::kWall }, STEP));
	CHECK(crawl.Count({ Refused::kLedge }, STEP));
	CHECK(!crawl.Count(flat, -STEP));
	CHECK(crawl.Count({ Refused::kWall }, STEP));
	CHECK(crawl.steps == 2);
	CHECK(crawl.refused == 4);
	CHECK(Close(crawl.distance, 2.0f * STEP));
	CHECK(crawl.Last() == Refused::kWall);
}

TEST("downed crawl: a load while down leaves nothing to crawl")
{
	Game game;
	auto now = Start();
	DownIn(game, now);
	REQUIRE(game.self.MayCrawl());
	// The save had the player down: they are stood up as if helped, and crawl no more.
	CHECK(game.self.Loaded(true, now));
	CHECK(!game.self.MayCrawl());
	CHECK(!Crawl::Wanted(Pushing(game.self, 0.0f, 1.0f)).Any());
	// A save made up: up.
	Game other;
	now = Start();
	DownIn(other, now);
	CHECK(!other.self.Loaded(false, now));
	CHECK(!other.self.MayCrawl());
}

TEST("downed: stood up with the console open (player.resurrect, a heal) while waiting for help, up it is: no putting down again")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kDown);
	game.console = true;
	game.body.StoodUpByGame();
	game.Frame(now);
	now += FRAME;
	CHECK(game.upByCommand == 1);
	CHECK(game.stoodUp == 0);
	CHECK(game.body.putDowns == 0);
	CHECK(!game.body.down);
	CHECK(!game.body.stayDown);
	CHECK(game.self.Now() == Phase::kUp);
	game.console = false;
	game.Run(now, 6s);
	CHECK(game.body.putDowns == 0);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.kills == 0);
	// The next lethal hit puts them down as usual.
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kDown);
}
