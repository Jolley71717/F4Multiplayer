// Downed and revive (src/game/DownRules.h, which Downed.cpp runs every frame): the player's own down state
// driven across whole sequences against a model of the game's side of it, and two players helping each other
// up through the real server, their status and Revive messages encoded and decoded as the plugin sends them.

#include "Test.h"
#include "sim/Sim.h"

#include "game/DownRules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
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
		bool getUpIgnored = false;    // the engine's own end of the down (Downed.cpp StandUp) does not take
		int  getUpsAsked = 0;
		bool knockedOver = false;     // a knock-down's ragdoll or its get-up
		bool sneaking = false;        // the sneak stance
		bool sneakHiddenDown = false; // the game's IsSneaking answers false while down, whatever the stance
		bool unsneakIgnored = false;  // our stand out of sneak does not take
		bool graphSneaking = false;   // the behaviour graph's own sneak, which the sneak key's start and stop must agree with
		bool stanceOnlyGetUp = false; // the game's end of the down stands them by the stance alone (SetSneaking), the graph left in sneak
		bool graphStopIgnored = false;  // the graph is still in the get-up and does not take a sneak stop
		int  graphStops = 0;
		bool bleedOutCamera = false;    // the player camera's bleed-out state, on or waiting to come back
		bool bleedOutLayer = false;     // the input layer the engine's start of a down puts on
		bool upLeavesBleedOut = false;  // the game's stand-ups other than the engine's end of the down leave both on (a knock-down's get-up)
		bool finishIgnored = false;     // our end of those (Downed.cpp FinishDown) does not take
		int  finishes = 0;
		// The player camera's states as the engine's start and end of a bleed-out move them (DownRules::Camera), and the
		// frames its transition back from the bleed-out camera takes.
		DownRules::Camera camera;
		int               transitionFrames = 5;
		int               transitionLeft = 0;
		int               lastView = DownRules::Camera::THIRD_PERSON;  // as Downed.cpp keeps it, while up
		int               cameraFixes = 0;
		bool              transitionFrozen = false;  // a transition whose time never runs out

		// The engine's start of a bleed-out (+1028580): the current state pushed (in a tween, the bleed-out camera), and
		// the bleed-out camera.
		void CameraStart()
		{
			if (camera.current == DownRules::Camera::BLEEDOUT) {
				return;
			}
			if (camera.current == DownRules::Camera::TWEEN) {
				camera.returns.push_back(DownRules::Camera::BLEEDOUT);
				return;
			}
			camera.returns.push_back(camera.current);
			camera.current = DownRules::Camera::BLEEDOUT;
		}

		// Its end (+1028600): from the bleed-out camera, a transition to the state popped (the first person if none);
		// with the bleed-out camera waiting on the stack, taken out of it.
		void CameraEnd()
		{
			if (camera.current == DownRules::Camera::BLEEDOUT) {
				int to = DownRules::Camera::FIRST_PERSON;
				if (!camera.returns.empty()) {
					to = camera.returns.back();
					camera.returns.pop_back();
				}
				camera.transitionTo = to;
				camera.current = DownRules::Camera::TRANSITION;
				transitionLeft = transitionFrames;
				return;
			}
			std::erase(camera.returns, DownRules::Camera::BLEEDOUT);
		}

		// A knock-down's ragdoll (+D4B4A0): the same start of the bleed-out camera; its end, as the player is up from it and
		// the knock state normal again (+D36149), the same end with a transition that takes no time.
		void KnockStart()
		{
			knockedOver = true;
			CameraStart();
		}

		void KnockEnd()
		{
			knockedOver = false;
			const bool fromBleedOut = camera.current == DownRules::Camera::BLEEDOUT;
			CameraEnd();
			if (fromBleedOut) {
				transitionLeft = 0;
			}
		}

		// The bleed-out camera on, or waiting on the return stack (Downed.cpp BleedOutCamera).
		[[nodiscard]] bool CameraHasBleedOut() const
		{
			return camera.current == DownRules::Camera::BLEEDOUT || std::ranges::find(camera.returns, DownRules::Camera::BLEEDOUT) != camera.returns.end();
		}

		// The camera's frame: a transition whose time is up switches to where it goes (to itself, it stays a transition).
		void CameraFrame()
		{
			if (camera.current != DownRules::Camera::TRANSITION) {
				return;
			}
			if (transitionLeft > 0 && !transitionFrozen) {
				--transitionLeft;
			}
			if (transitionLeft == 0 && camera.transitionTo >= 0) {
				camera.current = camera.transitionTo;
			}
		}

		[[nodiscard]] bool InView() const
		{
			return camera.current == DownRules::Camera::FIRST_PERSON || camera.current == DownRules::Camera::THIRD_PERSON;
		}

		// The engine's start of a down: the camera and the input layer with it.
		void GoDown()
		{
			down = true;
			bleedOutCamera = true;
			bleedOutLayer = true;
			CameraStart();
		}

		// Up from a down, however it ends. a_engineEnd: the life state's own end of the down (the engine's recovery, or
		// Downed.cpp StandUp), which takes the camera and the layer off; another path of the game's may not.
		void Up(bool a_engineEnd = false)
		{
			down = false;
			if (a_engineEnd || !upLeavesBleedOut) {
				bleedOutCamera = false;
				bleedOutLayer = false;
				CameraEnd();
			}
			if (stanceOnlyGetUp) {
				sneaking = false;
			}
		}

		// The game stands them up on a path the stay-down flag does not hold back.
		void StoodUpByGame()
		{
			if (down && !dead) {
				Up();
			}
		}

		// The sneak key: the stance says whether a start or a stop is asked of the graph, which refuses one it is already in.
		bool PressSneak()
		{
			if (sneaking != graphSneaking) {
				return false;
			}
			sneaking = !sneaking;
			graphSneaking = sneaking;
			return true;
		}

		void LethalHit()
		{
			if (god || dead) {
				return;
			}
			if (essential) {
				GoDown();
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
		int               gotUp = 0;
		int               settled = 0;
		int               unsneaks = 0;
		int               sneakKept = 0;
		int               strayEnds = 0;
		int               transitionEnds = 0;
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
					body.Up(true);
					body.fullHealth = true;
				}
			}
		}

		DownRules::Step Frame(Clock::time_point a_now)
		{
			// The game's own frame first: a pending kill, standing up. A menu stops all of it.
			if (!paused) {
				GameFrame();
				body.CameraFrame();
			}
			// As Downed.cpp keeps the last view: while up and out of any down.
			if (!body.down && !self.IsDown() && !self.Settling() && body.InView()) {
				body.lastView = body.camera.current;
			}

			// As Downed.cpp reads it: the life state, and the game's IsDead, which is true while down as well.
			const std::int32_t     lifeState = body.down ? DownRules::LIFE_ESSENTIAL_DOWN : body.dead ? DownRules::LIFE_DEAD : 0;
			const DownRules::Facts facts{ .enabled = withFriends && self.GetRules().revive, .essential = body.essential, .down = body.down, .dead = DownRules::Dead(lifeState, body.dead || body.down), .paused = paused, .inCombat = fighting, .console = console,
				.knockedOver = body.knockedOver, .sneaking = body.sneaking && !(body.sneakHiddenDown && body.down), .bleedOutCamera = body.bleedOutCamera || body.CameraHasBleedOut(),
				.bleedOutLayer = body.bleedOutLayer, .cameraLoop = body.camera.Loops(), .inTransition = body.camera.current == DownRules::Camera::TRANSITION };
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
					body.GoDown();
				}
			}
			if (step.standUp) {
				// Downed.cpp StandUp: the stay-down flag off, health, and the engine's own end of the down.
				++body.getUpsAsked;
				body.stayDown = false;
				if (!body.getUpIgnored && body.down && !body.dead) {
					body.Up(true);
					body.fullHealth = true;
				}
			}
			if (step.fixCamera) {
				// Downed.cpp FixCamera (before FinishDown, as there).
				body.camera = body.camera.Unlooped(body.lastView);
				++body.cameraFixes;
			}
			if (step.finishDown) {
				// Downed.cpp FinishDown: the engine's end of the camera's bleed-out state, and of the layer.
				++body.finishes;
				strayEnds += step.strayBleedOut;
				if (!body.finishIgnored) {
					body.bleedOutCamera = false;
					body.bleedOutLayer = false;
					body.CameraEnd();
				}
			}
			if (step.endTransition && body.camera.current == DownRules::Camera::TRANSITION) {
				// Downed.cpp EndTransition.
				body.camera.current = body.camera.StuckEnd(body.down, body.lastView);
				++transitionEnds;
			}
			if (step.unsneak) {
				// Downed.cpp Unsneak: the stance, and the graph told its sneak stop.
				body.sneaking = body.sneaking && body.unsneakIgnored;
				body.graphSneaking = body.graphSneaking && body.graphStopIgnored;
				++unsneaks;
			}
			if (step.graphSneakStop) {
				body.graphSneaking = body.graphSneaking && body.graphStopIgnored;
				++body.graphStops;
			}
			gotUp += step.gotUp;
			settled += step.settled;
			sneakKept += step.sneakKept;
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

		// As Downed::ApplyRevive: a friend's help stands us up only once per down (the next frame's standUp does it).
		bool Revived(Clock::time_point a_now)
		{
			if (!self.Revive(a_now)) {
				return false;
			}
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
	CHECK(game.self.IsDown());          // still on the ground until the next frame stands them up
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

TEST("downed: giving up bleeds out at once")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.self.GiveUp();
	game.Run(now, 1s);
	CHECK(game.kills == 1);
	CHECK(game.body.dead);
	CHECK(game.downNotices == 1);
}

