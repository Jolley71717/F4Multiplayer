#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "game/ArmorFrames.h"
#include "game/MoveSync.h"

// A sneaking stand-in's legs, watched for a while after its weapon reached its holster. A friend who holsters mid
// sneak-walk had a stand-in that slid along crouched until they stopped: its graph goes from the weapon-drawn sneak
// (WeaponBehavior's WPNSneakReady_SM) to the weapon-away one (MTBehavior's MTSneakIdleLocomotion_SM) and comes to rest
// in its sneak idle, which refuses the resync's "moveStart" (MoveSync::NamesOf). The set-off sent the frame the weapon
// reached its holster (MoveSync::RestartAfterHolster) did not bring the legs back on the test rig, both ways: no foot
// event and no animation event at all in the 3 s after, the thigh drifting a sixth of a stride one way. The weapon is
// counted as away from an event part-way through the holster's clip, and what the graph does between that and the
// clip's end is not known here: a set-off sent then is early if the graph reaches the sneak idle only as the clip
// ends. A refusal can't tell "walks already" from "held in the sneak idle" (the walk refuses "moveStart" too, and
// takes "moveStartAnimated" by playing its set-off clip again), so the legs are what is looked at, as for a stand-in
// in power armor (ArmorFrames::Stride): the left thigh's rotation, which a walk swings by tens of degrees each stride
// and a graph at rest in an idle leaves almost where it was. While the stand-in is told to walk crouched with its
// weapon away and its thigh has stayed within ArmorFrames::STRIDE_MOVED of where it last moved to for STILL, the
// set-off is sent again, at most MAX_TRIES times in the WATCH after the holster. Legs that move are left alone, so a
// walk that goes on through the holster, or one a set-off brought back, is never set off again (no clip replayed).
// Kept free of the game so it can be tested.
namespace HolsterWatch
{
	using Clock = MoveSync::Clock;
	using Leg = ArmorFrames::LegPose;

	inline constexpr auto         WATCH = std::chrono::seconds(8);         // after the weapon reached its holster
	inline constexpr auto         STILL = std::chrono::milliseconds(600);  // legs still this long while it should walk: set off again
	inline constexpr auto         LONGEST_FRAME = std::chrono::milliseconds(100);  // a longer frame (a hitch, a load) counts as this
	inline constexpr std::uint8_t MAX_TRIES = 5;

	enum class Do : std::uint8_t
	{
		kNothing,
		kSetOff,   // the legs stood still while it should walk: send the set-off again (MoveSync::Drive's a_restart)
		kWalks,    // the legs move again, after a set-off sent here: for the log
		kGaveUp,   // still after MAX_TRIES set-offs: let be until its next holster, for the log
	};

	class Watch
	{
	public:
		// The weapon has just reached its holster on a sneaking humanoid (moving or not: one that sets off before the
		// holster's clip ends is watched too).
		void Arm(Clock::time_point a_now)
		{
			armed = true;
			until = a_now + WATCH;
			tries = 0;
			told = false;
			anchor.reset();
			still = {};
			last.reset();
		}

		void Clear() { armed = false; }

		[[nodiscard]] bool         Armed() const { return armed; }
		[[nodiscard]] std::uint8_t Tries() const { return tries; }
		// How long the legs have stood still while it should walk.
		[[nodiscard]] Clock::duration Still() const { return still; }

		// One frame. a_walks: it is told to move crouched with its weapon away (a sneaking humanoid, moving, faster than
		// MoveSync::START, on the ground); a_leg: its thigh's rotation (null: not readable, nothing is counted).
		Do Update(bool a_walks, const Leg* a_leg, Clock::time_point a_now)
		{
			if (!armed) {
				return Do::kNothing;
			}
			if (a_now >= until) {
				armed = false;
				return Do::kNothing;
			}
			const Clock::duration elapsed = last ? (std::min)(Clock::duration{ a_now - *last }, Clock::duration{ LONGEST_FRAME }) : Clock::duration{};
			last = a_now;
			if (!a_walks || !a_leg) {
				anchor.reset();
				still = {};
				return Do::kNothing;
			}
			if (!anchor || ArmorFrames::Moved(*anchor, *a_leg)) {
				const bool walksAgain = anchor && tries > 0 && !told;
				anchor = *a_leg;
				still = {};
				told = told || walksAgain;
				return walksAgain ? Do::kWalks : Do::kNothing;
			}
			still += elapsed;
			if (still < STILL) {
				return Do::kNothing;
			}
			if (tries >= MAX_TRIES) {
				armed = false;
				return Do::kGaveUp;
			}
			++tries;
			told = false;
			anchor = *a_leg;
			still = {};
			return Do::kSetOff;
		}

	private:
		bool                             armed = false;
		Clock::time_point                until{};
		std::uint8_t                     tries = 0;   // set-offs sent since the holster
		bool                             told = false;  // kWalks was said for the last set-off
		std::optional<Leg>               anchor;      // the pose the thigh last moved to
		Clock::duration                  still{};
		std::optional<Clock::time_point> last;
	};

	// Whether the watch starts this frame: a sneaking humanoid whose weapon has just reached its holster. a_wasSheathed
	// is the weapon's last frame (true when not known).
	constexpr bool Starts(bool a_sneakingHumanoid, bool a_wasSheathed, bool a_sheathed)
	{
		return a_sneakingHumanoid && !a_wasSheathed && a_sheathed;
	}
}
