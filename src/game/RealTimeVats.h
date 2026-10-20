#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstdint>

// VATS in a session (iVatsMode, bRealTimeVats before it; the host's rule, Protocol::SessionRules::vatsMode). Alone,
// aiming in VATS slows the world almost to a stop and the shots play out slowed: in a session that slowed only our
// game, and friends saw us stand still while they ran about at full speed. Three modes, to compare in play:
//   0 real time: VATS runs at full speed, as in Fallout 76: the game's slow motion is set back to normal while VATS is up.
//   1 the game's slow motion: as it is alone. The VATS player's own game slows; friends' stand-ins in it keep moving at
//     full speed.
//   2 slowed friends: the game's slow motion stays, and every friend's stand-in in the VATS player's game (and the copies
//     of NPCs mirrored from a friend's game) is played back at the same slowed rate while VATS is up, so everyone looks
//     slowed to the VATS player. Friends' own games, and what they see of the VATS player, are as in 1.
// Free of the game, so it is tested; game/RealTime.cpp does it.
//
// The game slows VATS's aiming through its "magic" time slowdown (VATS::magicTimeSlowdown, 0.04 while aiming,
// measured on 1.11.240), the same one Jet sets (0.3). So Jet is told by the slowdown
// as it was before VATS came up: on Jet, VATS is left as the game has it, in every mode.
namespace RealTimeVats
{
	using Rules = Protocol::SessionRules;

	// A mode this build does not know is real time (SessionRules::Clamped).
	constexpr bool RunsInRealTime(std::uint8_t a_vatsMode)
	{
		return a_vatsMode == Rules::VATS_REAL_TIME || a_vatsMode > Rules::MAX_VATS_MODE;
	}

	constexpr bool SlowsFriends(std::uint8_t a_vatsMode)
	{
		return a_vatsMode == Rules::VATS_SLOWED_FRIENDS;
	}

	// For the log and the dev channel.
	constexpr const char* ModeName(std::uint8_t a_vatsMode)
	{
		switch (a_vatsMode) {
		case Rules::VATS_SLOW_MOTION:
			return "slow motion";
		case Rules::VATS_SLOWED_FRIENDS:
			return "slowed friends";
		default:
			return "real time";
		}
	}

	// bRealTimeVats, the key from before there were three modes: true is real time, false the game's slow motion.
	constexpr std::uint8_t ModeOfFlag(bool a_realTime)
	{
		return a_realTime ? Rules::VATS_REAL_TIME : Rules::VATS_SLOW_MOTION;
	}

	// a_multiplier: the game's global time multiplier now; a_slowdownBeforeVats: the magic time slowdown the last frame
	// before VATS came up (0 = none). The multiplier to set, or a negative value to leave it. Only real time sets it.
	constexpr float TimeMultiplier(bool a_inSession, std::uint8_t a_vatsMode, bool a_vatsUp, float a_multiplier, float a_slowdownBeforeVats)
	{
		if (!a_inSession || !RunsInRealTime(a_vatsMode) || !a_vatsUp || a_slowdownBeforeVats > 0.0f) {
			return -1.0f;
		}
		return a_multiplier < 0.999f ? 1.0f : -1.0f;
	}

	// How fast friends' stand-ins in our game play while we are in VATS: 1 (as they are) in every mode but 2, and in 2 the
	// game's own slowdown, so they slow as our world does. Alone, out of VATS, and on Jet (left as the game has it)
	// they are not slowed.
	constexpr float FriendsRate(bool a_inSession, std::uint8_t a_vatsMode, bool a_vatsUp, float a_multiplier, float a_slowdownBeforeVats)
	{
		if (!a_inSession || !SlowsFriends(a_vatsMode) || !a_vatsUp || a_slowdownBeforeVats > 0.0f || !(a_multiplier < 1.0f)) {
			return 1.0f;
		}
		return a_multiplier > 0.0f ? a_multiplier : 0.0f;
	}

	// Each frame: remembers the slowdown while VATS is down, and says what to set while it is up.
	class Clock
	{
	public:
		float Frame(bool a_inSession, std::uint8_t a_vatsMode, bool a_vatsUp, float a_multiplier, float a_slowdown)
		{
			if (!a_vatsUp) {
				before = a_slowdown;
				return -1.0f;
			}
			return TimeMultiplier(a_inSession, a_vatsMode, a_vatsUp, a_multiplier, before);
		}

		// After Frame, the same frame: the rate friends' stand-ins play at (FriendsRate).
		[[nodiscard]] float FriendsRate(bool a_inSession, std::uint8_t a_vatsMode, bool a_vatsUp, float a_multiplier) const
		{
			return RealTimeVats::FriendsRate(a_inSession, a_vatsMode, a_vatsUp, a_multiplier, before);
		}

		[[nodiscard]] float SlowdownBeforeVats() const { return before; }

	private:
		float before = 0.0f;
	};

	// Where friends' stand-ins are played from, so that they slow when our world does (mode 2). Their states are not
	// bent: the playback runs behind the present by a lag that grows by what the slowdown took out of each frame and,
	// once VATS is down, closes again faster than real time. The stand-ins draw the friend's path as it was `lag`
	// seconds ago (StateBuffer::Buffer::lag, MotionPath through FrameClock::PlaceAt): network time is never touched.
	// The most it can fall behind is MAX_LAG (the states are kept that long); a longer VATS than the slowdown can cover
	// plays at normal speed from there, as it does when the host keeps the game's slow motion for everyone.
	class Playback
	{
	public:
		static constexpr float MAX_LAG = 5.0f;     // seconds behind the present the stand-ins can be played
		static constexpr float CATCH_UP = 3.0f;    // how fast they play (times normal) while closing the lag after VATS
		static constexpr float MAX_STEP = 0.25f;   // seconds: a longer frame is a stall, which is no slowdown to follow

		// a_seconds of real time went by, with the stand-ins playing at a_rate (1 = as they are, below 1 slowed).
		void Advance(float a_seconds, float a_rate)
		{
			if (!(a_seconds > 0.0f)) {
				return;
			}
			a_seconds = (std::min)(a_seconds, MAX_STEP);
			if (a_rate < 1.0f) {
				lag = (std::min)(lag + (1.0f - (std::max)(a_rate, 0.0f)) * a_seconds, MAX_LAG);
			} else {
				lag = (std::max)(lag - (CATCH_UP - 1.0f) * a_seconds, 0.0f);
			}
		}

		// No lag at once: the session ended, or the rule is off.
		void Reset() { lag = 0.0f; }

		[[nodiscard]] float Lag() const { return lag; }

	private:
		float lag = 0.0f;
	};
}
