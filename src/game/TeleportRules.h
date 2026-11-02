#pragma once

#include "game/AutoSaveRules.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>

// How the teleport key moves the player to a friend in the worldspace they are in already, and when the landing is safe
// for the travel autosave, kept free of the game so it can be tested (Party.cpp does what this answers).
//
// A SetPosition far across an exterior worldspace, beyond the cells loaded around the player, did not put the player
// where the friend was: in a test from near Corvega to a friend at Vault 111 (about 40,000 units), three teleports
// landed at the same wrong spot, about 5,700 units short of the friend and high in the air, with no ground loaded under
// it. The player fell and went down. The autosave the teleport asked for 3 s later was made in the middle of the fall,
// so the bleed-out loaded that save and the player fell and went down again, a loop only an older save broke. So:
// - a spot beyond the cells next to the player's own goes through the loading path (cow to the friend's cell, then
//   SetPosition once the player is there), as a friend in another worldspace does; a hop within them stays a SetPosition;
// - the autosave after a teleport waits until the player has stood on the ground for a moment, alive and not down, and
//   is skipped if that never comes;
// - from the move until that moment, a fall does no damage.
namespace TeleportRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr float CELL_SIZE = 4096.0f;  // an exterior cell's side, in units

	// The exterior cell a coordinate is in (as cow counts them).
	inline int GridOf(float a_coord)
	{
		return static_cast<int>(std::floor(a_coord / CELL_SIZE));
	}

	// Whether a spot in the exterior worldspace the player is in needs its cells loaded first: it is beyond the cells
	// next to the player's own (the 3 x 3 around them, always loaded with their ground). The game's loaded grid is
	// wider (5 x 5 by default), but its outer cells may still be loading, and a player's ini can make it smaller.
	inline bool NeedsLoad(float a_fromX, float a_fromY, float a_toX, float a_toY)
	{
		return std::abs(GridOf(a_toX) - GridOf(a_fromX)) > 1 || std::abs(GridOf(a_toY) - GridOf(a_fromY)) > 1;
	}

	// How long the player stands on the ground before the landing counts, at least as long as the cells around a move
	// within a worldspace take to load (AutoSaveRules::LANDING_SETTLE), and how long a landing is waited for.
	inline constexpr auto GROUNDED_FOR = 1s;
	inline constexpr auto GIVE_UP = 30s;
	// A fall within this long of the move does no damage, until the player has landed.
	inline constexpr auto FALL_HOLD = 15s;

	// What the game shows of the player, each frame after a teleport.
	struct Footing
	{
		bool loaded = false;    // their 3D is loaded
		bool onGround = false;  // the character controller is not jumping or in the air
		bool alive = false;     // not dead, and not down
		bool loading = false;   // a loading screen is up
	};

	enum class Step
	{
		kNone,     // no teleport being landed
		kWait,     // not landed yet
		kSave,     // landed: ask for the travel autosave now
		kLanded,   // landed, and the teleport wanted no autosave (a hop)
		kNoSave,   // down, dead, or no landing in GIVE_UP: no autosave of this teleport
	};

	// The landing after a teleport's last SetPosition.
	class Landing
	{
	public:
		// a_travel: the teleport is travel (AutoSaveRules::TeleportIsTravel, or any coc or cow), so it wants an autosave.
		void Begin(Clock::time_point a_now, bool a_travel)
		{
			movedAt = a_now;
			groundSince.reset();
			travel = a_travel;
			active = true;
		}

		void Cancel() { active = false; }

		[[nodiscard]] bool Active() const { return active; }

		// Whether a fall now does no damage: after the move, until the player has landed, for at most FALL_HOLD.
		[[nodiscard]] bool HoldsFall(Clock::time_point a_now) const
		{
			return active && a_now - movedAt < FALL_HOLD;
		}

		Step Update(Clock::time_point a_now, const Footing& a_footing)
		{
			if (!active) {
				return Step::kNone;
			}
			if (!a_footing.alive || a_now - movedAt >= GIVE_UP) {
				active = false;
				return Step::kNoSave;
			}
			if (a_footing.loading || !a_footing.loaded || !a_footing.onGround) {
				groundSince.reset();  // standing must be unbroken: a frame on the ground before the fall is no landing
				return Step::kWait;
			}
			if (!groundSince) {
				groundSince = a_now;
			}
			if (a_now - *groundSince < GROUNDED_FOR || a_now - movedAt < AutoSaveRules::LANDING_SETTLE) {
				return Step::kWait;
			}
			active = false;
			return travel ? Step::kSave : Step::kLanded;
		}

	private:
		Clock::time_point                movedAt{};
		std::optional<Clock::time_point> groundSince;
		bool                             travel = false;
		bool                             active = false;
	};
}
