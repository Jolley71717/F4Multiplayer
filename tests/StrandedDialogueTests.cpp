#include "Test.h"

#include "game/StrandedDialogue.h"

using namespace std::chrono_literals;

namespace
{
	constexpr std::uint32_t STAND_IN = 0xFF000800;  // a friend's stand-in, a reference the plugin made
	constexpr std::uint32_t CODSWORTH = 0x0001CA7D;
	constexpr std::uint32_t QUEST_SCENE = 0x0002A0B1;

	// A conversation open with its speaker gone (disabled, or deleted and no longer resolving), the
	// dialogue menu down, the player in no scene.
	StrandedDialogue::Facts Stranded(std::uint32_t a_speaker)
	{
		return { .open = true, .speaker = a_speaker, .speakerGone = true };
	}
}

TEST("stranded dialogue: a conversation that is fine is never ended")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(false, start));
	CHECK(!timer.Due(false, start + 10s));
	CHECK(timer.Ends() == 0);
}

TEST("stranded dialogue: ended once it has been without its speaker for the grace time")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(!timer.Due(true, start + 1900ms));
	CHECK(timer.Due(true, start + StrandedDialogue::GRACE));
	CHECK(timer.Ends() == 1);
}

TEST("stranded dialogue: a speaker that comes back in time starts the wait over")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(!timer.Due(false, start + 1s));
	CHECK(!timer.Due(true, start + 1500ms));
	CHECK(!timer.Due(true, start + 3s));
	CHECK(timer.Due(true, start + 3500ms));
}

TEST("stranded dialogue: an end that didn't take is tried again after another grace time")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(timer.Due(true, start + 2s));
	CHECK(!timer.Due(true, start + 2s + 100ms));
	CHECK(!timer.Due(true, start + 3900ms));
	CHECK(timer.Due(true, start + 4s));
	CHECK(timer.Ends() == 2);
	CHECK(!timer.Due(false, start + 5s));
	CHECK(timer.Ends() == 0);
}

TEST("stranded dialogue: a loading screen starts the wait over")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	timer.Reset();  // loading for longer than the grace time
	CHECK(!timer.Due(true, start + 30s));
	CHECK(!timer.Due(true, start + 31s));
	CHECK(timer.Due(true, start + 32s));
}

TEST("stranded dialogue: a conversation whose speaker the plugin took out is ended after the grace time")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	CHECK(!watch.Due(Stranded(STAND_IN), start + 10ms));  // disabled: it still resolves
	CHECK(watch.Ours());
	CHECK(!watch.Due(Stranded(0), start + 1s));  // deleted a moment later: its handle no longer resolves
	CHECK(watch.Speaker() == STAND_IN);
	CHECK(watch.Due(Stranded(0), start + 10ms + StrandedDialogue::GRACE));
	CHECK(watch.Ends() == 1);
}

TEST("stranded dialogue: a conversation the game left open is the game's, however it went")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	// Codsworth's, with its speaker lost (the vanilla bug): nobody of ours was in it.
	CHECK(!watch.Due(Stranded(CODSWORTH), start));
	CHECK(!watch.Due(Stranded(CODSWORTH), start + 10s));
	CHECK(!watch.Ours());
	// A speaker that never resolved at all.
	CHECK(!watch.Due(Stranded(0), start + 20s));
	CHECK(!watch.Due(Stranded(0), start + 30s));
	// A stand-in's that the game took out itself (its cell unloaded), not the plugin.
	CHECK(!watch.Due(Stranded(STAND_IN), start + 40s));
	CHECK(!watch.Due(Stranded(STAND_IN), start + 50s));
	CHECK(watch.Ends() == 0);
}

TEST("stranded dialogue: a quest's scene is never ended, unless the speaker the plugin took out was in it")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);  // it was in no scene as it went
	auto facts = Stranded(STAND_IN);
	facts.scene = QUEST_SCENE;
	CHECK(!watch.Due(facts, start + 10ms));
	CHECK(!watch.Due(facts, start + 10s));
	CHECK(!watch.Ours());

	StrandedDialogue::Watch inScene;
	inScene.Removed(STAND_IN, QUEST_SCENE, start);  // it went in the middle of that very scene
	CHECK(!inScene.Due(facts, start + 10ms));
	CHECK(inScene.Ours());
	CHECK(inScene.Due(facts, start + 10ms + StrandedDialogue::GRACE));

	StrandedDialogue::Watch another;
	another.Removed(STAND_IN, QUEST_SCENE, start);
	facts.scene = QUEST_SCENE + 1;  // the player went on into another scene of the quest
	CHECK(!another.Due(facts, start + 10ms));
	CHECK(!another.Due(facts, start + 10s));
}