TEST("downed: the last friend leaving does not kill: the player bleeds out on the clock, and a friend back before then can still help")
{
	{
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		game.withFriends = false;
		game.Run(now, 10s);
		CHECK(game.kills == 0);
		CHECK(game.self.IsDown());
		CHECK(game.body.essential);
		game.withFriends = true;
		REQUIRE(game.Revived(now));
		game.Run(now, 5s);
		CHECK(game.kills == 0);
		CHECK(game.self.Now() == Phase::kUp);
	}
	{
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		game.withFriends = false;
		game.Run(now, DownRules::BLEEDOUT_TIME - 5s);
		CHECK(game.kills == 0);
		game.Run(now, 10s);
		CHECK(game.kills == 1);
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
	game.body.getUpIgnored = true;  // nor our stand-up
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
	game.paused = true;  // the help-up comes while our game is in the menu
	REQUIRE(game.Revived(now));
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
	game.body.getUpIgnored = true;
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

TEST("downed rules: going down asks for the autosave only while the player has revives and no cooldown")
{
	// With revives, or the cooldown, to run out, a save made as they go down would make their death a respawn on the spot.
	Protocol::SessionRules session;
	session.saveOnDown = true;
	session.maxRevives = 1;
	session.downCooldownSeconds = 30;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(session));
	game.Run(now, 1s);
	CHECK(game.self.AsksForDownSave(now));
	game.body.LethalHit();
	game.Run(now, 1s);
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	CHECK(game.self.RevivesLeft(now) == 0);
	CHECK(!game.self.AsksForDownSave(now));

	// In the cooldown alone (revives without limit), and again after it.
	session.maxRevives = 0;
	Game other;
	auto later = Start();
	other.self.SetRules(DownRules::Rules::From(session));
	other.Run(later, 1s);
	other.body.LethalHit();
	other.Run(later, 1s);
	REQUIRE(other.Revived(later));
	other.Run(later, 5s);
	CHECK(other.self.InCooldown(later));
	CHECK(!other.self.AsksForDownSave(later));
	other.Run(later, 30s);
	CHECK(other.self.AsksForDownSave(later));

	// The host's rule off: never.
	session.saveOnDown = false;
	Game off;
	auto start = Start();
	off.self.SetRules(DownRules::Rules::From(session));
	CHECK(!off.self.AsksForDownSave(start));
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

TEST("downed rules: rules that come while the player is down wait until they are up")
{
	constexpr std::uint64_t SESSION = 0x0000000100000000ull;
	Protocol::SessionRules  tighter;
	tighter.bleedOutSeconds = 10;
	tighter.reviveHealth = 10;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(Protocol::SessionRules{}), SESSION);
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 20s);
	REQUIRE(game.self.Now() == Phase::kDown);
	game.self.SetRules(DownRules::Rules::From(tighter), SESSION);
	CHECK(game.self.RulesWaiting());
	CHECK(game.self.GetRules().bleedOut == DownRules::BLEEDOUT_TIME);
	game.withFriends = false;  // a dropped connection between the rules and the help: the rules still wait
	game.Run(now, 3s);
	game.withFriends = true;
	game.Run(now, 2s);
	CHECK(!game.body.dead);  // 25 s down, past the new 10 s: the old 45 s still hold
	CHECK(game.self.RulesWaiting());
	REQUIRE(game.Revived(now));
	CHECK(game.self.GetRules().reviveHealth == 0.3f);  // helped up with the health of the rules the down began with
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	CHECK(!game.self.RulesWaiting());
	CHECK(game.self.GetRules().bleedOut == 10s);
	CHECK(game.kills == 0);
}

