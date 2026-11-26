#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// A Port-A-Diner's pie (PortaDinerScript, on the diner's activator).
//
// A press runs the script's OnActivate in its state Waiting: it goes to Busy, and unless the pie was won before
// (PieSuccess) it counts the press (Attempts) and rolls Utility.RandomInt(1, RandomMax) against
// (1 + fSuccessMod) * the game's own player's Luck + Attempts / 2. A win, or ForcePieSuccess, plays "Succeed", puts
// the pie in the diner's container and sets PieSuccess; a loss plays "Fail01"; a diner won before plays "Succeed"
// again. Then back to Waiting. A press while Busy does nothing.
//
// A friend's press was replayed here by their stand-in (WorldSync::ApplyRemoteActivation), so this game rolled its own
// dice for it, with its own player's Luck: the claw dropped the pie in one game and handed it over in the other, and
// the winner's friend saw a shut diner, or got a pie the presser never won.
//
// Now the presser's dice are the only ones:
// - A friend's press at a diner not won here is still replayed by the stand-in through the script (its Busy state, its
//   animation and sound, its count), with dice that cannot win: Attempts is put LEVER below what it was for the frames
//   the script takes to read it (Lever), and put back, counted, as soon as the script has (Restored). The value says
//   of itself that it is a lever, so one that a save caught is put back when the diner is next seen. None should be:
//   every lever comes off before a save is written and when our game stops (a menu: its scripts stand still, and no
//   press is made here meanwhile), so a save loaded without the mod has no diner that can never win.
// - Whether the pie was won is the diner's open state, shared as a door's is (kReportRefState, the session's world
//   state): the presser's game watches what its player pressed, as it does a door, and says so once the pie is won. A
//   diner not won says nothing: nothing is un-won, here or in the session.
// - A win heard here, at a diner not won here, moments after the winner's press was replayed here (LIVE), is made
//   through the script by their stand-in (ForcePieSuccess and a press: the animation, the pie, PieSuccess). Any other
//   (a place loaded later, a join, a stand-in not there) sets the diner won, with no pie and no animation: the
//   session's one pie may be long taken, and the next press plays the script's win again as it does at a won diner.
//   So does a win being made through the script when our game stops, a save is written, or the script never makes it.
//
// Kept free of the game so the rules can be tested.
namespace DinerShare
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// The script, by the name the game's VM knows it by, and what is read and set of it.
	inline constexpr std::string_view SCRIPT = "PortaDinerScript";
	inline constexpr const char*      WON = "PieSuccess";         // bool
	inline constexpr const char*      TRIES = "Attempts";         // int
	inline constexpr const char*      FORCE = "ForcePieSuccess";  // bool
	inline constexpr const char*      BUSY = "Busy";

	// How far below its value Attempts is put for a press that must lose: half of it weighs against a roll of at
	// least 1 and a Luck term of a few dozen.
	inline constexpr std::int32_t LEVER = 2'000'000'000;
	// Attempts is counted in presses: more than this is not a count we lever (a mod's, or a broken one).
	inline constexpr std::int32_t MAX_TRIES = 100'000'000;

	inline constexpr auto LIVE = 30s;        // a win heard this soon after its press was replayed here is made with its pie
	inline constexpr auto LEVER_TIME = 10s;  // a lever the script never read (no press came of it) is taken off after this much of the game running
	inline constexpr auto FORCE_TIME = 60s;  // ... and a forced win that never came
	inline constexpr auto MAX_TICK = 1s;     // a frame counts for no more than this of it: a hitch is not time the script had

	// How much of the time since the diners were last looked at counts as the game running, for LEVER_TIME and
	// FORCE_TIME: none of a stopped game's, and no more than MAX_TICK of a frame's.
	constexpr Clock::duration Ran(Clock::duration a_sinceLast, bool a_stopped)
	{
		if (a_stopped || a_sinceLast <= Clock::duration::zero()) {
			return Clock::duration::zero();
		}
		return a_sinceLast < MAX_TICK ? a_sinceLast : Clock::duration{ MAX_TICK };
	}

	// Attempts as it is put for a press that must lose, or none where it can't be (not a count of presses).
	constexpr std::optional<std::int32_t> Lever(std::int32_t a_tries)
	{
		if (a_tries < 0 || a_tries > MAX_TRIES) {
			return std::nullopt;
		}
		return a_tries - LEVER;
	}

	// Whether a value of Attempts is a levered one (the script may have counted a press on top of it).
	constexpr bool IsLevered(std::int32_t a_tries)
	{
		return a_tries < -LEVER / 2;
	}

	// What a levered Attempts goes back to: the count before, and the press if the script counted it.
	constexpr std::int32_t Restored(std::int32_t a_tries)
	{
		return IsLevered(a_tries) ? a_tries + LEVER : a_tries;
	}

	// Whether the script has read a lever put at a_put (it counts the press after reading the count).
	constexpr bool WasRead(std::int32_t a_put, std::int32_t a_now)
	{
		return a_now != a_put;
	}

	// The script, as read.
	struct Script
	{
		bool won = false;   // PieSuccess
		bool busy = false;  // in its state Busy: a press does nothing
	};

	// What a friend's press at the diner does here.
	enum class Press : std::uint8_t
	{
		kNothing,  // busy here, or at work on it (a lever on, a win being made), or our game is stopped (a menu: its scripts stand
		           // still, and a lever would wait with them): the press isn't made here, and their win is heard all the same
		kReplay,   // won here: the stand-in presses, the script plays its "Succeed" again, no dice
		kLose,     // not won here: the stand-in presses with dice that cannot win
	};

	// a_working: a lever is on or a win is being made here, or our game is stopped.
	constexpr Press OnPress(const Script& a_here, bool a_working)
	{
		if (a_here.busy || a_working) {
			return Press::kNothing;
		}
		return a_here.won ? Press::kReplay : Press::kLose;
	}

	// What a friend's word of the diner does here.
	enum class Apply : std::uint8_t
	{
		kNothing,  // nothing to do: won here already, being won, or their diner isn't
		kWait,     // busy with a press: asked again when it isn't
		kWin,      // won through the script: the animation, the pie
		kSet,      // set won, with no pie
	};

	// a_open: their diner is won. a_levered: a lever is on here. a_forcing: a win is being made here already.
	// a_sincePress: how long ago a press by the player who says so was replayed here, if one was (none while our game is
	// stopped: nothing is made through a script that stands still).
	constexpr Apply Decide(bool a_open, const Script& a_here, bool a_levered, bool a_forcing, std::optional<Clock::duration> a_sincePress)
	{
		if (!a_open || a_here.won || a_forcing) {
			return Apply::kNothing;
		}
		if (a_here.busy || a_levered) {
			return Apply::kWait;
		}
		return a_sincePress && *a_sincePress <= LIVE ? Apply::kWin : Apply::kSet;
	}
}
