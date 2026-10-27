#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

// A log line for something that can happen many times a second (a report dropped, a hit ignored): one line per
// interval, and how many went by unlogged since the last. Every occurrence is counted (Count), so the log never hides
// how often it happens, only the repeats. Pure.
namespace LogPace
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration INTERVAL = std::chrono::seconds{ 10 };

	class Pace
	{
	public:
		// One more happened now: whether to log it (the first, and then one per a_interval).
		bool Due(Clock::time_point a_now, Clock::duration a_interval = INTERVAL)
		{
			++since;
			++total;
			if (logged && a_now - last < a_interval) {
				return false;
			}
			logged = true;
			last = a_now;
			return true;
		}

		// How many happened since the last line (this one included), for the line; starts the count again.
		std::uint32_t Since() { return std::exchange(since, 0); }

		[[nodiscard]] std::uint32_t Count() const { return total; }

	private:
		Clock::time_point last{};
		bool              logged = false;
		std::uint32_t     since = 0;
		std::uint32_t     total = 0;
	};
}
