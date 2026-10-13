#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// A slow sweep over waiting entries that touches at most a few per pass, carrying on where the last pass
// stopped. Entries are named by a key ordered with <; entries that come and go between passes don't make
// it skip one that stays: the next pass starts after the last key visited, not at a position.
template <class Key>
class RetryCursor
{
public:
	explicit RetryCursor(std::size_t a_perPass) :
		perPass(a_perPass) {}

	// The keys this pass visits, from a_keys (any order): at most perPass, from after the last key the
	// previous pass visited, wrapping round to the start.
	[[nodiscard]] std::vector<Key> Pass(std::vector<Key> a_keys)
	{
		std::ranges::sort(a_keys);
		a_keys.erase(std::unique(a_keys.begin(), a_keys.end()), a_keys.end());
		auto start = last ? std::ranges::upper_bound(a_keys, *last) : a_keys.begin();
		std::vector<Key> visit;
		for (std::size_t i = 0; i < a_keys.size() && visit.size() < perPass; ++i) {
			if (start == a_keys.end()) {
				start = a_keys.begin();
			}
			visit.push_back(*start++);
		}
		if (!visit.empty()) {
			last = visit.back();
		}
		return visit;
	}

private:
	std::size_t        perPass;
	std::optional<Key> last;
};

// When what waits for its reference to load is tried again (WorldSync::Frame): each reference as the game
// says it loaded; everything when the player comes out of a loading screen (or the opening) or into another
// cell; otherwise a slow sweep of a few at a time (RetryCursor), for what loaded before its change came.
// What loads while nothing can be applied (a loading screen) isn't tried then: the full pass on the way out
// covers it. Pure scheduling, so it can be tested without the game: trying is the caller's.
class RetrySchedule
{
public:
	using Clock = std::chrono::steady_clock;

	// What to try this frame.
	struct Plan
	{
		std::vector<std::uint32_t> loaded;  // what the game said loaded since the last frame
		std::vector<std::uint32_t> others;  // the full pass, or this sweep's few
		bool                       full = false;
	};

	RetrySchedule(Clock::duration a_sweepInterval, std::size_t a_sweepEntries) :
		sweepInterval(a_sweepInterval), cursor(a_sweepEntries) {}

	// One frame. a_canApply: in the world and out of the opening; a_cell: the player's parent cell then.
	// a_waitingIds lists every reference something waits for; it is asked only when a pass needs it, and
	// only if a_anyWaiting.
	template <class WaitingIds>
	[[nodiscard]] Plan Frame(Clock::time_point a_now, bool a_canApply, std::uint32_t a_cell, std::vector<std::uint32_t> a_loaded, bool a_anyWaiting,
		WaitingIds&& a_waitingIds)
	{
		Plan plan;
		if (!a_canApply) {
			wasInWorld = false;
			return plan;
		}
		const bool arrived = !wasInWorld || a_cell != lastCell;
		wasInWorld = true;
		lastCell = a_cell;
		if (!a_anyWaiting) {
			return plan;
		}
		if (arrived) {
			plan.full = true;
			plan.others = a_waitingIds();
			nextSweep = a_now + sweepInterval;
			return plan;
		}
		plan.loaded = std::move(a_loaded);
		if (a_now >= nextSweep) {
			nextSweep = a_now + sweepInterval;
			plan.others = cursor.Pass(a_waitingIds());
		}
		return plan;
	}

private:
	Clock::duration            sweepInterval;
	RetryCursor<std::uint32_t> cursor;
	Clock::time_point          nextSweep{};
	bool                       wasInWorld = false;
	std::uint32_t              lastCell = 0;  // the player's parent cell at the last frame in the world
};

// How full the session's shared history looks from what our game was told (dead NPCs, picked-up items,
// container changes), in percent of what a session can hold: each of those stops being shared once its
// table is full.
[[nodiscard]] inline std::size_t SharedHistoryPercent(std::size_t a_dead, std::size_t a_pickups, std::uint32_t a_containerNext)
{
	const auto most = (std::max)({ a_dead, a_pickups, static_cast<std::size_t>(a_containerNext) });
	return most * 100 / Protocol::MAX_WORLD_STATE_ACTORS;
}
