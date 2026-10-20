#pragma once

#include <chrono>
#include <cstdint>

// A friend in their Pip-Boy: their stand-in raises its own, looks at it and lowers it, over and over while they are in
// it, and lowers it as they close it. Kept free of the game so it can be tested.
//
// Fallout 4 has no third-person "Pip-Boy up" pose for NPCs to hold: the player's own raise is first-person
// (_1stPerson\Behaviors\Pipboy.hkx). What NPCs have is the check of the wrist (IdlePipboyExamineMale 001C776B and
// IdlePipboyExamineFemale 001C776C in Fallout4.esm, both a dyn_Activation idle on the human graph, RaiderRootBehavior,
// of Actors\Character\Animations\DynamicAnims\[Female\]PipboyIdle1-3.hkx): the arm comes up, the head goes down to it,
// and the arm goes down again. It is played again as it ends for as long as they stay in the Pip-Boy, and IdleStop
// (000E9855, the same graph) ends it early when they close the Pip-Boy mid-way.
namespace StandInPipboy
{
	using Clock = std::chrono::steady_clock;

	// The idles, by editor ID (played the way the emotes are, the console's playidle).
	inline constexpr const char* RAISE_MALE = "IdlePipboyExamineMale";
	inline constexpr const char* RAISE_FEMALE = "IdlePipboyExamineFemale";
	inline constexpr const char* LOWER = "IdleStop";

	// How long one check of the wrist plays, about: it is played again this long after it began. Measured on the rig
	// (the stand-in's graph, dev animlog).
	inline constexpr auto REPLAY = std::chrono::milliseconds(6000);

	// The friend and their stand-in this frame.
	struct Facts
	{
		bool          inPipboy = false;  // their status says they are in the Pip-Boy
		std::uint32_t standIn = 0;       // their stand-in's form ID, 0 while they have none here
		// It can play the idle: its 3D is in, and it stands on its own feet (not in furniture, not in power armor, not down).
		bool          ready = false;
	};

	// What was played on which stand-in.
	struct Track
	{
		std::uint32_t      standIn = 0;
		bool               raised = false;  // a raise was played, and it may still be playing
		Clock::time_point  raisedAt{};
		std::uint32_t      raises = 0;      // for the status line
		std::uint32_t      lowers = 0;
	};

	enum class Do
	{
		kNothing,
		kRaise,  // play the raise (RAISE_MALE or RAISE_FEMALE)
		kLower,  // play LOWER
	};

	// What their stand-in plays this frame. A stand-in placed afresh (another form ID) starts with nothing played on it. In
	// the Pip-Boy it raises as soon as it can, and again each REPLAY. Out of it, a raise still playing is ended; one that
	// ended on its own needs nothing. A stand-in that can't play it any more (it sat down, got into power armor, went
	// down) is left to what it does now: nothing is ended on it, and once it can again it raises afresh.
	constexpr Do Frame(Track& a_track, const Facts& a_facts, Clock::time_point a_now)
	{
		if (a_facts.standIn != a_track.standIn) {
			a_track.standIn = a_facts.standIn;
			a_track.raised = false;
		}
		if (!a_facts.standIn) {
			return Do::kNothing;
		}
		if (!a_facts.ready) {
			a_track.raised = false;
			return Do::kNothing;
		}
		if (a_facts.inPipboy) {
			if (a_track.raised && a_now - a_track.raisedAt < REPLAY) {
				return Do::kNothing;
			}
			a_track.raised = true;
			a_track.raisedAt = a_now;
			++a_track.raises;
			return Do::kRaise;
		}
		if (!a_track.raised) {
			return Do::kNothing;
		}
		a_track.raised = false;
		if (a_now - a_track.raisedAt >= REPLAY) {
			return Do::kNothing;  // it ended on its own
		}
		++a_track.lowers;
		return Do::kLower;
	}

	// The raise for a stand-in of this sex (each idle is conditioned on it).
	constexpr const char* RaiseFor(bool a_female)
	{
		return a_female ? RAISE_FEMALE : RAISE_MALE;
	}
}
