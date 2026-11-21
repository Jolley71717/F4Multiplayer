#pragma once

#include <chrono>

// A friend who stands near us and is not shown (game/RemotePlayers.cpp). Kept free of the game so it can be tested.
//
// A player's report (0.20.1): a friend on the compass, acting on the world, and not drawn; three times in 25-30 h; a disconnect and
// a reload did not bring them back, a restart did. The compass goes by their newest state; what is drawn is a stand-in, and every
// way one can fail to be seen is looked for here, by that same newest state: when it says they are near us, in our space, in cells
// the engine draws, they should be seen. The ways (Why):
//   - no stand-in stands for them (placing one failed, or something kept one from being placed);
//   - it stands in a cell the engine does not draw (StandInRange.h replaces one left in such a cell; this is what is left over);
//   - its 3D never came;
//   - its 3D is culled, faded out (while they are within FADE_NEAR of us), or scaled to nothing;
//   - its 3D is a bare skeleton: no body, clothes or head on it;
//   - it is drawn somewhere else: far from where they are.
// One in a power armor frame is not looked at (its 3D is the frame's: ArmorFrames has its own watch), nor anything while a load is
// on, nor a friend whose states have stopped coming (their own loading screen, their game stopped): that one is only where they
// were. A friend not seen for DRAW_WAIT is said once, with the way; then, when a stand-in stands, it is asked to build its 3D again
// (Actor::Reset3D, what LooksMenu does after editing a face), once; still not seen DRAW_WAIT later it is placed again; then placed
// again plain: the default settler, without the copy of the base that wears their face and without their LooksMenu data, the two
// things a fresh stand-in takes over from the one before; then it is left as it is and said so, once. When none stands and placing
// one has failed, the next is placed plain at once. Seen again, it all starts over (a plain one stays plain for that friend until
// the next load or a new look from them).
namespace StandInDraw
{
	using Clock = std::chrono::steady_clock;

	// How long a friend who should be seen gets before anything is done about it (a 3D loads and fades in within a second or two).
	constexpr auto DRAW_WAIT = std::chrono::seconds(5);
	// Fresh stand-ins placed for one that stayed unseen, the last of them plain, before it is left alone.
	constexpr int MAX_REPLACES = 2;
	// Only a friend this near (by their newest state) is looked at: well inside the cells the engine draws and the distance it
	// fades actors out at.
	constexpr float NEAR = 2500.0f;
	// A stand-in's 3D further than this from where they are is drawn somewhere else.
	constexpr float ELSEWHERE = 2000.0f;
	// A 3D faded out counts only this near: further off the engine fades actors out by the player's own actor fade setting.
	constexpr float FADE_NEAR = 800.0f;
	// A 3D scaled below this is not seen.
	constexpr float TINY = 0.05f;

	// What the game shows this frame.
	struct Facts
	{
		bool  wanted = false;       // they should be seen: near us in our space by their newest state, their states still coming, no load on
		bool  placed = false;       // a stand-in stands for them
		bool  spawnFailed = false;  // ... none, and placing one has failed since the last one stood
		bool  busy = false;         // its 3D is a power armor frame's (or it is on its way into one): not looked at
		bool  cellDrawn = false;    // it is in a cell the engine draws
		bool  has3D = false;        // its 3D is loaded
		bool  culled = false;       // ... with its root culled
		float fade = 1.0f;          // ... its root's fade (0: fully transparent)
		float scale = 1.0f;         // ... its root's scale
		int   parts = 1;            // ... what hangs on its skeleton: body, clothes and head pieces
		float off = 0.0f;           // ... how far it is from where they are
		float away = 0.0f;          // how far they are from us
	};

	enum class Why
	{
		kSeen,
		kNone,       // no stand-in
		kCell,       // in a cell the engine does not draw
		kNo3D,
		kCulled,
		kFaded,
		kTiny,
		kBare,       // a skeleton with nothing on it
		kElsewhere,
	};

	constexpr Why WhyNot(const Facts& a_facts)
	{
		if (!a_facts.placed) {
			return Why::kNone;
		}
		if (!a_facts.cellDrawn) {
			return Why::kCell;
		}
		if (!a_facts.has3D) {
			return Why::kNo3D;
		}
		if (a_facts.culled) {
			return Why::kCulled;
		}
		if (!(a_facts.fade > 0.0f) && a_facts.away <= FADE_NEAR) {
			return Why::kFaded;
		}
		if (!(a_facts.scale >= TINY)) {
			return Why::kTiny;
		}
		if (a_facts.parts <= 0) {
			return Why::kBare;
		}
		if (!(a_facts.off <= ELSEWHERE)) {
			return Why::kElsewhere;
		}
		return Why::kSeen;
	}

