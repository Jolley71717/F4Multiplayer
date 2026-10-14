#pragma once

#include "game/StandInRange.h"

#include <chrono>
#include <cstdint>
#include <optional>

// A friend's Pip-Boy light: the light we keep at their stand-in while it is on. Kept free of the game so it can be tested.
// A reference stays in the cell it was made in, and a stand-in's own cell isn't kept up to date while it walks outside
// either (it stays the one it was placed in). The light was remade whenever its cell was not the stand-in's: once the
// stand-in had walked out of its first cell, that was every frame (it flickered, the deleted lights piled up waiting to go,
// and a save caught many of them). It was also remade every frame while its cell was still being brought in.
namespace FriendLight
{
	using Clock = std::chrono::steady_clock;
	using Duration = Clock::duration;

	// Where a reference is, as far as being in one place goes.
	struct Place
	{
		std::uint32_t cell = 0;        // its cell's form ID; 0 while it is in none (still attaching)
		bool          interior = false;
		std::uint32_t worldspace = 0;  // its cell's worldspace, outside
	};

	// Two references in one place: the same cell, or both outside in the same worldspace (a neighbouring cell outside is
	// the same place: a stand-in walks on across cell borders). Not known while either is in no cell yet.
	constexpr std::optional<bool> SamePlace(const Place& a_one, const Place& a_other)
	{
		if (!a_one.cell || !a_other.cell) {
			return std::nullopt;
		}
		return a_one.cell == a_other.cell || (!a_one.interior && !a_other.interior && a_one.worldspace == a_other.worldspace);
	}

	// A light made in a cell the engine is still bringing in (it is drawn only once that cell is attached) waits for it this
	// long at most, as a stand-in does, then is made again.
	constexpr auto ATTACH_LIMIT = StandInRange::ATTACH_LIMIT;

	// A friend's light and stand-in this frame, as the engine has them.
	struct Facts
	{
		bool                    wanted = false;  // their Pip-Boy light is on, and their stand-in stands here with its 3D in
		bool                    lit = false;     // a light of theirs stands
		Place                   light;           // where it is
		StandInRange::CellState lightCell = StandInRange::CellState::kNotLoaded;  // the state of its cell
		bool                    attached = false;  // its cell has been attached since it was placed (this frame included)
		Duration                sincePlaced{};
		Place                   standIn;         // where the stand-in is
	};

	enum class Action
	{
		kNone,     // no light, and none wanted
		kRemove,   // the light goes
		kPlace,    // a light is placed at the stand-in
		kReplace,  // the light goes, and a new one is placed at the stand-in
		kMove,     // the light stays, and is moved to the stand-in
	};

	// What becomes of a friend's light this frame. One already standing stays (and is moved along with the stand-in) while
	// it is in the stand-in's place, a neighbouring cell outside included, and the engine draws its cell, or is bringing
	// it in for one just placed. It is replaced when the stand-in is somewhere else (through a door, into another
	// worldspace), or when the engine lets go of its cell as the stand-in walks on: then once, in the cell the stand-in
	// has reached.
	constexpr Action Choose(const Facts& a_facts)
	{
		if (!a_facts.wanted) {
			return a_facts.lit ? Action::kRemove : Action::kNone;
		}
		if (!a_facts.lit) {
			return Action::kPlace;
		}
		if (!a_facts.light.cell) {
			return Action::kMove;  // still attaching: its place is not known yet
		}
		if (const auto same = SamePlace(a_facts.light, a_facts.standIn); same && !*same) {
			return Action::kReplace;
		}
		const auto cell = StandInRange::CellOf(false, a_facts.lightCell);
		if (cell == StandInRange::Cell::kAttached) {
			return Action::kMove;
		}
		// A cell that was drawn and no longer is has left the loaded area; one never drawn yet may still be coming in.
		if (cell == StandInRange::Cell::kOnItsWay && !a_facts.attached && a_facts.sincePlaced < ATTACH_LIMIT) {
			return Action::kMove;
		}
		return Action::kReplace;
	}

	// What is kept of a friend's light from frame to frame.
	struct Track
	{
		bool              attached = false;  // its cell has been attached since it was placed
		Clock::time_point placedAt{};
	};

	// The frame's look at a friend's light (a_facts without attached and sincePlaced, which come from a_track).
	// RemotePlayers::UpdateLight asks this every frame, and calls Placed when it places a light.
	constexpr Action Frame(Track& a_track, Facts a_facts, Clock::time_point a_now)
	{
		a_track.attached = a_track.attached || (a_facts.lit && a_facts.lightCell == StandInRange::CellState::kAttached);
		a_facts.attached = a_track.attached;
		a_facts.sincePlaced = a_now - a_track.placedAt;
		return Choose(a_facts);
	}

	// A new light was placed for them.
	constexpr void Placed(Track& a_track, Clock::time_point a_now)
	{
		a_track.attached = false;
		a_track.placedAt = a_now;
	}
}