TEST("downed rules: turning revive off while down does not kill; the next lethal hit does")
{
	Protocol::SessionRules off;
	off.revive = false;
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.self.Now() == Phase::kDown);
	game.self.SetRules(DownRules::Rules::From(off), 0);
	game.Run(now, 10s);
	CHECK(!game.body.dead);
	CHECK(game.self.Now() == Phase::kDown);
	CHECK(game.kills == 0);
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	CHECK(!game.body.essential);
	game.body.LethalHit();
	game.Run(now, 1s);
	CHECK(game.body.dead);
	CHECK(game.downNotices == 1);
}

TEST("downed rules: a shorter bleed-out applies to the next down")
{
	Protocol::SessionRules shorter;
	shorter.bleedOutSeconds = 10;
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.self.SetRules(DownRules::Rules::From(shorter), 0);
	game.Run(now, 20s);
	REQUIRE(game.self.Now() == Phase::kDown);
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	// The next down bleeds out in the new 10 s.
	game.body.LethalHit();
	game.Run(now, 9s);
	CHECK(!game.body.dead);
	game.Run(now, 2s);
	CHECK(game.body.dead);
}

TEST("downed rules: new rules in the same session keep the revive count")
{
	constexpr std::uint64_t SESSION = 0x0000000100000000ull;
	Protocol::SessionRules  three;
	three.maxRevives = 3;
	Protocol::SessionRules two;
	two.maxRevives = 2;
	Game game;
	auto now = Start();
	game.self.SetRules(DownRules::Rules::From(three), SESSION);
	game.fighting = true;
	game.Run(now, 1s);
	DownAndUp(game, now);
	CHECK(game.self.RevivesLeft(now) == 2);
	game.self.SetRules(DownRules::Rules::From(two), SESSION);
	game.Run(now, 1s);
	CHECK(game.self.RevivesLeft(now) == 1);
	// Changed while down: the count goes on as well, under the new limit once up.
	game.body.LethalHit();
	game.Run(now, 1s);
	game.self.SetRules(DownRules::Rules::From(three), SESSION);
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	REQUIRE(game.self.Now() == Phase::kUp);
	CHECK(game.self.RevivesLeft(now) == 1);
	CHECK(game.body.essential);
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

TEST("downed get-up: helped up, the engine's own get-up is asked for at once, and the player is up the next frame")
{
	// Left to the game's update, a revived host stood up on the bleed-out's own path and, on their own screen, sank
	// into the ground: the get-up is the engine's full end of a down, asked for straight away.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	CHECK(game.body.getUpsAsked == 0);  // the help-up itself only marks them helped
	game.Frame(now);
	now += FRAME;
	CHECK(game.body.getUpsAsked == 1);
	CHECK(!game.body.down);
	CHECK(game.gotUp == 0);  // seen up only by the next frame
	game.Frame(now);
	now += FRAME;
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.gotUp == 1);
	CHECK(game.body.trims == 1);
	game.Run(now, 5s);
	CHECK(game.body.getUpsAsked == 1);  // once up, never again
	CHECK(game.gotUp == 1);
	CHECK(game.settled == 1);
	CHECK(game.self.standUpAsks == 1);
	CHECK(game.self.getUps == 1);
}

TEST("downed get-up: a get-up that does not take is asked for again every second, never in the pause menu")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.getUpIgnored = true;
	game.body.standUpFrames = 1000;  // nor the game's own recovery, for now
	REQUIRE(game.Revived(now));
	game.Run(now, 3s);
	CHECK(game.body.getUpsAsked == 3);  // at 0, 1 and 2 s
	game.paused = true;
	game.Run(now, 10s);
	CHECK(game.body.getUpsAsked == 3);  // a stopped game stands nobody up
	game.paused = false;
	game.body.getUpIgnored = false;
	game.Run(now, 1s);
	CHECK(game.body.getUpsAsked == 4);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.kills == 0);
}

TEST("downed get-up: helped up while knocked over, the get-up waits for the knock-down to end, KNOCK_WAIT at most")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.knockedOver = true;  // a ragdoll the bleed-out's end would not take in
	REQUIRE(game.Revived(now));
	game.Run(now, 2s);
	CHECK(game.body.getUpsAsked == 0);
	CHECK(game.body.down);
	game.body.knockedOver = false;
	game.Frame(now);
	now += FRAME;
	CHECK(game.body.getUpsAsked == 1);
	CHECK(!game.body.down);

	// Knocked over for good (a ragdoll that never settles): asked anyway once KNOCK_WAIT is over.
	Game stuck;
	auto then = Start();
	stuck.Run(then, 1s);
	stuck.body.LethalHit();
	stuck.Run(then, 2s);
	stuck.body.knockedOver = true;
	REQUIRE(stuck.Revived(then));
	stuck.Run(then, DownRules::KNOCK_WAIT);
	CHECK(stuck.body.getUpsAsked == 0);
	stuck.Run(then, FRAME);
	CHECK(stuck.body.getUpsAsked == 1);
	CHECK(!stuck.body.down);
}

TEST("downed get-up: sneaking as they went down, the player stands as they get up; a sneak after that is theirs")
{
	// The game keeps the sneak stance through a down: a player helped up was left in sneak.
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.Run(now, 1s);
	CHECK(game.unsneaks == 0);  // sneaking while up is the player's own
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(game.unsneaks == 1);
	CHECK(game.sneakKept == 0);
	CHECK(!game.body.sneaking);
	game.body.sneaking = true;  // the player presses the sneak key before the get-up has played out
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.unsneaks == 1);  // theirs: not overridden
	CHECK(game.body.sneaking);
	game.Run(now, 10s);
	CHECK(game.unsneaks == 1);
	CHECK(game.body.sneaking);
	CHECK(game.self.unsneaks == 1);
}

