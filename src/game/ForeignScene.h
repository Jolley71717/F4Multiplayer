#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

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
// The scene can also open the dialogue menu in our game by itself, with no key pressed (the Cabot House's scene of the
// intercom, Jack and Edward, 2026-11-27): that menu is the other game's conversation, and if its speaker were taken for ours
// (Dialogue::Holds, NpcSync's talk partner) the NPCs would change hands and the quest's progress split.
//
// So a scene that holds our player, is of a quest the session shares, and is played by NPCs another game runs (and none of
// ours), with no conversation of ours open, is stopped here (the game that runs its NPCs plays it), the menu it opened is
// closed, and its NPCs are not claimed. A conversation of ours is one our player started: the dialogue menu opened with
// our player's activation of its speaker just before (Talk). A scene with one of our NPCs in it, or one our player's talk
// is part of, stays.
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
		bool          talking = false;      // the dialogue menu is open and our player started it (Talk): a conversation of ours
		bool          menu = false;         // the dialogue menu is open, whoever started it
		bool          shared = false;       // its quest is one the session shares
		bool          foreignNpcs = false;  // an NPC in it is run by another game
		bool          ourNpcs = false;      // an NPC in it is run by ours (or by none yet, or is a town scene's of ours)
	};

	// How long an activation of an NPC counts as what opened its dialogue menu.
	inline constexpr auto ACTIVATION = std::chrono::seconds(5);

	// Whether the open dialogue menu was started by our player: its speaker was activated by them just before it opened (or
	// the activation is seen a look later). Kept until it closes.
	class Talk
	{
	public:
		// a_menu: the dialogue menu is open. a_activated: our player activated its speaker within ACTIVATION of now.
		// The activation reaches Dialogue through WorldSync's inbox, drained in WorldSync's own frame, so it can arrive a look
		// after the menu opened: an activation of the speaker seen while the menu is open makes it ours (an E on that NPC is
		// our player's talk either way; the stop waits GRACE, so a late one is in before it). Ours is never taken back while
		// the menu stays open.
		bool Update(bool a_menu, bool a_activated)
		{
			if (!a_menu) {
				ours = false;
				return false;
			}
			ours = ours || a_activated;
			return ours;
		}

	private:
		bool ours = false;
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

	// Stopping a scene ends it, and its end (or a phase's end) fragment can set a stage of its quest. That stage is not this
	// game's progress: the game that plays the scene reports its own, when its scene gets there, and a stage reported from
	// here would move the quest on while the scene still plays there. So for HOLD_BACK after a stop, a stage of the scene's
	// quest set here is not reported, unless the other game's report of the quest came after the stop (the stage was theirs).
	inline constexpr auto HOLD_BACK = std::chrono::seconds(10);

	class Guard
	{
	public:
		// A scene of a_quest was stopped at a_now.
		void Stopped(std::uint32_t a_quest, Clock::time_point a_now) { stops.insert_or_assign(a_quest, a_now); }

		// Whether a stage of a_quest set here is held back from the report at a_now. a_remote: when a stage of that quest last
		// came from another game (none: never).
		[[nodiscard]] bool Holds(std::uint32_t a_quest, Clock::time_point a_now, std::optional<Clock::time_point> a_remote = {}) const
		{
			const auto it = stops.find(a_quest);
			if (it == stops.end() || a_now - it->second >= HOLD_BACK) {
				return false;
			}
			return !a_remote || *a_remote < it->second;
		}

		void Clear() { stops.clear(); }

	private:
		std::unordered_map<std::uint32_t, Clock::time_point> stops;
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