	constexpr const char* Name(Why a_why)
	{
		switch (a_why) {
		case Why::kSeen:
			return "seen";
		case Why::kNone:
			return "no stand-in stands for them";
		case Why::kCell:
			return "it is in a cell the engine does not draw";
		case Why::kNo3D:
			return "it has no 3D";
		case Why::kCulled:
			return "its 3D is culled";
		case Why::kFaded:
			return "its 3D is faded out";
		case Why::kTiny:
			return "its 3D is scaled to nothing";
		case Why::kBare:
			return "its 3D is a bare skeleton";
		case Why::kElsewhere:
			return "it is drawn somewhere else";
		}
		return "?";
	}

	enum class Then
	{
		kNothing,
		kSay,      // none stands, for a reason of the game's or ours: said once
		kRebuild,  // ask the engine to build its 3D again
		kReplace,  // place a fresh stand-in
		kPlain,    // place a fresh one, plain (or, none standing, the next one)
		kGiveUp,   // left as it is, said once
		kSeenAgain // seen after any of the above: said once
	};

	// What is kept of one friend from frame to frame.
	struct Track
	{
		bool              unseen = false;   // not seen though wanted, since unseenSince
		Clock::time_point unseenSince{};
		bool              said = false;     // none standing was said in this spell
		bool              rebuilt = false;  // this stand-in was asked to build its 3D again
		int               replaces = 0;     // fresh stand-ins placed in this spell
		bool              plain = false;    // theirs are placed plain
		bool              gaveUp = false;   // said once
		bool              acted = false;    // something was said or done in this spell
	};

	// A new stand-in was placed for them: watched afresh; the spell's replaces are kept until one is seen.
	constexpr void Placed(Track& a_track)
	{
		a_track.unseen = false;
		a_track.rebuilt = false;
	}

	// A load, or a new look from them: their own face and data get their chance again.
	constexpr void Afresh(Track& a_track)
	{
		a_track = {};
	}

	constexpr Then Frame(Track& a_track, const Facts& a_facts, Clock::time_point a_now)
	{
		if (!a_facts.wanted || (a_facts.placed && a_facts.busy)) {
			a_track.unseen = false;  // not looked at: looked at afresh when they should be seen again
			return Then::kNothing;
		}
		if (WhyNot(a_facts) == Why::kSeen) {
			a_track.unseen = false;
			a_track.said = false;
			a_track.replaces = 0;
			a_track.gaveUp = false;
			if (a_track.acted) {
				a_track.acted = false;
				return Then::kSeenAgain;
			}
			return Then::kNothing;
		}
		if (!a_track.unseen) {
			a_track.unseen = true;
			a_track.unseenSince = a_now;
			return Then::kNothing;
		}
		if (a_now - a_track.unseenSince < DRAW_WAIT) {
			return Then::kNothing;
		}
		if (!a_facts.placed) {
			// Nothing to rebuild or replace. Placing failed: the next is tried plain. Else it is said, once.
			if (a_facts.spawnFailed && !a_track.plain) {
				a_track.plain = true;
				a_track.unseenSince = a_now;
				a_track.acted = true;
				return Then::kPlain;
			}
			if (a_track.said) {
				return Then::kNothing;
			}
			a_track.said = true;
			a_track.acted = true;
			return Then::kSay;
		}
		if (!a_track.rebuilt) {
			a_track.rebuilt = true;
			a_track.unseenSince = a_now;  // given DRAW_WAIT again for the rebuild
			a_track.acted = true;
			return Then::kRebuild;
		}
		if (a_track.replaces < MAX_REPLACES) {
			++a_track.replaces;
			if (a_track.replaces == MAX_REPLACES && !a_track.plain) {
				a_track.plain = true;
				a_track.acted = true;
				return Then::kPlain;
			}
			a_track.acted = true;
			return Then::kReplace;
		}
		if (a_track.gaveUp) {
			return Then::kNothing;
		}
		a_track.gaveUp = true;
		return Then::kGiveUp;
	}
}
