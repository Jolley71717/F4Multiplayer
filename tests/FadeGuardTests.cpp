#include "Test.h"

#include "game/FadeGuard.h"

#include <chrono>
#include <vector>

namespace
{
	using FadeGuard::Act;
	using FadeGuard::Clock;
	using namespace std::chrono_literals;

	// Plays a_for in 100 ms frames with the screen as given; the acts other than kNone, in order.
	struct Player
	{
		FadeGuard::Guard  guard;
		Clock::time_point now{ Clock::duration(1h) };

		std::vector<Act> Play(Clock::duration a_for, bool a_faded, bool a_paused = false)
		{
			std::vector<Act> acts;
			const auto       end = now + a_for;
			while (now < end) {
				now += 100ms;
				if (const auto act = guard.Step(now, { .faded = a_faded, .paused = a_paused }); act != Act::kNone) {
					acts.push_back(act);
				}
			}
			return acts;
		}
	};
}

TEST("fade guard: a fade the session caused that nothing fades back in is faded in after 20 s of play")
{
	Player p;
	p.Play(1s, false);
	p.guard.Arm();  // a friend's stage set here
	p.Play(2s, false);
	CHECK(p.Play(19s, true).empty());  // it fades out and stays so
	CHECK(p.guard.Caused());
	CHECK(p.Play(1500ms, true) == std::vector{ Act::kFadeIn });
	CHECK(p.guard.Tries() == 1);
	// The fade-in worked.
	CHECK(p.Play(1s, false) == std::vector{ Act::kCleared });
	CHECK(p.guard.LastCaused());
	CHECK(p.guard.LastTries() == 1);
	CHECK(p.guard.FadedIn() == 1);
}

TEST("fade guard: the game's own short fades after a stage are left alone")
{
	Player p;
	p.guard.Arm();
	p.Play(3s, false);
	CHECK(p.Play(8s, true).empty());  // MS07b's 35 to 37: black for a while, then faded in by the quest itself
	CHECK(p.Play(1s, false) == std::vector{ Act::kCleared });
	CHECK(p.guard.LastTries() == 0);
	CHECK(p.guard.FadedIn() == 0);
}

TEST("fade guard: a fade long after the session did anything is only logged, once")
{
	Player p;
	p.guard.Arm();
	p.Play(16s, false);  // the arm is over
	CHECK(!p.guard.Armed());
	CHECK(p.Play(29s, true).empty());
	CHECK(!p.guard.Caused());
	CHECK(p.Play(2s, true) == std::vector{ Act::kLongFade });
	CHECK(p.Play(60s, true).empty());  // never faded in, never logged again
	CHECK(p.guard.FadedIn() == 0);
	CHECK(p.guard.LongFades() == 1);
}

TEST("fade guard: a fade already up when the session acts is the session's")
{
	Player p;
	p.Play(30s, false);
	p.Play(5s, true);  // the game's own fade
	p.guard.Arm();     // a teleport or a stage while it is up
	CHECK(p.guard.Caused());
	CHECK(p.Play(16s, true) == std::vector{ Act::kFadeIn });  // 20 s of play in all
}

TEST("fade guard: a loading screen, a pausing menu or the opening doesn't count as play, for the fade or the arm")
{
	Player p;
	p.guard.Arm();
	p.Play(5min, false, true);  // the teleport's loading screen: the arm doesn't run down
	CHECK(p.guard.Armed());
	p.Play(1s, true, true);  // the fade comes up still loading
	CHECK(p.guard.Caused());
	CHECK(p.Play(10min, true, true).empty());  // the vault's lift, the pause menu: no play
	CHECK(p.guard.Up() == Clock::duration::zero());
	CHECK(p.Play(20500ms, true) == std::vector{ Act::kFadeIn });
}

TEST("fade guard: a fade-in that doesn't take is tried once more, then left")
{
	Player p;
	p.guard.Arm();
	CHECK(p.Play(20500ms, true) == std::vector{ Act::kFadeIn });
	CHECK(p.Play(20s, true) == std::vector{ Act::kFadeIn });
	CHECK(p.Play(5min, true).empty());
	CHECK(p.guard.Tries() == FadeGuard::MAX_TRIES);
}

TEST("fade guard: a hitch or a hang counts as one short frame, not as play")
{
	Player p;
	p.guard.Arm();
	p.guard.Step(p.now, { .faded = true });
	p.now += 10min;  // the game hung, or a debugger held it
	CHECK(p.guard.Step(p.now, { .faded = true }) == Act::kNone);
	CHECK(p.guard.Up() <= FadeGuard::MAX_STEP);
}

TEST("fade guard: each fade is judged afresh")
{
	Player p;
	p.guard.Arm();
	CHECK(p.Play(20500ms, true) == std::vector{ Act::kFadeIn });
	CHECK(p.Play(1s, false) == std::vector{ Act::kCleared });
	p.Play(20s, false);
	CHECK(!p.guard.Caused());
	CHECK(p.Play(25s, true).empty());  // not the session's: no fade-in
	CHECK(p.guard.Tries() == 0);
}
