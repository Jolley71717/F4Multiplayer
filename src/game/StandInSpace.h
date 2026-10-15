#pragma once

#include <cmath>
#include <optional>

// Where a friend's stand-in is put when they stand where we do. Kept free of the game so it can be tested.
//
// A friend can arrive right on top of us: a moveto onto our stand-in (to reach us while we are down), a door or fast
// travel to the spot we stand on, or both of us walking to one spot in two games. Their stand-in was put at their spot
// all the same, through our own body, and our camera sat inside its mesh (its belt and shirt filled the view). So a
// stand-in is put at its friend's spot unless that is closer to our feet than a body's room on the ground and on our
// floor; then it goes straight out from us to that distance, at their height. Their own game is not changed, only
// where we draw them: in theirs, our stand-in is kept out of their body the same way.
namespace StandInSpace
{
	struct Point
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	// Between our feet and theirs on the ground: a little more than two bodies' width, so the meshes do not meet. Two
	// people who walk up to each other stand about this far apart anyway (each game keeps its player out of the other's
	// stand-in), so only a friend placed on top of us is moved.
	constexpr float CLEAR = 40.0f;
	constexpr float CLEAR_ARMOR = 56.0f;  // when either of us is in power armor, which is wider
	// Feet further apart than this up or down are on another floor (or a ledge above): no body is in the way.
	constexpr float TALL = 120.0f;
	// Closer than this on the ground is our very spot, with no way out from us to go by: they go a step ahead of
	// themselves (along their own heading, which holds still while they stand), not toward wherever we turn.
	constexpr float SAME = 1.0f;

	// a_at: where the friend stands, a_heading: which way they face (radians, 0 along +y, a quarter turn along +x, as
	// a stand-in faces), a_us: our feet, a_clear: CLEAR, or CLEAR_ARMOR.
	inline Point Clear(const Point& a_at, float a_heading, const Point& a_us, float a_clear = CLEAR)
	{
		const float dx = a_at.x - a_us.x;
		const float dy = a_at.y - a_us.y;
		const float apart = std::hypot(dx, dy);
		if (!(apart < a_clear) || !(std::abs(a_at.z - a_us.z) < TALL)) {
			return a_at;  // room enough, another floor, or a spot that is not a number (left as it came)
		}
		float outX = 0.0f;
		float outY = 1.0f;
		if (apart >= SAME) {
			outX = dx / apart;
			outY = dy / apart;
		} else if (std::isfinite(a_heading)) {
			outX = std::sin(a_heading);
			outY = std::cos(a_heading);
		}
		return { a_us.x + outX * a_clear, a_us.y + outY * a_clear, a_at.z };
	}

	// The heading a friend had as they came onto our very spot, kept while they stay on it. Most look about once there
	// (after a moveto), and a stand-in put a step along the heading they have now would go round and round us.
	struct Away
	{
		std::optional<float> heading;
	};

	// Clear, for a friend seen each frame: on our very spot, the stand-in stays a step along the heading they came onto
	// it with (Away), whichever way they turn after.
	inline Point Place(Away& a_away, const Point& a_at, float a_heading, const Point& a_us, float a_clear = CLEAR)
	{
		const bool same = std::hypot(a_at.x - a_us.x, a_at.y - a_us.y) < SAME && std::abs(a_at.z - a_us.z) < TALL;
		if (!same) {
			a_away.heading.reset();
		} else if (!a_away.heading && std::isfinite(a_heading)) {
			a_away.heading = a_heading;
		}
		return Clear(a_at, same ? a_away.heading.value_or(a_heading) : a_heading, a_us, a_clear);
	}
}
