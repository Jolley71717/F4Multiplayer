#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

#include "StandInSpace.h"

// A friend's stand-in steps aside when we are pressed against it. Kept free of the game so it can be tested.
//
// A stand-in has a body like any actor's, and it is put where its friend stands every frame: it has no AI to step out
// of our way as an NPC does, and no push moves it. Walked into dead-centre it is a pillar (two bodies touching, 56
// units between them, the stick pushed for 20 s and nothing moved), and a friend standing in a doorway shuts it. So
// when our player pushes at a stand-in right ahead and does not move for WEDGED, it is drawn a body's room to the
// side of the way we push, and we walk on through where they stand. It goes back to their spot once we are clear of
// it. Only where we draw them changes: their own game, and where they are for everything else, is as it was (there,
// our stand-in is kept out of their body by StandInSpace as we pass).
//
// A player who steers a little off-centre slides round the stand-in as round any body: that is movement, and nothing
// steps aside for it.
namespace StandInYield
{
	using Clock = std::chrono::steady_clock;
	using Point = StandInSpace::Point;

	constexpr float PUSHED = 0.5f;                               // movement input that counts as pushing (keys give 1)
	constexpr auto  WEDGED = std::chrono::milliseconds{ 750 };   // pushing at it this long without moving: it steps aside
	constexpr float STILL = 6.0f;                                // moved less than this since the push began: not moving
	constexpr float AHEAD = 0.5f;                                // the cosine between our push and the way to it (60 degrees)
	constexpr float SLIDE = 240.0f;                              // how fast it is drawn aside and back, units a second
	constexpr float MAX_STEP = 0.1f;                             // a frame longer than this (a hitch) slides it as one of this

	// By the room a body takes on the ground (StandInSpace::CLEAR, or CLEAR_ARMOR in power armor): two bodies touch at
	// about 1.4 of it (56 units, as measured).
	constexpr float Touch(float a_clear) { return a_clear * 1.6f; }    // closer than this, it can be what holds us
	constexpr float Lane(float a_clear) { return a_clear * 1.8f; }     // how far to the side of our way it is drawn
	constexpr float Release(float a_clear) { return a_clear * 2.0f; }  // this far from their spot we are clear of it

	// Our own player this frame.
	struct Us
	{
		Point at;
		float heading = 0.0f;  // radians, 0 along +y, a quarter turn along +x
		float inputX = 0.0f;   // the movement input the game reads: to our right
		float inputY = 0.0f;   // and ahead
	};

	struct State
	{
		std::optional<Point>             pressedFrom;   // where we stood as the push at it began
		Clock::time_point                pressedSince{};
		bool                             aside = false;  // it is drawn aside, or on its way there
		float                            wantX = 0.0f;   // how far from their spot it is to be drawn
		float                            wantY = 0.0f;
		float                            offX = 0.0f;    // and how far it is drawn now
		float                            offY = 0.0f;
		std::optional<Clock::time_point> last;           // the frame before this
		bool                             stepped = false;  // it began to step aside this frame (for the log and the count)
		bool                             returned = false; // it began to go back this frame
	};

	inline void Reset(State& a_state)
	{
		a_state = {};
	}

	// Whether it is drawn off their spot at all.
	inline bool Off(const State& a_state)
	{
		return a_state.offX != 0.0f || a_state.offY != 0.0f;
	}