TEST("downed get-up: a stand out of sneak that does not take is asked once more when the get-up has played out")
{
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.body.unsneakIgnored = true;
	game.Run(now, 1s);
	CHECK(game.unsneaks == 1);
	CHECK(game.body.sneaking);
	game.body.unsneakIgnored = false;
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.unsneaks == 2);
	CHECK(!game.body.sneaking);
	game.Run(now, 10s);
	CHECK(game.unsneaks == 2);

	// Not taken, then the player stands and sneaks again themselves before it has played out: theirs.
	Game pressed;
	auto then = Start();
	pressed.body.sneaking = true;
	pressed.Run(then, 1s);
	pressed.body.LethalHit();
	pressed.Run(then, 2s);
	REQUIRE(pressed.Revived(then));
	pressed.body.unsneakIgnored = true;
	pressed.Run(then, 500ms);
	REQUIRE(pressed.unsneaks == 1);
	pressed.body.unsneakIgnored = false;
	pressed.body.sneaking = false;
	pressed.Run(then, 300ms);
	pressed.body.sneaking = true;
	pressed.Run(then, DownRules::GET_UP_SETTLE);
	CHECK(pressed.settled == 1);
	CHECK(pressed.unsneaks == 1);
	CHECK(pressed.body.sneaking);
}

TEST("downed get-up: down standing, the sneak key pressed while down, they get up sneaking as they chose")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.body.sneaking = true;  // the bleed-out's controls let the sneak key through
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(game.sneakKept == 1);
	CHECK(game.unsneaks == 0);
	game.Run(now, DownRules::GET_UP_SETTLE + 1s);
	CHECK(game.settled == 1);
	CHECK(game.unsneaks == 0);
	CHECK(game.body.sneaking);
}

TEST("downed get-up: the sneak they went down in is read as last seen up, whatever the game answers while down")
{
	// Down sneaking, the game's IsSneaking may answer false while the player lies there, the stance kept: still stood.
	Game game;
	auto now = Start();
	game.body.sneakHiddenDown = true;
	game.body.sneaking = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(game.unsneaks == 1);
	CHECK(game.sneakKept == 0);
	CHECK(!game.body.sneaking);

	// Down standing, the same: nothing to stand.
	Game standing;
	auto then = Start();
	standing.body.sneakHiddenDown = true;
	standing.Run(then, 1s);
	standing.body.LethalHit();
	standing.Run(then, 2s);
	REQUIRE(standing.Revived(then));
	standing.Run(then, DownRules::GET_UP_SETTLE + 1s);
	CHECK(standing.unsneaks == 0);
	CHECK(standing.sneakKept == 0);
}

TEST("downed get-up: stood by the stance alone, the graph left in its sneak, it is told to stop and the sneak key works again")
{
	// The game's end of the down unsneaks by the stance alone: the graph stayed in sneak, and the key asked a start of it
	// that it refused, until a load.
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.body.graphSneaking = true;
	game.body.stanceOnlyGetUp = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(game.unsneaks == 0);  // the stance is standing already
	CHECK(game.sneakKept == 0);
	CHECK(game.body.graphStops == 1);
	CHECK(!game.body.graphSneaking);
	CHECK(game.body.PressSneak());  // the key sneaks again
	CHECK(game.body.sneaking);
	game.Run(now, DownRules::GET_UP_SETTLE + 1s);
	CHECK(game.settled == 1);
	CHECK(game.body.graphStops == 1);  // their sneak since: not stopped
	CHECK(game.body.graphSneaking);
	CHECK(game.body.PressSneak());
	CHECK(!game.body.sneaking);
	CHECK(game.self.graphSneakStops == 1);

	// Without the stop the key does nothing: the model's own check.
	Game stuck;
	stuck.body.graphSneaking = true;
	CHECK(!stuck.body.PressSneak());
	CHECK(!stuck.body.sneaking);
}

TEST("downed get-up: stood by the stance alone, a sneak and a stand of their own before the get-up has played out are not followed by a stop")
{
	// Their key, twice: the stance is back where it was at the get-up, but the graph's stand is theirs, and a stop then
	// could cut off a sneak they start a moment later.
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.body.graphSneaking = true;
	game.body.stanceOnlyGetUp = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.body.graphStops == 1);
	CHECK(game.body.PressSneak());
	game.Run(now, 200ms);
	CHECK(game.body.PressSneak());
	CHECK(!game.body.sneaking);
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.body.graphStops == 1);
	CHECK(game.self.graphSneakStops == 1);
}

TEST("downed get-up: a sneak stop the graph does not take as they get up is told once more when the get-up has played out")
{
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.body.graphSneaking = true;
	game.body.stanceOnlyGetUp = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.body.graphStopIgnored = true;  // still in the get-up
	game.Run(now, 1s);
	CHECK(game.body.graphStops == 1);
	CHECK(game.body.graphSneaking);
	CHECK(!game.body.PressSneak());  // the key refused
	game.body.graphStopIgnored = false;
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.body.graphStops == 2);
	CHECK(!game.body.graphSneaking);
	CHECK(game.body.PressSneak());
	CHECK(game.body.sneaking);
	game.Run(now, 10s);
	CHECK(game.body.graphStops == 2);
	CHECK(game.self.graphSneakStops == 2);
}

TEST("downed get-up: stood out of sneak by us, the graph is told its stop once more when the get-up has played out")
{
	// Our stand (Unsneak) tells the graph its stop as they get up, when the graph may still be in the get-up.
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.body.graphSneaking = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.body.graphStopIgnored = true;
	game.Run(now, 1s);
	CHECK(game.unsneaks == 1);
	CHECK(!game.body.sneaking);
	CHECK(game.body.graphSneaking);
	CHECK(game.body.graphStops == 0);  // the stand told it
	game.body.graphStopIgnored = false;
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.unsneaks == 1);
	CHECK(game.body.graphStops == 1);
	CHECK(!game.body.graphSneaking);
	CHECK(game.body.PressSneak());
}

