#pragma once

#include <cmath>

// Pure clock arithmetic behind WorldClock, kept free of the game so it is unit tested.
namespace ClockMath
{
	// The signed shortest distance in hours from a_from to a_to on the 24-hour clock, in (-12, 12].
	inline double HourGap(double a_to, double a_from)
	{
		double gap = std::fmod(a_to - a_from + 36.0, 24.0) - 12.0;
		return gap <= -12.0 ? gap + 24.0 : gap;
	}

	// Hours to move so the local clock lines up with the owner's time of day: forward, or back if
	// that stays on the same day (never back across midnight).
	inline double LineUp(double a_ownerHour, double a_localHour)
	{
		double delta = std::fmod(a_ownerHour - a_localHour + 24.0, 24.0);
		if (delta > 12.0 && a_localHour - (24.0 - delta) >= 0.0) {
			delta -= 24.0;
		}
		return delta;
	}
}
