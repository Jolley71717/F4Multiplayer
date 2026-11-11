#pragma once

#include <cmath>

// VATS in a session (bRealTimeVats; the host's rule). Alone, aiming in VATS slows the world almost to a stop and the
// shots play out slowed: in a session that slowed only our game, and friends saw us stand still while they ran about
// at full speed. In real time VATS runs at full speed, as in Fallout 76: the game's slow motion is set back to normal
// while VATS is up. Free of the game, so it is tested; game/RealTime.cpp does it.
//
// The game slows VATS's aiming through its "magic" time slowdown (VATS::magicTimeSlowdown, 0.04 while aiming,
// measured on 1.11.240), the same one Jet sets (0.3). So Jet is told by the slowdown
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
	//
	// a_group: the group's slow motion in effect (game/VatsTime.h Group: the host's bGroupVatsSlowdown, while anyone in the
	// session is in VATS; 0 = none). While it is on, every game runs at that speed, in VATS or not, over real time VATS;
	// only a game slowed further on its own outside VATS (Jet) is left as it is. When it ends, a game it slowed is set back
	// to full speed, unless the game slows itself by then (VATS without real time, Jet), which it then keeps.
	class Clock
	{
	public:
		float Frame(bool a_inSession, bool a_realTimeVats, bool a_vatsUp, float a_multiplier, float a_slowdown, float a_group = 0.0f)
		{
			if (!a_vatsUp) {
				before = a_slowdown;
			}
			if (a_inSession && a_group > 0.0f) {
				grouped = true;
				if (!a_vatsUp && a_slowdown > 0.0f && a_multiplier < a_group) {
					return -1.0f;  // Jet: slower than the group already
				}
				return std::abs(a_multiplier - a_group) > 0.001f ? a_group : -1.0f;
			}
			if (grouped) {
				grouped = false;
				if (!a_vatsUp && a_slowdown <= 0.0f && a_multiplier < 0.999f) {
					return 1.0f;
				}
			}
			if (!a_vatsUp) {
				return -1.0f;
			}
			return TimeMultiplier(a_inSession, a_realTimeVats, a_vatsUp, a_multiplier, before);
		}

		[[nodiscard]] float SlowdownBeforeVats() const { return before; }
		// The group's slow motion was set last frame.
		[[nodiscard]] bool Grouped() const { return grouped; }

	private:
		float before = 0.0f;
		bool  grouped = false;
	};
}