TEST("downed get-up: down standing, the graph is never told a sneak stop")
{
	Game game;
	auto now = Start();
	game.body.stanceOnlyGetUp = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, DownRules::GET_UP_SETTLE + 2s);
	CHECK(game.settled == 1);
	CHECK(game.body.graphStops == 0);
	CHECK(game.self.graphSneakStops == 0);
	CHECK(game.body.PressSneak());
}

TEST("downed get-up: the log's stand-up count is this get-up's, not the session's")
{
	Game game;
	auto now = Start();
	for (int down = 0; down < 5; ++down) {
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		REQUIRE(game.Revived(now));
		game.Run(now, 1s);
		CHECK(game.self.standUpAsksNow == 1);
		game.Run(now, DownRules::FIGHT_OVER + 1s);
	}
	CHECK(game.self.standUpAsks == 5);
}

TEST("downed get-up: not sneaking, getting up stands nobody; knocked over or in a menu, the settle waits")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(game.unsneaks == 0);
	game.body.knockedOver = true;
	game.Run(now, 3s);
	game.body.knockedOver = false;
	game.paused = true;
	game.Run(now, 3s);
	CHECK(game.settled == 0);
	game.paused = false;
	game.Frame(now);
	now += FRAME;
	CHECK(game.settled == 1);
	CHECK(game.unsneaks == 0);
}

TEST("downed get-up: down again before the get-up has played out, nothing is left to finish")
{
	Game game;
	auto now = Start();
	game.body.sneaking = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 500ms);
	REQUIRE(game.gotUp == 1);
	game.body.LethalHit();
	game.Run(now, 5s);
	CHECK(game.settled == 0);
	CHECK(game.unsneaks == 1);
}

TEST("downed get-up: however the down ends with the player up, they get up the same way; dead, they do not")
{
	const auto downed = [](Game& a_game, Clock::time_point& a_now) {
		a_game.body.sneaking = true;
		a_game.Run(a_now, 1s);
		a_game.body.LethalHit();
		a_game.Run(a_now, 2s);
		REQUIRE(a_game.self.Now() == Phase::kDown);
	};

	// A bleed-out the game ends by standing them up.
	{
		Game game;
		auto now = Start();
		downed(game, now);
		game.body.killIgnored = true;
		game.body.standUpFrames = 5;
		game.Run(now, DownRules::BLEEDOUT_TIME + 2s);
		CHECK(game.self.Now() == Phase::kUp);
		CHECK(game.gotUp == 1);
		CHECK(!game.body.sneaking);
	}
	// Stood up as the last friend leaves.
	{
		Game game;
		auto now = Start();
		downed(game, now);
		game.withFriends = false;
		game.body.StoodUpByGame();
		game.Run(now, 1s);
		CHECK(game.self.Now() == Phase::kUp);
		CHECK(game.gotUp == 1);
		CHECK(!game.body.sneaking);
	}
	// Stood up with the console.
	{
		Game game;
		auto now = Start();
		downed(game, now);
		game.console = true;
		game.body.StoodUpByGame();
		game.Run(now, 1s);
		CHECK(game.upByCommand == 1);
		CHECK(game.gotUp == 1);
		CHECK(!game.body.sneaking);
	}
	// A save loaded with the player down: stood up the engine's way, as if helped.
	{
		Game game;
		auto now = Start();
		game.body.sneaking = true;
		game.body.down = true;
		REQUIRE(game.self.Loaded(true, now));
		game.body.essential = true;  // as Downed::OnGameLoaded does
		game.Run(now, 1s);
		CHECK(game.body.getUpsAsked == 1);
		CHECK(game.self.Now() == Phase::kUp);
		CHECK(game.gotUp == 1);
		CHECK(!game.body.sneaking);
	}
	// Bled out and dead: nothing to get up from.
	{
		Game game;
		auto now = Start();
		downed(game, now);
		game.Run(now, DownRules::BLEEDOUT_TIME + 5s);
		CHECK(game.body.dead);
		CHECK(game.gotUp == 0);
		CHECK(game.unsneaks == 0);
	}
}

TEST("downed get-up: helped up the engine's way, the bleed-out camera and layer go with the down and nothing more is ended")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	REQUIRE(game.body.bleedOutCamera);
	REQUIRE(game.body.bleedOutLayer);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	CHECK(game.gotUp == 1);
	CHECK(!game.body.bleedOutCamera);
	CHECK(!game.body.bleedOutLayer);
	// A knock-down of its own while the get-up plays out: the ragdoll has the bleed-out camera, and its own end takes it off.
	game.body.knockedOver = true;
	game.body.bleedOutCamera = true;
	game.Run(now, 2s);
	game.body.knockedOver = false;
	game.body.bleedOutCamera = false;
	game.Run(now, DownRules::GET_UP_SETTLE + 1s);
	CHECK(game.settled == 1);
	// And one once settled is nothing of the down's.
	game.body.knockedOver = true;
	game.body.bleedOutCamera = true;
	game.Run(now, 3s);
	CHECK(game.body.finishes == 0);
	CHECK(game.self.downFinishes == 0);
}

TEST("downed get-up: helped up knocked over, and the knock-down's get-up stands them first: the camera and layer it left on are ended once it is over")
{
	// Downed by a grenade that knocked them over, a player was stood up by the ragdoll's get-up before the engine's end of
	// the down could be asked, which then did nothing: the camera stayed on the ground and the Pip-Boy would not open.
	Game game;
	auto now = Start();
	game.body.upLeavesBleedOut = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.body.knockedOver = true;
	game.Run(now, 2s);
	REQUIRE(game.Revived(now));
	game.Run(now, 1s);
	REQUIRE(game.body.getUpsAsked == 0);  // waiting for the knock-down (KNOCK_WAIT)
	game.body.StoodUpByGame();          // its get-up, still playing
	game.Run(now, 1s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.gotUp == 1);
	CHECK(game.body.getUpsAsked == 0);
	CHECK(game.body.finishes == 0);  // not while knocked over: that camera may be the knock-down's
	CHECK(game.body.bleedOutCamera);
	game.body.knockedOver = false;
	game.Frame(now);
	now += FRAME;
	CHECK(game.body.finishes == 1);  // at once, not when the get-up has played out
	CHECK(!game.body.bleedOutCamera);
	CHECK(!game.body.bleedOutLayer);
	game.Run(now, DownRules::GET_UP_SETTLE + 5s);
	CHECK(game.settled == 1);
	CHECK(game.body.finishes == 1);
	CHECK(game.self.downFinishes == 1);
	CHECK(game.kills == 0);
}

