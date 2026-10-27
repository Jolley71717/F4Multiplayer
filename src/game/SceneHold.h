#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Whether the scene the player is in holds friends' quest stages back. Kept free of the game so it can be
// tested.
//
// A stage set in the middle of a conversation or a scene can break it, so friends' stages wait while the
// player is in one (QuestSync::PlayerBusy). But a friend's stage set here can itself start a scene with our
// player in it: its script runs a moment later and starts the scene as it did in the friend's game.
// Codsworth's talk in Sanctuary (DialogueCodsworthPostWar 50) starts his search of the houses, the scene
// MQ102_CodsworthSearch01, whose actors are Codsworth and the player. That scene is the session's, not the
// player's: the friend is playing it, their game runs Codsworth, and only the friend's later stages end it
// here (Out of Time's 40 sets Codsworth's 100, whose script stops the scene). Held back by the scene, those
// stages never came, the scene never ended, and the host had to follow Codsworth through the search
// themselves to go on; meanwhile the hold kept every other quest's stages waiting too. So a scene that begins
// within STARTED_BY_FRIEND of a friend's stage being set here, or of such a scene ending (one scene can start
// the next), counts as the session's for as long as the player is in it: it holds nothing back. The
// conversation menu holds stages back whoever's scene it is: its topics change with the stages.
//
// The player's own scene holds friends' stages back, but one of a quest with stages waiting here only for
// MAX_HOLD: a stage set from far away can start a scene with the player in it that only a later stage of its
// quest ends (MQ104's 18 starts one, its 30 ends it), while the scene's other actors are nowhere near.
namespace SceneHold
{
	using Clock = std::chrono::steady_clock;

	// A scene that begins this soon after a friend's stage was set here (its script runs a moment later, and
	// some wait a few seconds first), or after the session's last scene ended, is the session's.
	inline constexpr auto STARTED_BY_FRIEND = std::chrono::seconds(10);
	// The player's own scene holds the stages of its quest back at most this long.
	inline constexpr auto MAX_HOLD = std::chrono::seconds(30);

	// The player, as the game shows them at a poll.
	struct Player
	{
		bool          talking = false;            // the dialogue menu is open
		std::uint32_t scene = 0;                  // the scene they are in (0: none)
		bool          sceneQuestWaiting = false;  // stages of that scene's quest are waiting here
	};

	class Watch
	{
	public:
		// A friend's stage was set here: its script may start a scene a moment later.
		void StageSet(Clock::time_point a_now) { lastStage = a_now; }

		// Whether friends' stages wait now.
		bool Holds(const Player& a_player, Clock::time_point a_now)
		{
			if (a_player.scene != scene) {
				const bool wasSessions = sessions;
				if (wasSessions) {
					sessionSceneEnded = a_now;
				}
				scene = a_player.scene;
				sessions = scene != 0 && (Within(lastStage, a_now) || Within(sessionSceneEnded, a_now));
				heldSince.reset();
			}
			if (a_player.talking) {
				return true;
			}
			if (scene == 0 || sessions) {
				return false;
			}
			if (!a_player.sceneQuestWaiting) {
				heldSince.reset();
				return true;
			}
			if (!heldSince) {
				heldSince = a_now;
			}
			return a_now - *heldSince < MAX_HOLD;
		}

		// The scene the player is in, if it is the session's (0: none, or their own).
		[[nodiscard]] std::uint32_t SessionScene() const { return sessions ? scene : 0; }

		// The session ended, or a save was loaded: nothing of it is remembered.
		void Clear()
		{
			scene = 0;
			sessions = false;
			heldSince.reset();
			lastStage.reset();
			sessionSceneEnded.reset();
		}

	private:
		static bool Within(const std::optional<Clock::time_point>& a_at, Clock::time_point a_now)
		{
			return a_at && a_now - *a_at < STARTED_BY_FRIEND;
		}

		std::uint32_t                    scene = 0;        // the scene the player was in at the last poll
		bool                             sessions = false;  // ... which a friend's stage started here
		std::optional<Clock::time_point> heldSince;         // since when the player's own scene holds its quest's stages
		std::optional<Clock::time_point> lastStage;         // when a friend's stage was last set here
		std::optional<Clock::time_point> sessionSceneEnded;  // when the player last left a scene of the session's
	};
}
