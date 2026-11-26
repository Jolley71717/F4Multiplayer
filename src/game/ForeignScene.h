#pragma once

#include <chrono>
#include <cstdint>

// A scene of a shared quest that our player is in, played by NPCs another game runs, is not ours to play. Kept free of the
// game so it can be tested (tests/ForeignSceneTests.cpp).
//
// A shared quest's stages come to every game and run their fragments there, and a scene's own start conditions are true in
// every game the quest has reached: the host finishes a quest NPC's conversation, and a friend's game starts the same scene
// a moment later, with the friend as its player. The NPC is the host's (its copy in the friend's game runs no AI), so the
// scene cannot go on there: the engine holds the friend in it (no weapons, Pip-Boy or workshop), the NPC "tries to talk to
// them" and the friend has to walk up to it and answer a conversation the host has had, which splits the quest's progress.
// Past the 30 s of SceneHold::STARTED_BY_FRIEND the scene is taken for the friend's own, and NpcSync claims its people
// (Dialogue::Holds): the NPC changes hands to the friend's game, runs its scene package there waiting for the friend, and
// the host's quest never gets it where it must go.
//
// So a scene that holds our player, is of a quest the session shares, and is played by NPCs another game runs (and none of
// ours), with no conversation of ours open, is stopped here (the game that runs its NPCs plays it) and its NPCs are not
// claimed. A scene with one of our NPCs in it, or one our player's talk is part of, stays.
namespace ForeignScene
{
	using Clock = std::chrono::steady_clock;

	// How long the facts must hold before the scene is stopped: the owners' states and the scene's actors can lag a moment
	// behind the scene's start, and a conversation our player started is open before its scene shows.
	inline constexpr auto GRACE = std::chrono::milliseconds(1500);
	// A stop that did not take (the scene is still ours) is asked again after this, at most MAX_TRIES times in all.
	inline constexpr auto RETRY = std::chrono::seconds(5);
	inline constexpr int  MAX_TRIES = 3;

	// The scene our player is in, as the game shows it at a look.
	struct Facts
	{
		std::uint32_t scene = 0;            // the scene (0: none)
		bool          talking = false;      // the dialogue menu is open: a conversation of ours (or our talk's scene)
		bool          shared = false;       // its quest is one the session shares
		bool          foreignNpcs = false;  // an NPC in it is run by another game
		bool          ourNpcs = false;      // an NPC in it is run by ours (or by none yet, or is a town scene's of ours)
	};

	// Whether the scene is another game's to play (the facts alone, no wait).
	constexpr bool IsForeign(const Facts& a_facts)
	{
		return a_facts.scene != 0 && !a_facts.talking && a_facts.shared && a_facts.foreignNpcs && !a_facts.ourNpcs;
	}

	enum class Verdict
	{
		kKeep,  // not foreign: ours, or none
		kWait,  // foreign, not yet for GRACE (or a stop waiting for its retry, or given up)
		kStop,  // stop it now
	};

	class Watch
	{
	public:
		Verdict Update(const Facts& a_facts, Clock::time_point a_now)
		{
			if (!IsForeign(a_facts)) {
				scene = 0;
				return Verdict::kKeep;
			}
			if (a_facts.scene != scene) {
				scene = a_facts.scene;
				since = a_now;
				tries = 0;
				lastTry = {};
			}
			if (a_now - since < GRACE) {
				return Verdict::kWait;
			}
			if (tries >= MAX_TRIES || (tries > 0 && a_now - lastTry < RETRY)) {
				return Verdict::kWait;
			}
			++tries;
			lastTry = a_now;
			++stops;
			return Verdict::kStop;
		}

		// The scene being watched as foreign (0: none): its NPCs are not claimed meanwhile.
		[[nodiscard]] std::uint32_t Foreign() const { return scene; }
		// Stops asked for in all.
		[[nodiscard]] std::uint32_t Stops() const { return stops; }
		// Whether the stops asked for the scene being watched ran out without ending it.
		[[nodiscard]] bool GaveUp() const { return scene != 0 && tries >= MAX_TRIES; }

		void Clear() { scene = 0; }

	private:
		std::uint32_t     scene = 0;
		Clock::time_point since{};
		Clock::time_point lastTry{};
		int               tries = 0;
		std::uint32_t     stops = 0;
	};
}
