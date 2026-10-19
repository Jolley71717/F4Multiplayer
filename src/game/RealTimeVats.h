#pragma once

// VATS in a session (bRealTimeVats; the host's rule). Alone, aiming in VATS slows the world almost to a stop and the
// shots play out slowed: in a session that slowed only our game, and friends saw us stand still while they ran about
// at full speed. In real time VATS runs at full speed, as in Fallout 76: the game's slow motion is set back to normal
// while VATS is up. Free of the game, so it is tested; game/RealTime.cpp does it.
//
// The game slows VATS's aiming through its "magic" time slowdown (VATS::magicTimeSlowdown, 0.04 while aiming,
// measured on 1.11.240), which Jet and the like are taken to use too (not measured). So Jet is told by the slowdown
// as it was before VATS came up: on Jet, VATS is left as the game has it.
namespace RealTimeVats
{
	// a_multiplier: the game's global time multiplier now; a_slowdownBeforeVats: the magic time slowdown the last frame
	// before VATS came up (0 = none). The multiplier to set, or a negative value to leave it.
	constexpr float TimeMultiplier(bool a_inSession, bool a_realTimeVats, bool a_vatsUp, float a_multiplier, float a_slowdownBeforeVats)
	{
		if (!a_inSession || !a_realTimeVats || !a_vatsUp || a_slowdownBeforeVats > 0.0f) {
			return -1.0f;
		}
		return a_multiplier < 0.999f ? 1.0f : -1.0f;
	}

	// Each frame: remembers the slowdown while VATS is down, and says what to set while it is up.
	class Clock
	{
	public:
		float Frame(bool a_inSession, bool a_realTimeVats, bool a_vatsUp, float a_multiplier, float a_slowdown)
		{
			if (!a_vatsUp) {
				before = a_slowdown;
				return -1.0f;
			}
			return TimeMultiplier(a_inSession, a_realTimeVats, a_vatsUp, a_multiplier, before);
		}

		[[nodiscard]] float SlowdownBeforeVats() const { return before; }

	private:
		float before = 0.0f;
	};
}
