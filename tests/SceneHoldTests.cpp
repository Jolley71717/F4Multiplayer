#include "Test.h"

#include "game/SceneHold.h"

#include <chrono>

namespace
{
	using namespace std::chrono_literals;
	using Clock = SceneHold::Clock;

	constexpr std::uint32_t SEARCH = 0x0019E137;    // MQ102_CodsworthSearch01: Codsworth and the player
	constexpr std::uint32_t SEARCH2 = 0x0019E146;   // MQ102_CodsworthSearch02
	constexpr std::uint32_t FORCEGREET = 0x000A183C;  // a scene of the player's own

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	SceneHold::Player Out() { return {}; }
	SceneHold::Player In(std::uint32_t a_scene, bool a_questWaiting = false) { return { .scene = a_scene, .sceneQuestWaiting = a_questWaiting }; }
	SceneHold::Player Talking(std::uint32_t a_scene = 0) { return { .talking = true, .scene = a_scene }; }
}

TEST("scene hold: out of any scene nothing holds; the conversation menu does")
{
	SceneHold::Watch watch;
	CHECK(!watch.Holds(Out(), T0));
	CHECK(watch.Holds(Talking(), T0));
	CHECK(watch.Holds(Talking(FORCEGREET), T0 + 1s));
	CHECK(!watch.Holds(Out(), T0 + 2s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: the player's own scene holds friends' stages back until they are out of it")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(In(FORCEGREET), T0));
	CHECK(watch.Holds(In(FORCEGREET), T0 + 5min));  // however long it lasts
	CHECK(watch.SessionScene() == 0);
	CHECK(!watch.Holds(Out(), T0 + 6min));
}

TEST("scene hold: the player's own scene of a quest with stages waiting holds them for 30 s, then again once nothing of that quest waits")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(In(FORCEGREET, true), T0));
	CHECK(watch.Holds(In(FORCEGREET, true), T0 + SceneHold::MAX_HOLD - 1s));
	CHECK(!watch.Holds(In(FORCEGREET, true), T0 + SceneHold::MAX_HOLD));
	CHECK(!watch.Holds(In(FORCEGREET, true), T0 + 2min));
	// Its stages set, nothing of the quest waits: the scene is the player's own again, and the next of its
	// stages to come waits its 30 s.
	CHECK(watch.Holds(In(FORCEGREET, false), T0 + 3min));
	CHECK(watch.Holds(In(FORCEGREET, true), T0 + 3min + 1s));
	CHECK(!watch.Holds(In(FORCEGREET, true), T0 + 3min + 1s + SceneHold::MAX_HOLD));
}

TEST("scene hold: a scene that begins within 10 s of a friend's stage being set here is the session's: it holds nothing, however long it runs")
{
	SceneHold::Watch watch;
	CHECK(!watch.Holds(Out(), T0));
	watch.StageSet(T0);  // Codsworth's 50: its script starts his search a moment later
	CHECK(!watch.Holds(Out(), T0 + 500ms));
	CHECK(!watch.Holds(In(SEARCH), T0 + 1s));
	CHECK(watch.SessionScene() == SEARCH);
	// Out of Time's 32 to 40 are set during it, and Codsworth's stages don't wait (sceneQuestWaiting false).
	CHECK(!watch.Holds(In(SEARCH, false), T0 + 2min));
	CHECK(!watch.Holds(In(SEARCH, true), T0 + 5min));
	CHECK(watch.SessionScene() == SEARCH);
	// Codsworth's 100 stops it.
	CHECK(!watch.Holds(Out(), T0 + 6min));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: a scene that begins 10 s or more after the last friend's stage is the player's own")
{
	SceneHold::Watch watch;
	watch.StageSet(T0);
	CHECK(!watch.Holds(Out(), T0 + 5s));
	CHECK(watch.Holds(In(FORCEGREET), T0 + SceneHold::STARTED_BY_FRIEND));
	CHECK(watch.SessionScene() == 0);
	CHECK(watch.Holds(In(FORCEGREET), T0 + 1min));
}

TEST("scene hold: the conversation menu holds stages back in the session's scene too, and the scene holds nothing again once it closes")
{
	SceneHold::Watch watch;
	watch.StageSet(T0);
	CHECK(!watch.Holds(In(SEARCH), T0 + 1s));
	CHECK(watch.Holds(Talking(SEARCH), T0 + 10s));  // Codsworth has a line for the player
	CHECK(watch.SessionScene() == SEARCH);
	CHECK(!watch.Holds(In(SEARCH), T0 + 20s));
}

TEST("scene hold: a scene that begins within 10 s of the session's scene ending is the session's too (one scene starts the next)")
{
	SceneHold::Watch watch;
	watch.StageSet(T0);
	CHECK(!watch.Holds(In(SEARCH), T0 + 1s));
	CHECK(!watch.Holds(Out(), T0 + 2min));  // the first search scene ends
	CHECK(!watch.Holds(In(SEARCH2), T0 + 2min + 3s));
	CHECK(watch.SessionScene() == SEARCH2);
	CHECK(!watch.Holds(In(SEARCH2), T0 + 5min));
	// Straight from one into the next, with no poll between.
	CHECK(!watch.Holds(In(SEARCH), T0 + 5min + 500ms));
	CHECK(watch.SessionScene() == SEARCH);
	CHECK(!watch.Holds(Out(), T0 + 6min));
	// Long after it, a scene of the player's own.
	CHECK(watch.Holds(In(FORCEGREET), T0 + 7min));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: a scene the player was already in when a friend's stage was set stays their own")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(In(FORCEGREET), T0));
	watch.StageSet(T0 + 1s);  // a stage of another quest, set anyway (the hold is 30 s for the scene's quest's)
	CHECK(watch.Holds(In(FORCEGREET), T0 + 2s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: cleared, nothing is remembered: the scene the player is in is their own")
{
	SceneHold::Watch watch;
	watch.StageSet(T0);
	CHECK(!watch.Holds(In(SEARCH), T0 + 1s));
	watch.Clear();  // the session ended
	CHECK(watch.SessionScene() == 0);
	CHECK(watch.Holds(In(SEARCH), T0 + 2s));
	CHECK(watch.SessionScene() == 0);
}
