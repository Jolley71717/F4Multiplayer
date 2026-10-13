#pragma once

#include <algorithm>
#include <chrono>

// A budget that holds up to a_burst and gains a_refillPerSecond back over time: what one player may do
// in a burst, and how often after that. Starts full. The time is passed in, so it can be tested
// without waiting.
class TokenBucket
{
public:
	using Clock = std::chrono::steady_clock;

	constexpr TokenBucket(double a_burst, double a_refillPerSecond) :
		burst(a_burst), refillPerSecond(a_refillPerSecond), tokens(a_burst)
	{}

	// Spends one; false (nothing spent) while the budget is empty.
	bool Take(Clock::time_point a_now)
	{
		if (started && a_now > last) {
			tokens = (std::min)(burst, tokens + std::chrono::duration<double>(a_now - last).count() * refillPerSecond);
		}
		if (!started || a_now > last) {
			last = a_now;
		}
		started = true;
		if (tokens < 1.0) {
			return false;
		}
		tokens -= 1.0;
		return true;
	}

private:
	double            burst;
	double            refillPerSecond;
	double            tokens;
	bool              started = false;
	Clock::time_point last{};
};
