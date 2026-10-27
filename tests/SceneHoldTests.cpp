#include "Test.h"

#include "game/SceneHold.h"

#include <chrono>

namespace
{
	using namespace std::chrono_literals;
	using Clock = SceneHold::Clock;

	constexpr std::uint32_t CODSWORTH = 0x0001E64E;   // DialogueCodsworthPostWar
	constexpr std::uint32_t SEARCH = 0x0019E137;      // MQ102_CodsworthSearch01, of his quest: Codsworth and the player
	constexpr std::uint32_t SEARCH2 = 0x0019E146;     // MQ102_CodsworthSearch02, of his quest too
	constexpr std::uint32_t TALK = 0x0001F1A5;        // a conversation with Codsworth, of his quest
	constexpr std::uint32_t MQ102 = 0x0001CC2A;       // Out of Time
	constexpr std::uint32_t FORCEGREET = 0x000A183C;  // a scene of the player's own
	constexpr std::uint32_t OWN_QUEST = 0x000229E5;   // ... of another quest

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	SceneHold::Player Out() { return {}; }
	SceneHold::Player In(std::uint32_t a_scene, std::uint32_t a_quest, bool a_questWaiting = false)
	{
		return { .scene = a_scene, .sceneQuest = a_quest, .sceneQuestWaiting = a_questWaiting };
	}
	SceneHold::Player Search(bool a_questWaiting = false) { return In(SEARCH, CODSWORTH, a_questWaiting); }
	SceneHold::Player Own(bool a_questWaiting = false) { return In(FORCEGREET, OWN_QUEST, a_questWaiting); }
	SceneHold::Player Talking(std::uint32_t a_scene = 0, std::uint32_t a_quest = 0) { return { .talking = true, .scene = a_scene, .sceneQuest = a_quest }; }
}

TEST("scene hold: out of any scene nothing holds; the conversation menu does")
{
	SceneHold::Watch watch;
	CHECK(!watch.Holds(Out(), T0));
	CHECK(watch.Holds(Talking(), T0));
	CHECK(watch.Holds(Talking(FORCEGREET, OWN_QUEST), T0 + 1s));
	CHECK(!watch.Holds(Out(), T0 + 2s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: the player's own scene holds friends' stages back until they are out of it")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(Own(), T0));
	CHECK(watch.Holds(Own(), T0 + 5min));  // however long it lasts
	CHECK(watch.SessionScene() == 0);
	CHECK(!watch.Holds(Out(), T0 + 6min));
}

TEST("scene hold: the player's own scene of a quest with stages waiting holds them for 30 s, then again once nothing of that quest waits")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(Own(true), T0));
	CHECK(watch.Holds(Own(true), T0 + SceneHold::MAX_HOLD - 1s));
	CHECK(!watch.Holds(Own(true), T0 + SceneHold::MAX_HOLD));
	CHECK(!watch.Holds(Own(true), T0 + 2min));
	// Its stages set, nothing of the quest waits: the scene is the player's own again, and the next of its
	// stages to come waits its 30 s.
	CHECK(watch.Holds(Own(false), T0 + 3min));
	CHECK(watch.Holds(Own(true), T0 + 3min + 1s));
	CHECK(!watch.Holds(Own(true), T0 + 3min + 1s + SceneHold::MAX_HOLD));
}

