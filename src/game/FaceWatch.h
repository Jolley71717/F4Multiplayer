#pragma once

#include <chrono>
#include <cstdint>

// A friend's own head on their stand-in (game/Face.h): whether the engine built it, and what to do when it did not.
// Kept free of the game so it can be tested.
//
// The stand-in is placed from a copy of a settler's base NPC wearing the friend's head parts, hair, complexion,
// sculpting and tints. The engine builds an NPC's head as it loads the actor's 3D: from the FaceGen files baked for it,
// or from its head parts and sculpting right then, as for the player. Our copy is a runtime form with no files, which the
// engine took for one with baked files and gave no head at all (2026-10-17, 3 of 3); game/FaceHeads.h has it built the
// player's way. Once (2026-10-15, see docs/ROADMAP.md) our own player's head went for as long as such a copy was loaded.
// So every stand-in placed with its own head is still watched; its first face is reported (kBuilt). With its 3D loaded and no face
// on it HEAD_WAIT later, the engine is asked to build its 3D again, once (what LooksMenu does after editing an NPC's face).
// With still no face HEAD_WAIT after that, or with our own player's face gone OUR_FACE_GRACE long while the stand-in is
// loaded, their own head is given up here: the stand-in is placed again with the settler's face (as before 0.8.8) and
// never with their head again this session. Our face gone with the stand-in gone too is asked back once.
namespace FaceWatch
{
	using Clock = std::chrono::steady_clock;

	// How long the engine gets to build a head once the stand-in's 3D is loaded, and again after a rebuild was asked.
	constexpr auto HEAD_WAIT = std::chrono::seconds(3);
	// Our own face is given that long to come back by itself (a frame of rebuilding is not it being taken).
	constexpr auto OUR_FACE_GRACE = std::chrono::seconds(2);
	// Our face still gone that long after the stand-in that took it was sent away is asked back.
	constexpr auto OUR_REPAIR_WAIT = std::chrono::seconds(2);
	// Builds asked for one stand-in: a head the engine does not build on the second try is not coming.
	constexpr int MAX_REBUILDS = 1;

	enum class Step
	{
		kNone,        // placed with the settler's face: nothing to watch
		kLoading,     // its 3D is not loaded (yet, or any more)
		kLoaded,      // its 3D is loaded; waiting for a face on it
		kRebuilding,  // a rebuild was asked; waiting for a face on it
		kBuilt,       // it has a face
		kGivenUp      // their own head is not shown here any more
	};

	// What the game shows this frame.
	struct Facts
	{
		bool has3D = false;     // the stand-in's 3D is loaded
		bool hasFace = false;   // ... and has a face node (a head) on it
		bool busy = false;      // it is in power armor, or going into it: its 3D is the frame's for now
		bool oursGone = false;  // our own player has 3D but no face node
	};

	enum class Then
	{
		kNothing,
		kRebuild,  // ask the engine to build the stand-in's 3D again
		kGiveUp,   // place it again with the settler's face
		kBuilt     // it has its own head: the first time since it was placed (for the log)
	};

	enum class Why
	{
		kNoHead,  // the engine built no head for it
		kOurs     // our own player's head went while it was loaded
	};

	struct Outcome
	{
		Then then = Then::kNothing;
		Why  why = Why::kNoHead;
	};

	struct StandIn
	{
		Step              step = Step::kNone;
		Clock::time_point since{};  // when the step began
		int               rebuilds = 0;
		bool              reported = false;  // its first face was reported (kBuilt)
		bool              oursGone = false;  // our face was missing last frame too
		Clock::time_point oursGoneSince{};
	};

	// Placed with their own head.
	inline void Placed(StandIn& a_standIn, Clock::time_point a_now)
	{
		a_standIn = {};
		a_standIn.step = Step::kLoading;
		a_standIn.since = a_now;
	}

	inline Outcome Frame(StandIn& a_standIn, const Facts& a_facts, Clock::time_point a_now)
	{
		auto& s = a_standIn;
		if (s.step == Step::kNone || s.step == Step::kGivenUp) {
			return {};
		}
		// Our own head, while the stand-in is loaded: gone for good, theirs goes.
		if (a_facts.has3D && a_facts.oursGone) {
			if (!s.oursGone) {
				s.oursGone = true;
				s.oursGoneSince = a_now;
			} else if (a_now - s.oursGoneSince >= OUR_FACE_GRACE) {
				s.step = Step::kGivenUp;
				return { Then::kGiveUp, Why::kOurs };
			}
		} else {
			s.oursGone = false;
		}
		if (a_facts.busy) {
			s.since = a_now;  // its head is looked at again once it is its own
			return {};
		}
		if (!a_facts.has3D) {
			if (s.step != Step::kLoading) {
				s.step = Step::kLoading;  // unloaded (a cell away, or for the rebuild): looked at again when it is back
				s.since = a_now;
			}
			return {};
		}
		if (s.step == Step::kLoading) {
			s.step = Step::kLoaded;
			s.since = a_now;
		}
		if (a_facts.hasFace) {
			s.step = Step::kBuilt;
			if (!s.reported) {
				s.reported = true;
				return { Then::kBuilt, Why::kNoHead };
			}
			return {};
		}
		switch (s.step) {
		case Step::kBuilt:
			s.step = Step::kLoaded;  // its head went: given the same chances again
			s.since = a_now;
			return {};
		case Step::kLoaded:
			if (a_now - s.since < HEAD_WAIT) {
				return {};
			}
			if (s.rebuilds < MAX_REBUILDS) {
				++s.rebuilds;
				s.step = Step::kRebuilding;
				s.since = a_now;
				return { Then::kRebuild, Why::kNoHead };
			}
			s.step = Step::kGivenUp;
			return { Then::kGiveUp, Why::kNoHead };
		case Step::kRebuilding:
			if (a_now - s.since < HEAD_WAIT) {
				return {};
			}
			s.step = Step::kGivenUp;
			return { Then::kGiveUp, Why::kNoHead };
		default:
			return {};
		}
	}

	// Our own player's head, after a stand-in was sent away for taking it.
	struct Ours
	{
		bool              armed = false;
		bool              repaired = false;  // asked back once already this session
		Clock::time_point armedAt{};
	};

	inline void TakenBy(Ours& a_ours, Clock::time_point a_now)
	{
		if (!a_ours.repaired) {
			a_ours.armed = true;
			a_ours.armedAt = a_now;
		}
	}

	// Whether to ask our face back now: once, if it is still gone OUR_REPAIR_WAIT after the stand-in went (back by itself
	// before then, nothing is asked).
	inline bool RepairDue(Ours& a_ours, bool a_oursGone, Clock::time_point a_now)
	{
		if (!a_ours.armed || a_now - a_ours.armedAt < OUR_REPAIR_WAIT) {
			return false;
		}
		a_ours.armed = false;
		if (!a_oursGone) {
			return false;
		}
		a_ours.repaired = true;
		return true;
	}
}