TEST("downed get-up: up some other way with the camera or the layer left on, they are ended as the player gets up")
{
	// Stood up with the console open: up it is, and the rest of the down is ended.
	{
		Game game;
		auto now = Start();
		game.body.upLeavesBleedOut = true;
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		game.console = true;
		game.body.StoodUpByGame();
		game.Frame(now);
		now += FRAME;
		CHECK(game.upByCommand == 1);
		CHECK(game.gotUp == 1);
		CHECK(game.body.finishes == 1);
		CHECK(!game.body.bleedOutCamera);
		CHECK(!game.body.bleedOutLayer);
	}
	// The layer alone left on.
	{
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		REQUIRE(game.Revived(now));
		game.Frame(now);
		now += FRAME;
		game.body.bleedOutLayer = true;  // as if the end had let go of the camera only
		game.Frame(now);
		now += FRAME;
		CHECK(game.gotUp == 1);
		CHECK(game.body.finishes == 1);
		CHECK(!game.body.bleedOutLayer);
	}
	// Up in the pause menu (the last friend gone as it opened): the end waits for the game to run.
	{
		Game game;
		auto now = Start();
		game.body.upLeavesBleedOut = true;
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		game.withFriends = false;
		game.paused = true;
		game.body.StoodUpByGame();
		game.Run(now, 2s);
		CHECK(game.gotUp == 1);
		CHECK(game.body.finishes == 0);
		game.paused = false;
		game.Frame(now);
		now += FRAME;
		CHECK(game.body.finishes == 1);
		CHECK(!game.body.bleedOutCamera);
	}
}

TEST("downed get-up: an end of the camera and layer that does not take is asked once more when the get-up has played out, then every few seconds")
{
	Game game;
	auto now = Start();
	game.body.upLeavesBleedOut = true;
	game.body.finishIgnored = true;
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	game.console = true;
	game.body.StoodUpByGame();
	game.Run(now, 1s);
	CHECK(game.body.finishes == 1);
	game.Run(now, DownRules::GET_UP_SETTLE);
	CHECK(game.settled == 1);
	CHECK(game.body.finishes == 2);
	CHECK(game.strayEnds == 0);
	// Up and settled and still on: a down's left behind, ended STRAY_WAIT on and every STRAY_RETRY after, never every frame.
	game.Run(now, DownRules::STRAY_WAIT + 2 * DownRules::STRAY_RETRY + 500ms);
	CHECK(game.body.finishes == 5);
	CHECK(game.strayEnds == 3);
	CHECK(game.self.strayEnds == 3);
	CHECK(game.self.downFinishes == 5);
	CHECK(game.body.bleedOutCamera);
	// Once it takes, nothing more.
	game.body.finishIgnored = false;
	game.Run(now, DownRules::STRAY_RETRY + 1s);
	CHECK(game.strayEnds == 4);
	CHECK(!game.body.bleedOutCamera);
	CHECK(!game.body.CameraHasBleedOut());
	CHECK(game.body.InView());
	game.Run(now, 20s);
	CHECK(game.strayEnds == 4);
}

TEST("downed camera: the engine's start and end of a bleed-out, as modelled, go back to the view the down began in")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 2s);
	CHECK(game.body.camera.current == DownRules::Camera::BLEEDOUT);
	REQUIRE(game.Revived(now));
	game.Run(now, 2s);
	CHECK(game.body.camera.current == DownRules::Camera::THIRD_PERSON);
	CHECK(game.body.camera.returns.empty());
	CHECK(game.body.cameraFixes == 0);
	CHECK(game.self.cameraFixes == 0);
}

TEST("downed camera: downed again while the camera is still on its way back from the last down, the next help-up goes back to the view")
{
	// A player's report: downed by a rocket engine's fire, helped up, downed again at once by the same fire; after the
	// second help-up the camera stayed by the ground and the Pip-Boy would not open until a load. The second start, made in
	// the first end's transition, pushed the transition state, and the second end made a transition to itself.
	for (const bool fix : { false, true }) {
		Game game;
		auto now = Start();
		game.body.transitionFrames = 10;  // 1 s
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 2s);
		REQUIRE(game.Revived(now));
		game.Frame(now);  // the stand-up: the camera's transition back begins
		now += FRAME;
		game.Frame(now);
		now += FRAME;
		REQUIRE(game.self.Now() == Phase::kUp);
		REQUIRE(game.body.camera.current == DownRules::Camera::TRANSITION);
		game.body.LethalHit();  // the fire again, the transition still running
		REQUIRE(game.body.camera.current == DownRules::Camera::BLEEDOUT);
		REQUIRE(game.body.camera.returns == std::vector<int>{ DownRules::Camera::TRANSITION });
		if (!fix) {
			// As the engine leaves it with nothing undone (before the fix): a transition to itself, for good.
			game.body.Up(true);
			for (int i = 0; i < 100; ++i) {
				game.body.CameraFrame();
			}
			CHECK(game.body.camera.current == DownRules::Camera::TRANSITION);
			CHECK(game.body.camera.transitionTo == DownRules::Camera::TRANSITION);
			CHECK(game.body.camera.Loops());
			continue;
		}
		game.Frame(now);  // down again: seen on the ground, undone at once
		now += FRAME;
		CHECK(game.self.Now() == Phase::kDown);
		CHECK(game.body.cameraFixes == 1);
		CHECK(game.body.camera.returns == std::vector<int>{ DownRules::Camera::THIRD_PERSON });
		game.Run(now, 2s);
		REQUIRE(game.Revived(now));
		game.Run(now, 3s);
		CHECK(game.self.Now() == Phase::kUp);
		CHECK(game.body.camera.current == DownRules::Camera::THIRD_PERSON);
		CHECK(game.body.camera.returns.empty());
		CHECK(!game.body.camera.Loops());
		CHECK(game.self.cameraFixes == 1);
		CHECK(game.kills == 0);
	}
}

