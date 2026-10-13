#pragma once

#include "game/WorkshopCells.h"

#include <algorithm>
#include <chrono>

// Whether a friend outside in our worldspace gets a stand-in here, and what becomes of a stand-in left in a cell that is
// not ours. Kept free of the game so it can be tested.
// The engine draws references only in the cells it has attached: a square of uGridsToLoad cells a side (5 by default)
// around the cell our player stands in. Outside, a friend used to count as in sight within 9,000 units, and a stand-in in a
// cell that is not attached was replaced at once: a friend 8,192 to 9,000 units off across a cell edge got a stand-in in a
// cell the engine had not attached, never drawn, replaced the next frame and the next, every frame, and hundreds of settler
// copies of them piled up waiting to be deleted.
namespace StandInRange
{
	using Duration = std::chrono::steady_clock::duration;

	// Outside, a friend further than this is never drawn, whatever the grid.
	constexpr float MAX_DISTANCE = 9000.0f;
	// The game's own uGridsToLoad, when the ini's cannot be read.
	constexpr int DEFAULT_GRIDS = 5;

	// How many cells out from our player's own the attached square reaches, for a uGridsToLoad.
	constexpr int LoadedRadius(int a_gridsToLoad)
	{
		if (a_gridsToLoad <= 0) {
			a_gridsToLoad = DEFAULT_GRIDS;
		}
		return std::clamp((a_gridsToLoad - 1) / 2, 1, 5);
	}

	// Whether the cell at grid (a_x, a_y) is in the square loaded around ours at (a_ourX, a_ourY).
	constexpr bool InLoadedGrid(std::int32_t a_x, std::int32_t a_y, std::int32_t a_ourX, std::int32_t a_ourY, int a_gridsToLoad)
	{
		const auto radius = LoadedRadius(a_gridsToLoad);
		const auto dx = a_x > a_ourX ? a_x - a_ourX : a_ourX - a_x;
		const auto dy = a_y > a_ourY ? a_y - a_ourY : a_ourY - a_y;
		return dx <= radius && dy <= radius;
	}

	// Whether a friend outside in our worldspace, at (a_x, a_y), is drawn here while our player stands at (a_ourX, a_ourY):
	// within MAX_DISTANCE, and in a cell the engine attaches around ours.
	inline bool Visible(float a_x, float a_y, float a_ourX, float a_ourY, int a_gridsToLoad)
	{
		const float dx = a_x - a_ourX;
		const float dy = a_y - a_ourY;
		return dx * dx + dy * dy < MAX_DISTANCE * MAX_DISTANCE &&
		       InLoadedGrid(WorkshopCells::Grid(a_x), WorkshopCells::Grid(a_y), WorkshopCells::Grid(a_ourX), WorkshopCells::Grid(a_ourY), a_gridsToLoad);
	}

	// The cell a stand-in stands in, as far as it matters here (TESObjectCELL::CELL_STATE).
	enum class Cell
	{
		kAttached,  // drawn
		kOnItsWay,  // loading, loaded or attaching: the engine is bringing it in
		kGone,      // detaching, unloading or not loaded
		kInterior,
		kNone,      // the stand-in is in no cell
	};

	// A stand-in in a cell that is coming in (a grid shift: our player crossed a cell edge) waits for it this long at
	// most, then is replaced.
	constexpr auto ATTACH_LIMIT = std::chrono::seconds(10);

	struct LeftFacts
	{
		bool     weInside = false;    // our player is in an interior
		Cell     cell = Cell::kNone;  // the stand-in's, which is not our player's
		bool     cellInGrid = false;  // that cell is in the square loaded around ours (outside)
		bool     drawn = false;       // the stand-in has had its 3D
		Duration sinceSpawned{};
	};

	enum class Left
	{
		kKeep,         // it stays: a neighbouring cell outside that is attached, or coming in
		kReplace,      // replaced at once: we went through a door, or it was drawn in a cell that has since gone
		kReplaceLater, // replaced, and none placed again for RESPAWN_WAIT: it was never drawn where it was placed
	};

	// What becomes of a stand-in whose cell is not our player's. A stand-in walks on across cell borders outside while its
	// cell stays the one it was placed in, so another cell outside is fine as long as the engine draws it.
	constexpr Left ChooseLeft(const LeftFacts& a_facts)
	{
		if (a_facts.weInside || a_facts.cell == Cell::kInterior || a_facts.cell == Cell::kNone) {
			return Left::kReplace;
		}
		if (a_facts.cell == Cell::kAttached) {
			return Left::kKeep;
		}
		// Attaching takes some frames, longer from a slow disk: replaced every frame meanwhile, it never got its 3D.
		if (a_facts.cell == Cell::kOnItsWay && a_facts.cellInGrid && a_facts.sinceSpawned < ATTACH_LIMIT) {
			return Left::kKeep;
		}
		// One drawn there before (its cell left the grid as our player walked on) is replaced at once, as through a door;
		// one never drawn was placed where the engine draws nothing, and placed again at once it would be again.
		return a_facts.drawn ? Left::kReplace : Left::kReplaceLater;
	}

	// After Left::kReplaceLater, no stand-in is placed for them for this long.
	constexpr auto RESPAWN_WAIT = std::chrono::seconds(1);

	constexpr bool MayRespawn(Duration a_sinceReplacedLater)
	{
		return a_sinceReplacedLater >= RESPAWN_WAIT;
	}
}