	// Where their stand-in is drawn this frame. a_at: where they stand. a_allowed: both of us are on our feet and it
	// follows their spot (neither down, nor it in furniture, nor a loading screen); when not, it is at their spot at once.
	inline Point Frame(State& a_state, const Point& a_at, const Us& a_us, float a_clear, bool a_allowed, Clock::time_point a_now)
	{
		a_state.stepped = false;
		a_state.returned = false;
		const float dt = a_state.last ? std::clamp(std::chrono::duration<float>(a_now - *a_state.last).count(), 0.0f, MAX_STEP) : 0.0f;
		a_state.last = a_now;
		const bool numbers = std::isfinite(a_at.x) && std::isfinite(a_at.y) && std::isfinite(a_at.z) && std::isfinite(a_us.at.x) && std::isfinite(a_us.at.y) &&
		                     std::isfinite(a_us.at.z) && std::isfinite(a_us.heading) && std::isfinite(a_us.inputX) && std::isfinite(a_us.inputY);
		if (!a_allowed || !numbers) {
			const bool was = a_state.aside;
			Reset(a_state);
			a_state.last = a_now;
			a_state.returned = was;
			return a_at;
		}

		const bool  sameFloor = std::abs(a_at.z - a_us.at.z) < StandInSpace::TALL;
		const float fromSpot = std::hypot(a_at.x - a_us.at.x, a_at.y - a_us.at.y);
		// Clear of their spot, or on another floor: it goes back.
		if (a_state.aside && (!sameFloor || fromSpot >= Release(a_clear))) {
			a_state.aside = false;
			a_state.returned = true;
			a_state.wantX = 0.0f;
			a_state.wantY = 0.0f;
			a_state.pressedFrom.reset();
		}

		// Pushing at it, where it is drawn now, without moving.
		const float push = std::hypot(a_us.inputX, a_us.inputY);
		const float drawnX = a_at.x + a_state.offX;
		const float drawnY = a_at.y + a_state.offY;
		const float toX = drawnX - a_us.at.x;
		const float toY = drawnY - a_us.at.y;
		const float apart = std::hypot(toX, toY);
		bool        pressed = false;
		float       dirX = 0.0f;
		float       dirY = 1.0f;
		if (push >= PUSHED && sameFloor && apart < Touch(a_clear)) {
			// Our push on the ground: ahead is (sin, cos) of our heading, our right (cos, -sin).
			const float s = std::sin(a_us.heading);
			const float c = std::cos(a_us.heading);
			dirX = (a_us.inputX * c + a_us.inputY * s) / push;
			dirY = (-a_us.inputX * s + a_us.inputY * c) / push;
			// On our very spot there is no way to it to compare: it is in our way whichever way we push.
			pressed = apart < StandInSpace::SAME || (toX * dirX + toY * dirY) / apart >= AHEAD;
		}
		if (!pressed) {
			a_state.pressedFrom.reset();
		} else if (!a_state.pressedFrom || std::hypot(a_us.at.x - a_state.pressedFrom->x, a_us.at.y - a_state.pressedFrom->y) > STILL) {
			a_state.pressedFrom = a_us.at;  // we move (a slide round it counts): the push begins again from here
			a_state.pressedSince = a_now;
		} else if (a_now - a_state.pressedSince >= WEDGED) {
			// To the side of our way it is already on (our right when it is dead ahead, to within SAME), far enough that our
			// body passes.
			const float rightX = dirY;
			const float rightY = -dirX;
			const float beside = toX * rightX + toY * rightY;
			const float side = beside < -StandInSpace::SAME ? -1.0f : 1.0f;
			const float more = Lane(a_clear) - side * beside;
			a_state.wantX = a_state.offX + rightX * side * more;
			a_state.wantY = a_state.offY + rightY * side * more;
			// Never further from their spot than two lanes, however often we turn and push at it again.
			const float reach = std::hypot(a_state.wantX, a_state.wantY);
			if (reach > 2.0f * Lane(a_clear)) {
				a_state.wantX *= 2.0f * Lane(a_clear) / reach;
				a_state.wantY *= 2.0f * Lane(a_clear) / reach;
			}
			a_state.stepped = true;
			a_state.aside = true;
			a_state.pressedFrom.reset();  // the next push at it is timed afresh
		}

		// Drawn toward where it is wanted, SLIDE units a second.
		const float leftX = a_state.wantX - a_state.offX;
		const float leftY = a_state.wantY - a_state.offY;
		const float left = std::hypot(leftX, leftY);
		const float step = SLIDE * dt;
		if (left <= step || left <= 0.0f) {
			a_state.offX = a_state.wantX;
			a_state.offY = a_state.wantY;
		} else {
			a_state.offX += leftX / left * step;
			a_state.offY += leftY / left * step;
		}
		return { a_at.x + a_state.offX, a_at.y + a_state.offY, a_at.z };
	}
}