TEST("downed camera: the transition on the return stack with nowhere known to go is sent to the last view, and the get-up ends there")
{
	Game game;
	auto now = Start();
	game.body.camera.current = DownRules::Camera::FIRST_PERSON;
	game.Run(now, 1s);
	CHECK(game.body.lastView == DownRules::Camera::FIRST_PERSON);
	game.body.LethalHit();
	game.Run(now, 2s);
	// As a start made in a transition leaves the stack, the transition never set going anywhere here.
	game.body.camera.returns = { DownRules::Camera::TRANSITION };
	game.body.transitionFrames = 30;
	REQUIRE(game.Revived(now));
	game.Run(now, 5s);
	CHECK(game.body.camera.current == DownRules::Camera::FIRST_PERSON);
	CHECK(!game.body.camera.Loops());
	CHECK(game.self.cameraFixes >= 1);
}

TEST("downed camera: a loop the fix does not undo is tried again once a second, up and settled too")
{
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.LethalHit();
	game.Run(now, 1s);
	// Something keeps the transition on the return stack: the fix is asked again every CAMERA_FIX_RETRY, not every frame.
	for (int i = 0; i < 30; ++i) {
		game.body.camera.returns = { DownRules::Camera::TRANSITION };
		game.Frame(now);
		now += FRAME;
	}
	CHECK(game.self.cameraFixes == 3);
	// Up and settled, a loop is still undone: the game never makes one of its own (the class's notes).
	game.body.camera.returns.clear();
	game.body.camera.returns.push_back(DownRules::Camera::THIRD_PERSON);
	REQUIRE(game.Revived(now));
	game.Run(now, DownRules::GET_UP_SETTLE + 3s);
	REQUIRE(game.settled == 1);
	const auto fixes = game.self.cameraFixes;
	game.body.camera.returns = { DownRules::Camera::TRANSITION };
	game.Frame(now);
	now += FRAME;
	CHECK(game.self.cameraFixes == fixes + 1);
	CHECK(game.body.camera.returns == std::vector<int>{ game.body.lastView });
	game.Run(now, 3s);
	CHECK(game.self.cameraFixes == fixes + 1);
}

TEST("downed camera: the loop is read and undone by the camera's own rules")
{
	using DownRules::Camera;
	CHECK(!Camera{}.Loops());
	CHECK(!(Camera{ .current = Camera::TRANSITION, .transitionTo = Camera::THIRD_PERSON }.Loops()));
	CHECK(!(Camera{ .current = Camera::TRANSITION, .transitionTo = -1 }.Loops()));  // its update does nothing
	CHECK((Camera{ .current = Camera::TRANSITION, .transitionTo = Camera::TRANSITION }.Loops()));
	CHECK(!(Camera{ .current = Camera::TRANSITION, .transitionTo = Camera::BLEEDOUT }.Loops()));  // a down begun in a tween
	CHECK((Camera{ .current = Camera::BLEEDOUT, .transitionTo = Camera::THIRD_PERSON, .returns = { Camera::FIRST_PERSON, Camera::TRANSITION } }.Loops()));
	// On the stack: where the transition was going, the view the first down left.
	const auto stacked = Camera{ .current = Camera::BLEEDOUT, .transitionTo = Camera::THIRD_PERSON, .returns = { Camera::TRANSITION } }.Unlooped(Camera::FIRST_PERSON);
	CHECK(stacked.returns == std::vector<int>{ Camera::THIRD_PERSON });
	CHECK(stacked.current == Camera::BLEEDOUT);
	CHECK(stacked.transitionTo == Camera::THIRD_PERSON);
	CHECK(!stacked.Loops());
	// Going nowhere better: the last view.
	const auto itself = Camera{ .current = Camera::TRANSITION, .transitionTo = Camera::TRANSITION, .returns = { Camera::TRANSITION } }.Unlooped(Camera::FIRST_PERSON);
	CHECK(itself.returns == std::vector<int>{ Camera::FIRST_PERSON });
	CHECK(itself.transitionTo == Camera::FIRST_PERSON);
	CHECK(!itself.Loops());
}

TEST("downed camera: downed again and again within moments of each stand-up, and once more while getting up, the camera ends in the view")
{
	// The same report again on 0.13.0: a joining player downed by a rocket engine's fire, helped up, and downed again by it
	// at once, several times over. Each next down begins while the camera is still on its way back from the one before.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	for (int round = 0; round < 5; ++round) {
		game.body.LethalHit();
		game.Run(now, 1s);
		REQUIRE(game.self.Now() == Phase::kDown);
		REQUIRE(game.Revived(now));
		game.Frame(now);  // the stand-up: the camera's transition back begins
		now += FRAME;
		REQUIRE(game.body.camera.current == DownRules::Camera::TRANSITION);
		if (round == 2) {
			// Down again before our next frame saw them up: still getting up, stood up again by STAND_RETRY.
			game.body.LethalHit();
			game.Run(now, DownRules::STAND_RETRY + 300ms);
			REQUIRE(!game.body.down);
			REQUIRE(game.body.camera.current == DownRules::Camera::TRANSITION);
		}
		game.Frame(now);
		now += FRAME;  // a moment up: the fire again
	}
	game.body.LethalHit();
	game.Run(now, 1s);
	REQUIRE(game.self.Now() == Phase::kDown);
	REQUIRE(game.Revived(now));
	game.Run(now, 10s);
	CHECK(game.self.Now() == Phase::kUp);
	CHECK(game.body.camera.current == DownRules::Camera::THIRD_PERSON);
	CHECK(!game.body.camera.Loops());
	CHECK(!game.body.CameraHasBleedOut());
	CHECK(game.self.cameraFixes >= 4);
	CHECK(game.kills == 0);
}

TEST("downed camera: a knock-down started in another's instant way back, out of any down, still has its loop undone")
{
	// Two ragdolls in one frame (a second explosion): the second start pushes the first end's transition, which takes no
	// time but has not run yet; the second end would make it a transition to itself. 0.13 looked only around a down.
	Game game;
	auto now = Start();
	game.Run(now, 1s);
	game.body.KnockStart();
	game.Run(now, 2s);
	game.body.KnockEnd();
	game.body.KnockStart();  // before the camera's next update
	REQUIRE(game.body.camera.returns == std::vector<int>{ DownRules::Camera::TRANSITION });
	game.Run(now, 3s);
	game.body.KnockEnd();
	game.Run(now, 2s);
	CHECK(game.body.InView());
	CHECK(!game.body.camera.Loops());
	CHECK(game.self.cameraFixes >= 1);
	CHECK(game.self.downs == 0);
}

