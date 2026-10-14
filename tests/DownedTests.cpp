// Downed and revive (src/game/DownRules.h, which Downed.cpp runs every frame): the player's own down state
// driven across whole sequences against a model of the game's side of it, and two players helping each other
// up through the real server, their status and Revive messages encoded and decoded as the plugin sends them.

#include "Test.h"
#include "sim/Sim.h"

#include "game/DownRules.h"

#include <algorithm>
#include <array>
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
		int               downNotices = 0;
		int               kills = 0;
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
			const DownRules::Facts facts{ .enabled = withFriends, .essential = body.essential, .down = body.down, .dead = DownRules::Dead(lifeState, body.dead || body.down), .paused = paused };
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
			if (step.trimHealth) {
				body.fullHealth = false;
				++body.trims;
			}
			downNotices += step.wentDown;
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