TEST("stranded dialogue: a speaker who is still there, or being talked to, is no stranding")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	auto there = Stranded(STAND_IN);
	there.speakerGone = false;
	CHECK(!watch.Due(there, start + 10ms));
	CHECK(!watch.Due(there, start + 10s));
	auto talking = Stranded(STAND_IN);
	talking.talking = true;
	CHECK(!watch.Due(talking, start + 20s));
	CHECK(!watch.Due(talking, start + 30s));
	auto closed = Stranded(STAND_IN);
	closed.open = false;
	CHECK(!watch.Due(closed, start + 40s));
	CHECK(!watch.Due(closed, start + 50s));
	// Stranded at last: the wait starts only now.
	CHECK(!watch.Due(Stranded(STAND_IN), start + 60s));
	CHECK(!watch.Due(Stranded(STAND_IN), start + 61900ms));
	CHECK(watch.Due(Stranded(STAND_IN), start + 62s));
}

TEST("stranded dialogue: another speaker, not the plugin's, takes the conversation over")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	CHECK(!watch.Due(Stranded(STAND_IN), start + 10ms));
	CHECK(!watch.Due(Stranded(CODSWORTH), start + 1s));
	CHECK(!watch.Ours());
	CHECK(!watch.Due(Stranded(0), start + 3s));  // and stays its speaker while none resolves
	CHECK(!watch.Due(Stranded(0), start + 10s));
	CHECK(watch.Speaker() == CODSWORTH);
}

TEST("stranded dialogue: a loading screen is no part of the wait")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	CHECK(!watch.Due(Stranded(STAND_IN), start + 10ms));
	auto loading = Stranded(0);
	loading.loading = true;
	CHECK(!watch.Due(loading, start + 1s));
	CHECK(!watch.Due(loading, start + 30s));
	CHECK(!watch.Due(Stranded(0), start + 31s));  // the screen is gone: the wait starts over
	CHECK(!watch.Due(Stranded(0), start + 32900ms));
	CHECK(watch.Due(Stranded(0), start + 33s));
}

TEST("stranded dialogue: out of dialogue nothing is waited for, but what the plugin took out stays known")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	CHECK(!watch.Due(Stranded(STAND_IN), start + 10ms));
	watch.Reset();  // the engine said the player was out of dialogue for a frame
	CHECK(watch.Speaker() == 0);
	CHECK(!watch.Ours());
	CHECK(!watch.Due(Stranded(STAND_IN), start + 1s));
	CHECK(watch.Ours());
	CHECK(!watch.Due(Stranded(STAND_IN), start + 2900ms));
	CHECK(watch.Due(Stranded(STAND_IN), start + 3s));
}

TEST("stranded dialogue: what the plugin took out is forgotten after a while")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	const auto later = start + StrandedDialogue::REMEMBER;
	CHECK(!watch.Due(Stranded(STAND_IN), later));  // its form ID may be something else's by now
	CHECK(!watch.Due(Stranded(STAND_IN), later + 10s));
	CHECK(!watch.Ours());
	// Taken out again (a new stand-in with the same form ID): known afresh.
	watch.Removed(STAND_IN, 0, later + 20s);
	CHECK(!watch.Due(Stranded(STAND_IN), later + 20s));
	CHECK(watch.Ours());
	CHECK(watch.Due(Stranded(STAND_IN), later + 22s));
}

TEST("stranded dialogue: the end that didn't take is tried again")
{
	StrandedDialogue::Watch watch;
	const auto              start = StrandedDialogue::Clock::now();
	watch.Removed(STAND_IN, 0, start);
	CHECK(!watch.Due(Stranded(STAND_IN), start));
	CHECK(watch.Due(Stranded(STAND_IN), start + 2s));
	CHECK(!watch.Due(Stranded(STAND_IN), start + 3s));
	CHECK(watch.Due(Stranded(STAND_IN), start + 4s));
	CHECK(watch.Ends() == 2);
	auto closed = Stranded(STAND_IN);
	closed.open = false;
	CHECK(!watch.Due(closed, start + 5s));
	CHECK(watch.Ends() == 0);
}