TEST("downed camera: the bleed-out camera or layer on while up, settled and out of any down or knock-down is ended, after a moment")
{
	Game game;
	auto now = Start();
	game.Run(now, 5s);
	// As a start that no end followed would leave it: the camera in its bleed-out state, the player alive and up.
	game.body.CameraStart();
	game.body.bleedOutLayer = true;
	game.Run(now, DownRules::STRAY_WAIT - 200ms);
	CHECK(game.strayEnds == 0);  // not at once: the game's own ends may come a frame or two behind
	game.Run(now, 1s);
	CHECK(game.strayEnds == 1);
	CHECK(game.self.strayEnds == 1);
	CHECK(!game.body.bleedOutLayer);
	CHECK(!game.body.CameraHasBleedOut());
	game.Run(now, 1s);
	CHECK(game.body.InView());
	// Never while knocked over (the ragdoll's own), or in a menu.
	game.body.KnockStart();
	game.Run(now, 5s);
	game.paused = true;
	game.body.knockedOver = false;
	game.Run(now, 5s);
	CHECK(game.strayEnds == 1);
	game.paused = false;
	game.body.knockedOver = true;
	game.body.KnockEnd();
	game.Run(now, 5s);
	CHECK(game.strayEnds == 1);
	CHECK(game.body.InView());
}

TEST("downed camera: a transition left on far longer than the game's own is ended, where it goes or the view")
{
	// Where it goes: a view.
	{
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 1s);
		game.body.transitionFrozen = true;
		REQUIRE(game.Revived(now));
		game.Run(now, DownRules::TRANSITION_STUCK - 1s);
		REQUIRE(game.body.camera.current == DownRules::Camera::TRANSITION);
		CHECK(game.transitionEnds == 0);
		game.Run(now, 2s);
		CHECK(game.transitionEnds == 1);
		CHECK(game.body.camera.current == DownRules::Camera::THIRD_PERSON);
	}
	// Nowhere known, up: the last view. A menu's time does not count.
	{
		Game game;
		auto now = Start();
		game.body.camera.current = DownRules::Camera::FIRST_PERSON;
		game.Run(now, 1s);
		game.body.camera = { .current = DownRules::Camera::TRANSITION, .transitionTo = -1 };
		game.paused = true;
		game.Run(now, 10s);
		CHECK(game.transitionEnds == 0);
		game.paused = false;
		game.Run(now, DownRules::TRANSITION_STUCK + 500ms);
		CHECK(game.transitionEnds == 1);
		CHECK(game.body.camera.current == DownRules::Camera::FIRST_PERSON);
	}
	// Nowhere known while down: the bleed-out camera, which the down's end then takes off.
	{
		Game game;
		auto now = Start();
		game.Run(now, 1s);
		game.body.LethalHit();
		game.Run(now, 1s);
		game.body.camera.current = DownRules::Camera::TRANSITION;
		game.body.camera.transitionTo = -1;
		game.Run(now, DownRules::TRANSITION_STUCK + 500ms);
		CHECK(game.body.camera.current == DownRules::Camera::BLEEDOUT);
		REQUIRE(game.Revived(now));
		game.Run(now, 3s);
		CHECK(game.body.InView());
	}
}

TEST("downed camera: downs, help-ups, ragdolls and their ends in any order and timing, the camera always ends in the view")
{
	// Random sequences of what drives the engine's starts and ends of the bleed-out camera: lethal hits (on the ground,
	// getting up, just up), friends' help, ragdolls begun and ended, the game's own stand-ups, a few frames apart. Once it
	// is all over the camera must be a view, with no loop and no bleed-out camera or layer left anywhere.
	int                        windowMissed = 0;  // runs that reached a loop or a stray bleed-out camera outside 0.13's window
	std::vector<std::uint32_t> failed;
	for (std::uint32_t seed = 1; seed <= 400; ++seed) {
		std::mt19937 random{ seed };
		const auto   chance = [&](int a_percent) { return static_cast<int>(random() % 100) < a_percent; };
		Game         game;
		auto         now = Start();
		game.body.transitionFrames = 1 + static_cast<int>(random() % 6);
		if (chance(30)) {
			game.body.camera.current = DownRules::Camera::FIRST_PERSON;
		}
		game.Run(now, 1s);
		bool missed = false;
		for (int event = 0; event < 40; ++event) {
			switch (random() % 6) {
			case 0:
			case 1:
				game.body.LethalHit();
				break;
			case 2:
				game.Revived(now);
				break;
			case 3:
				if (!game.body.knockedOver) {
					game.body.KnockStart();
				}
				break;
			case 4:
				if (game.body.knockedOver) {
					game.body.KnockEnd();
				}
				break;
			default:
				if (chance(30)) {
					game.body.StoodUpByGame();
				}
				break;
			}
			const int frames = static_cast<int>(random() % 12);
			for (int i = 0; i < frames; ++i) {
				const bool outside = !game.self.IsDown() && !game.self.Settling();
				missed = missed || (outside && (game.body.camera.Loops() || (game.body.CameraHasBleedOut() && !game.body.knockedOver && !game.body.down)));
				game.Frame(now);
				now += FRAME;
			}
		}
		// It all ends: out of the ragdoll and the fire, helped up.
		game.body.god = true;
		if (game.body.knockedOver) {
			game.body.KnockEnd();
		}
		game.Run(now, 1s);
		game.Revived(now);
		game.Run(now, DownRules::GET_UP_TIME);
		windowMissed += missed ? 1 : 0;
		const bool ok = !game.body.dead && game.self.Now() == Phase::kUp && !game.body.down && game.body.InView() && !game.body.camera.Loops() &&
		                !game.body.CameraHasBleedOut() && !game.body.bleedOutLayer;
		if (!ok) {
			failed.push_back(seed);
		}
	}
	for (std::size_t i = 0; i < failed.size() && i < 10; ++i) {
		std::printf("    camera left wrong with seed %u\n", failed[i]);
	}
	CHECK(failed.empty());
	CHECK(windowMissed > 0);  // the sequences do reach what 0.13 never looked at
}
