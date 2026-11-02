#pragma once

#include <chrono>
#include <optional>

// A player marked as in a synced animation with none running, set right at a game's start.
//
// While the player's ActorState says it is in a synced animation (inSyncAnim, flags2 bit 21), PlayerControls hands no
// input to the player's handlers (Fallout4.exe 1.11.240, PlayerControls::PerformInputProcessing at 140DDBE50, unless
// the free camera is on): the player can't walk, look, shoot or open the Pip-Boy, while menus still work. The
// synchronized-animation manager (BGSSynchronizedAnimationManager) sets the mark when an actor joins one of its
// animations (a kill move, a VATS sync kill, a paired furniture enter) and clears it when the actor leaves.
//
// The ActorState constructor (140CEB250) clears only bits 0-7 and 9-19 of flags2 (`and [rcx+0Ch], 0FFF00100h`, then
// sets bits 8 and 20): bits 21-31 keep whatever the memory held. The PlayerCharacter is built once, at startup, so on
// some runs the player starts with the mark on and no animation behind it (2 of 11 rig starts). Starting a game with
// coc from the main menu or a new game doesn't touch it, and a save made while marked keeps the mark, so that save
// loads stuck too.
//
// So from each game's start (the first game of the run, a new game, every load) until it is settled: once the player
// is in the world with the mark on and nothing that could have set it running for GRACE without a break, the mark is
// cleared, once. Nothing running means the manager has no synced animation at all (anyone's, not just the player's:
// one being set up counts too), and the player is in no scene, no furniture, no dialogue, and is neither dead nor
// down. A real synced animation ends with the manager clearing the mark itself; a mark seen off settles the game.
namespace StuckSyncAnim
{
	using Clock = std::chrono::steady_clock;

	// How long the mark must have been on with nothing running before it is cleared: a synced animation the manager is
	// still setting up, or one just ended whose mark it is still clearing, is never taken for a stuck one.
	constexpr auto GRACE = std::chrono::seconds(1);

	// What the game says, each frame.
	struct Facts
	{
		bool inWorld = false;           // the player is in a cell with its 3D, no loading screen up
		bool marked = false;            // ActorState::inSyncAnim
		bool syncAnimsRunning = false;  // the synchronized-animation manager has any animation, anyone's
		bool inScene = false;           // the player is in a scene
		bool inFurniture = false;       // sitting, sleeping or using furniture, or on the way in or out of it
		bool inDialogue = false;
		bool deadOrDown = false;
	};

	enum class Step
	{
		kNothing,  // not this frame
		kClear,    // clear the mark now (once per game start)
	};

	class Guard
	{
	public:
		// A game starts (a new game, a load): it is looked at again.
		void Arm()
		{
			armed = true;
			since.reset();
		}

		Step Frame(const Facts& a_facts, Clock::time_point a_now)
		{
			if (!armed) {
				return Step::kNothing;
			}
			if (!a_facts.inWorld) {
				since.reset();  // the main menu, a loading screen: the game hasn't started yet
				return Step::kNothing;
			}
			if (!a_facts.marked) {
				armed = false;  // the player is free: settled until the next game starts
				since.reset();
				return Step::kNothing;
			}
			if (a_facts.syncAnimsRunning || a_facts.inScene || a_facts.inFurniture || a_facts.inDialogue || a_facts.deadOrDown) {
				since.reset();  // it may be real: the manager clears its own mark when the animation ends
				return Step::kNothing;
			}
			if (!since) {
				since = a_now;
				return Step::kNothing;
			}
			if (a_now - *since < GRACE) {
				return Step::kNothing;
			}
			armed = false;
			since.reset();
			++cleared;
			return Step::kClear;
		}

		[[nodiscard]] bool Armed() const { return armed; }
		[[nodiscard]] unsigned Cleared() const { return cleared; }

	private:
		bool                             armed = true;  // the run's first game is a game start too
		std::optional<Clock::time_point> since;
		unsigned                         cleared = 0;
	};
}
