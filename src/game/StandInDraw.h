#pragma once

#include <chrono>

// A stand-in that stands in our own cell and is not drawn (game/RemotePlayers.cpp). Kept free of the game so it can be tested.
//
// A stand-in placed in a cell that is not our player's is replaced when the engine stops drawing that cell (StandInRange.h),
// and one with its own head is watched for a head (FaceWatch.h); one in our own cell whose whole 3D never comes, or comes culled
// or faded out, stayed as it was until a door or a load: a friend on the compass, acting on the world, and not drawn (a player's
// report, 0.20.1: three times in 25-30 h; a disconnect and a reload did not bring it back, a restart did). So a stand-in in our
// cell (attached, no load on, its 3D its own: not a power armor frame's) that is not drawn for DRAW_WAIT is asked to build its 3D
// again (Actor::Reset3D, what LooksMenu does after editing a face), once; still not drawn DRAW_WAIT later it is placed again, up to
// MAX_REPLACES times; then it is left as it is and said so, once, with what the engine shows of it. A stand-in drawn again
// starts over.
namespace StandInDraw
{
	using Clock = std::chrono::steady_clock;

	// How long a stand-in that should be drawn gets before anything is done about it (a 3D loads and fades in within a second).
	constexpr auto DRAW_WAIT = std::chrono::seconds(5);
	// Fresh stand-ins placed for one that stayed undrawn, before it is left alone.
	constexpr int MAX_REPLACES = 2;

	// What the game shows this frame.
	struct Facts
	{
		bool  expected = false;  // it should be drawn: in our player's cell, which is attached, with no load on, out of power armor
		bool  has3D = false;     // its 3D is loaded
		bool  culled = false;    // ... and its root is culled (not drawn)
		float fade = 1.0f;       // ... and its root's fade (0: drawn fully transparent)
	};

	constexpr bool Drawn(const Facts& a_facts)
	{
		return a_facts.has3D && !a_facts.culled && a_facts.fade > 0.0f;
	}

	enum class Then
	{
		kNothing,
		kRebuild,  // ask the engine to build its 3D again
		kReplace,  // place a fresh stand-in
		kGiveUp    // left as it is, said once
	};

	// What is kept of one friend's stand-ins from frame to frame.
	struct Track
	{
		bool              undrawn = false;  // not drawn though expected, since undrawnSince
		Clock::time_point undrawnSince{};
		bool              rebuilt = false;  // this stand-in was asked to build its 3D again
		int               replaces = 0;     // fresh stand-ins placed in this spell
		bool              gaveUp = false;   // said once
	};

	// A new stand-in was placed for them: watched afresh; the spell's replaces are kept until one is drawn.
	constexpr void Placed(Track& a_track)
	{
		a_track.undrawn = false;
		a_track.rebuilt = false;
	}

	constexpr Then Frame(Track& a_track, const Facts& a_facts, Clock::time_point a_now)
	{
		if (!a_facts.expected) {
			a_track.undrawn = false;  // not looked at: looked at afresh when it should be drawn again
			return Then::kNothing;
		}
		if (Drawn(a_facts)) {
			a_track.undrawn = false;
			a_track.replaces = 0;
			a_track.gaveUp = false;
			return Then::kNothing;
		}
		if (!a_track.undrawn) {
			a_track.undrawn = true;
			a_track.undrawnSince = a_now;
			return Then::kNothing;
		}
		if (a_now - a_track.undrawnSince < DRAW_WAIT) {
			return Then::kNothing;
		}
		if (!a_track.rebuilt) {
			a_track.rebuilt = true;
			a_track.undrawnSince = a_now;  // given DRAW_WAIT again for the rebuild
			return Then::kRebuild;
		}
		if (a_track.replaces < MAX_REPLACES) {
			++a_track.replaces;
			return Then::kReplace;
		}
		if (a_track.gaveUp) {
			return Then::kNothing;
		}
		a_track.gaveUp = true;
		return Then::kGiveUp;
	}
}
