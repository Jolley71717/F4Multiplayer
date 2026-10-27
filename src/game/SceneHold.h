#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

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
// themselves to go on; meanwhile the hold kept every other quest's stages waiting too. So a scene of a quest
// a friend's stage was set on here within STARTED_BY_FRIEND, or of the quest of the session's scene that
// ended within STARTED_BY_FRIEND (one scene can start the next), counts as the session's for as long as the
// player is in it: it holds nothing back. The conversation menu holds stages back whoever's scene it is: its
// topics change with the stages.
//
// Of its quest, not any: the player talking to someone right after a friend's progress came starts a scene
// of their own, of that NPC's quest. Fallout4.esm's and DLCCoast.esm's shared quests' stage fragments start
// 502 scenes of their own quest and 18 of another's (Inst306 200 starts Inst307's, the Memory Den's memories
// a holotape quest's): those 18 are the player's own here, held back as before. A scene the player is first
// seen in with the dialogue menu open is theirs too: they started it by talking (Codsworth's search has no
// line for the player: his packages and his own lines only).
//
// The player's own scene holds friends' stages back, but one of a quest with stages waiting here only for
// MAX_HOLD: a stage set from far away can start a scene with the player in it that only a later stage of its
// quest ends (MQ104's 18 starts one, its 30 ends it), while the scene's other actors are nowhere near.
namespace SceneHold
{
	using Clock = std::chrono::steady_clock;

	// A scene that begins this soon after a friend's stage of its quest was set here, or after the session's last scene
	// of its quest ended, is the session's. Its script runs a moment later, some wait a few seconds first, and a
	// game with hundreds of mods can be many seconds behind on its scripts; the quest and the dialogue menu, not
	// the time, are what keep the player's own scenes theirs.
	inline constexpr auto STARTED_BY_FRIEND = std::chrono::seconds(30);
	// The player's own scene holds the stages of its quest back at most this long.
	inline constexpr auto MAX_HOLD = std::chrono::seconds(30);

	// The player, as the game shows them at a poll.
	struct Player
	{
		bool          talking = false;            // the dialogue menu is open
		std::uint32_t scene = 0;                  // the scene they are in (0: none)
		std::uint32_t sceneQuest = 0;             // the quest it belongs to
		bool          sceneQuestWaiting = false;  // stages of that scene's quest are waiting here
	};

	class Watch
	{
	public:
		// A friend's stage of a_quest was set here: its script may start a scene of that quest a moment later.
		void StageSet(std::uint32_t a_quest, Clock::time_point a_now)
		{
			Forget(a_now);
			if (const auto it = std::ranges::find(recent, a_quest, &Stage::first); it != recent.end()) {
				it->second = a_now;
			} else {
				recent.emplace_back(a_quest, a_now);
			}
		}

		// Whether friends' stages wait now.
		bool Holds(const Player& a_player, Clock::time_point a_now)
		{
			if (a_player.scene != scene) {
				if (sessions) {
					ended = { sceneQuest, a_now };
				}
				Forget(a_now);
				scene = a_player.scene;
				sceneQuest = a_player.sceneQuest;
				sessions = scene != 0 && sceneQuest != 0 && !a_player.talking &&
				           (std::ranges::contains(recent, sceneQuest, &Stage::first) || (ended && ended->first == sceneQuest));
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

		// The session ended (a dropped connection too: it is made again a moment later), or a save was loaded:
		// what was set before is forgotten, so a scene begun after it is the player's own. The scene they are
		// still in stays what it was: the session's search goes on in their game, and only the friend's stages
		// end it, the ones that come again once the session is back.
		void Clear()
		{
			recent.clear();
			ended.reset();
			heldSince.reset();
		}

	private:
		using Stage = std::pair<std::uint32_t, Clock::time_point>;  // a quest, and when a stage of it was set or its scene ended

		void Forget(Clock::time_point a_now)
		{
			std::erase_if(recent, [&](const Stage& a_stage) { return a_now - a_stage.second >= STARTED_BY_FRIEND; });
			if (ended && a_now - ended->second >= STARTED_BY_FRIEND) {
				ended.reset();
			}
		}

		std::uint32_t                    scene = 0;       // the scene the player was in at the last poll
		std::uint32_t                    sceneQuest = 0;  // ... its quest
		bool                             sessions = false;  // ... which a friend's stage started here
		std::optional<Clock::time_point> heldSince;         // since when the player's own scene holds its quest's stages
		std::vector<Stage>               recent;            // the quests friends' stages were set on lately
		std::optional<Stage>             ended;             // the session's scene the player last left, by its quest
	};
}