TEST("scene hold: a scene of a quest a friend's stage was set on here within 30 s is the session's: it holds nothing, however long it runs")
{
	SceneHold::Watch watch;
	CHECK(!watch.Holds(Out(), T0));
	watch.StageSet(CODSWORTH, T0);  // Codsworth's 50: its script starts his search a moment later
	CHECK(!watch.Holds(Out(), T0 + 500ms));
	CHECK(!watch.Holds(Search(), T0 + 1s));
	CHECK(watch.SessionScene() == SEARCH);
	// Out of Time's 32 to 40 are set during it, and Codsworth's stages don't wait (sceneQuestWaiting false).
	CHECK(!watch.Holds(Search(false), T0 + 2min));
	CHECK(!watch.Holds(Search(true), T0 + 5min));
	CHECK(watch.SessionScene() == SEARCH);
	// Codsworth's 100 stops it.
	CHECK(!watch.Holds(Out(), T0 + 6min));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: a scene that begins 30 s or more after the last friend's stage of its quest is the player's own")
{
	SceneHold::Watch watch;
	watch.StageSet(CODSWORTH, T0);
	CHECK(!watch.Holds(Out(), T0 + 5s));
	CHECK(watch.Holds(Search(), T0 + SceneHold::STARTED_BY_FRIEND));
	CHECK(watch.SessionScene() == 0);
	CHECK(watch.Holds(Search(), T0 + 1min));
}

TEST("scene hold: a scene the player walks into right after friends' stages of other quests came is their own: it holds friends' stages back")
{
	SceneHold::Watch watch;
	watch.StageSet(MQ102, T0);  // a friend's progress
	watch.StageSet(CODSWORTH, T0 + 1s);
	CHECK(watch.Holds(Own(), T0 + 2s));  // a forcegreet, an NPC's walk with the player, of another quest
	CHECK(watch.SessionScene() == 0);
	CHECK(watch.Holds(Own(), T0 + 1min));
}

TEST("scene hold: a scene first seen with the dialogue menu open is the player's own, even of a quest a friend's stage was just set on")
{
	SceneHold::Watch watch;
	watch.StageSet(CODSWORTH, T0);
	CHECK(watch.Holds(Talking(TALK, CODSWORTH), T0 + 2s));  // the player talks to Codsworth themselves
	CHECK(watch.SessionScene() == 0);
	// Between the conversation's lines the menu closes: the scene is still theirs, and holds.
	CHECK(watch.Holds(In(TALK, CODSWORTH), T0 + 5s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: the conversation menu holds stages back in the session's scene too, and the scene holds nothing again once it closes")
{
	SceneHold::Watch watch;
	watch.StageSet(CODSWORTH, T0);
	CHECK(!watch.Holds(Search(), T0 + 1s));
	CHECK(watch.Holds(Talking(SEARCH, CODSWORTH), T0 + 10s));  // a line for the player
	CHECK(watch.SessionScene() == SEARCH);
	CHECK(!watch.Holds(Search(), T0 + 20s));
}

TEST("scene hold: a scene of its quest that begins within 30 s of the session's scene ending is the session's too (one scene starts the next); another quest's is not")
{
	SceneHold::Watch watch;
	watch.StageSet(CODSWORTH, T0);
	CHECK(!watch.Holds(Search(), T0 + 1s));
	CHECK(!watch.Holds(Out(), T0 + 2min));  // the first search scene ends
	CHECK(!watch.Holds(In(SEARCH2, CODSWORTH), T0 + 2min + 3s));
	CHECK(watch.SessionScene() == SEARCH2);
	CHECK(!watch.Holds(In(SEARCH2, CODSWORTH), T0 + 5min));
	// Straight from one into the next, with no poll between.
	CHECK(!watch.Holds(Search(), T0 + 5min + 500ms));
	CHECK(watch.SessionScene() == SEARCH);
	// It ends, and the player walks into a scene of another quest at once: their own.
	CHECK(!watch.Holds(Out(), T0 + 6min));
	CHECK(watch.Holds(Own(), T0 + 6min + 2s));
	CHECK(watch.SessionScene() == 0);
	CHECK(!watch.Holds(Out(), T0 + 6min + 30s));
	// Long after it, one of Codsworth's quest: the player's own.
	CHECK(watch.Holds(Search(), T0 + 7min));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: a scene the player was already in when a friend's stage of its quest was set stays their own")
{
	SceneHold::Watch watch;
	CHECK(watch.Holds(Own(), T0));
	watch.StageSet(OWN_QUEST, T0 + 1s);  // set anyway after 30 s, as one of its quest (the hold is 30 s for those)
	CHECK(watch.Holds(Own(), T0 + 2s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: cleared (the session ended or was made again, a save loaded), the session's scene the player is still in stays the session's; what was set before is forgotten")
{
	SceneHold::Watch watch;
	watch.StageSet(CODSWORTH, T0);
	CHECK(!watch.Holds(Search(), T0 + 1s));
	watch.Clear();  // the connection dropped mid-search
	CHECK(watch.SessionScene() == SEARCH);
	CHECK(!watch.Holds(Search(), T0 + 30s));  // back: the friend's stages still go
	CHECK(watch.SessionScene() == SEARCH);
	// It ends; neither a stage set before a drop nor the scene that ended before it makes a scene begun after it the
	// session's.
	CHECK(!watch.Holds(Out(), T0 + 1min));
	watch.StageSet(CODSWORTH, T0 + 1min);
	watch.Clear();
	CHECK(watch.Holds(In(SEARCH2, CODSWORTH), T0 + 1min + 2s));
	CHECK(watch.SessionScene() == 0);
}

TEST("scene hold: friends' stages of many quests in a catch-up are each remembered only 30 s")
{
	SceneHold::Watch watch;
	for (std::uint32_t i = 0; i < 1000; ++i) {
		watch.StageSet(0x00100000 + i, T0 + std::chrono::milliseconds(i * 125));  // 8 a second for 125 s
	}
	const auto last = T0 + std::chrono::milliseconds(999 * 125);
	CHECK(watch.Holds(In(SEARCH, 0x00100000), last + 1s));  // its quest's stage set two minutes ago: the player's own
	CHECK(!watch.Holds(Out(), last + 2s));
	CHECK(!watch.Holds(In(SEARCH, 0x00100000 + 999), last + 3s));  // set 3 s ago
	CHECK(watch.SessionScene() == SEARCH);
}
