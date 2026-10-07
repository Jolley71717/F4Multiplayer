#pragma once

#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

// Replayed gunshots whose sound loops (automatic weapons: the game stops the loop when the trigger
// is released, but a replayed shot has no trigger). Each one is ended after one shot's worth.
// Pure bookkeeping, so it can be tested without the game; Handle is RE::BSSoundHandle in the game.
template <class Handle>
class ShotLoops
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr auto SHOT_LENGTH = std::chrono::milliseconds(120);

	void Add(Handle a_handle, Clock::time_point a_now)
	{
		loops.push_back({ std::move(a_handle), a_now });
	}

	// Calls a_end(handle) for every loop older than SHOT_LENGTH and forgets it. Returns how many.
	template <class End>
	std::size_t EndDue(Clock::time_point a_now, End&& a_end)
	{
		std::size_t ended = 0;
		std::erase_if(loops, [&](Loop& a_loop) {
			if (a_now - a_loop.started < SHOT_LENGTH) {
				return false;
			}
			a_end(a_loop.handle);
			++ended;
			return true;
		});
		return ended;
	}

	[[nodiscard]] std::size_t Active() const { return loops.size(); }

private:
	struct Loop
	{
		Handle            handle;
		Clock::time_point started;
	};
	std::vector<Loop> loops;
};
