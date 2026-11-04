#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

// When the dedicated server tries its session file again after a write failed. On Windows the write can fail while
// another program (antivirus, a backup tool) holds the file open, and the cause usually goes by itself, so a failed save
// is tried again on its own, whatever else changed meanwhile: the time of day isn't counted as a change, so waiting for
// one would leave a save the host's game asked for undone for good.
//
// The wait doubles after each failure (from a_base, the regular check interval) up to a_cap, so a file that stays shut
// is tried once a minute at most, not at every check. A streak of failures is said once (Failed returns true at its
// first), and its end is said once (Succeeded). The time is passed in, so it can be tested without waiting.
class SaveRetry
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr std::chrono::milliseconds kLongestWait{ 60000 };

	explicit SaveRetry(std::chrono::milliseconds a_base) :
		base((std::max)(a_base, std::chrono::milliseconds{ 1 })),
		cap((std::max)(base, (std::min)(base * 8, kLongestWait)))
	{}

	// A save failed. True for the first of a streak, which is the one to log.
	bool Failed(Clock::time_point a_now)
	{
		const auto first = failures == 0;
		if (failures < kMaxDoublings) {
			++failures;
		}
		next = a_now + Wait();
		return first;
	}

	// A save went through. True if the one before had failed, which is the one to log.
	bool Succeeded()
	{
		return std::exchange(failures, 0) != 0;
	}

	bool Failing() const { return failures != 0; }

	// Time to try again: a save failed, and its wait is over.
	bool Due(Clock::time_point a_now) const { return failures != 0 && a_now >= next; }

	// How long after the last failure the next try is.
	std::chrono::milliseconds Wait() const
	{
		if (failures == 0) {
			return base;
		}
		return (std::min)(cap, base * (std::int64_t{ 1 } << (failures - 1)));
	}

private:
	// Far past where the wait reaches its cap, so the shift never overflows.
	static constexpr std::uint32_t kMaxDoublings = 30;

	std::chrono::milliseconds base;
	std::chrono::milliseconds cap;
	std::uint32_t             failures = 0;
	Clock::time_point         next{};
};
