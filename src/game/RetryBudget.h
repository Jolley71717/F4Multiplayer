#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// A fix-up tried on an NPC our game copies whenever its owner's states say it is needed: switching on a copy that
// is off here (NpcSync::SwitchOn) or bringing one in from another cell (BringIn). Each runs a console command,
// and the states come ten or twenty times a second, so it is tried at most every a_interval. One that never takes
// (a copy a quest keeps off in our game, or one that can't be moved) used to be tried forever, and logged each
// time; it gets TRIES now, and is left alone until the copy loads or the session starts over. Pure, so it can be
// tested without the game.
namespace RetryBudget
{
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t TRIES = 3;
	constexpr std::size_t   MAX_TRACKED = 1024;  // NPCs remembered; past that it starts over (a guard)

	enum class Verdict
	{
		kWait,   // tried moments ago, or given up: nothing now
		kFirst,  // try (and say so)
		kAgain,  // try quietly
		kLast,   // try, and say it is the last time
	};

	[[nodiscard]] constexpr bool Go(Verdict a_verdict) { return a_verdict != Verdict::kWait; }

	class Budget
	{
	public:
		explicit Budget(Clock::duration a_interval) :
			interval(a_interval)
		{}

		// Asked each time the fix-up looks needed.
		Verdict Ask(std::uint32_t a_id, Clock::time_point a_now)
		{
			auto it = entries.find(a_id);
			if (it == entries.end()) {
				if (entries.size() >= MAX_TRACKED) {
					entries.clear();
				}
				it = entries.emplace(a_id, Entry{}).first;
			}
			auto& entry = it->second;
			if (entry.tries >= TRIES || (entry.tries > 0 && a_now - entry.last < interval)) {
				return Verdict::kWait;
			}
			entry.last = a_now;
			++entry.tries;
			if (entry.tries == TRIES) {
				++gaveUp;
				return Verdict::kLast;
			}
			return entry.tries == 1 ? Verdict::kFirst : Verdict::kAgain;
		}

		// It worked (the copy loaded where it should be): a later need starts afresh.
		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		void Clear() { entries.clear(); }

		[[nodiscard]] std::uint32_t GaveUp() const { return gaveUp; }

	private:
		struct Entry
		{
			Clock::time_point last{};
			std::uint32_t     tries = 0;
		};

		Clock::duration                             interval;
		std::unordered_map<std::uint32_t, Entry>    entries;
		std::uint32_t                               gaveUp = 0;  // NPCs given their last try
	};
}
