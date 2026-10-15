#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Replayed gunshots whose sound loops (automatic weapons: the game stops the loop when the trigger
// is released, but a replayed shot has no trigger). Each one is ended after one shot's worth, unless
// the same shooter's next shot arrives first: then the loop runs on, as one burst, instead of a new
// sound starting over it. Every other replayed shot's sound is watched, and ended should it run on
// far longer than a gunshot (see Watch). Pure bookkeeping, so it can be tested without the game; Handle is
// RE::BSSoundHandle in the game, the key is the shooter's form ID.
template <class Handle>
class ShotLoops
{
public:
	using Clock = std::chrono::steady_clock;
	using Key = std::uint32_t;

	static constexpr auto SHOT_LENGTH = std::chrono::milliseconds(120);

	void Add(Key a_key, Handle a_handle, Clock::time_point a_now)
	{
		loops.push_back({ a_key, std::move(a_handle), a_now });
	}

	// The shooter's loop is still running: keep it going for another shot's worth. False if there
	// is none (its sound is over, or doesn't loop), so a new sound is wanted.
	bool Extend(Key a_key, Clock::time_point a_now)
	{
		for (auto& loop : loops) {
			if (loop.key == a_key) {
				loop.started = a_now;
				return true;
			}
		}
		return false;
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

	// Ends every loop of one shooter now (its actor is about to be deleted; the sound follows its 3D).
	template <class End>
	std::size_t EndFor(Key a_key, End&& a_end)
	{
		std::size_t ended = 0;
		std::erase_if(loops, [&](Loop& a_loop) {
			if (a_loop.key != a_key) {
				return false;
			}
			a_end(a_loop.handle);
			++ended;
			return true;
		});
		return ended;
	}

	[[nodiscard]] std::size_t Active() const { return loops.size(); }

	// Every other replayed shot's sound, one the engine does not call a loop: a gunshot and its tail last a second or
	// two. One still sounding LONGEST_SHOT after it started loops in a way the engine does not call one (the whole file
	// played over and over), or is no gunshot at all (the shooter's game named a sound mapping that is another sound
	// here, as when a plugin sits elsewhere in the load order): it would never stop, so it is ended then.
	static constexpr auto LONGEST_SHOT = std::chrono::seconds(6);

	void Watch(Key a_key, Handle a_handle, Clock::time_point a_now)
	{
		shots.push_back({ a_key, std::move(a_handle), a_now });
	}

	// Calls a_end(handle) for every watched shot LONGEST_SHOT old (the game ends it if it is still sounding) and
	// forgets it. Returns how many.
	template <class End>
	std::size_t EndLong(Clock::time_point a_now, End&& a_end)
	{
		std::size_t ended = 0;
		std::erase_if(shots, [&](Loop& a_shot) {
			if (a_now - a_shot.started < LONGEST_SHOT) {
				return false;
			}
			a_end(a_shot.handle);
			++ended;
			return true;
		});
		return ended;
	}

	// Ends every watched shot of one shooter now (its actor is about to be deleted; the sound follows its 3D).
	template <class End>
	std::size_t EndWatchedFor(Key a_key, End&& a_end)
	{
		std::size_t ended = 0;
		std::erase_if(shots, [&](Loop& a_shot) {
			if (a_shot.key != a_key) {
				return false;
			}
			a_end(a_shot.handle);
			++ended;
			return true;
		});
		return ended;
	}

	[[nodiscard]] std::size_t Watched() const { return shots.size(); }

private:
	struct Loop
	{
		Key               key;
		Handle            handle;
		Clock::time_point started;
	};
	std::vector<Loop> loops;
	std::vector<Loop> shots;  // watched (see Watch)
};
