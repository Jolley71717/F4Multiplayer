#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// How many lines of a kind go to the log. Each of CopyWatch's lines (a hit on a copy, a drop given back, a report set,
// a death) had a cap for the whole run: 100 hits, 60 give-backs, 40 reports, 40 deaths. A fight that went wrong four
// hours into a session left nothing of itself in the log the player sent: the caps were used up in its first minutes.
// So each kind has its lines a WINDOW instead (Budget), the window starting with the first line asked for after the
// last one ended, what was left out is counted, and the counters go to the log every WINDOW (Every, Pick).
//
// Pure, so it can be tested without the game: tests/RollingLogTests.cpp.
namespace RollingLog
{
	using Clock = std::chrono::steady_clock;

	inline constexpr Clock::duration WINDOW = std::chrono::minutes{ 10 };

	class Budget
	{
	public:
		constexpr explicit Budget(std::uint32_t a_limit, Clock::duration a_window = WINDOW) :
			limit(a_limit),
			window(a_window)
		{}

		// Whether a line goes to the log now (counted against the window), or is left out (counted as such).
		bool Take(Clock::time_point a_now)
		{
			if (!started || a_now - since >= window) {
				started = true;
				since = a_now;
				used = 0;
			}
			if (used >= limit) {
				++leftOut;
				return false;
			}
			++used;
			return true;
		}

		// Lines left out since the run began.
		[[nodiscard]] constexpr std::uint32_t LeftOut() const { return leftOut; }

	private:
		std::uint32_t     limit;
		Clock::duration   window;
		bool              started = false;
		Clock::time_point since{};
		std::uint32_t     used = 0;
		std::uint32_t     leftOut = 0;
	};

	// A periodic line: due a_every after the first ask, then a_every after the last time it was due.
	class Every
	{
	public:
		constexpr explicit Every(Clock::duration a_every = WINDOW) :
			every(a_every)
		{}

		bool Due(Clock::time_point a_now)
		{
			if (!started) {
				started = true;
				last = a_now;
				return false;
			}
			if (a_now - last < every) {
				return false;
			}
			last = a_now;
			return true;
		}

		void Reset() { started = false; }

	private:
		Clock::duration   every;
		bool              started = false;
		Clock::time_point last{};
	};

	// The "key=value" entries of a status line (as `net status` writes each subsystem's Describe) whose keys are among
	// a_keys, in the line's order, space-separated: the damage counters picked out of the long lines for the summary.
	// A key matches a whole entry's key only ("dead" is not "deadline"'s).
	[[nodiscard]] inline std::string Pick(std::string_view a_line, std::initializer_list<std::string_view> a_keys)
	{
		std::string out;
		std::size_t at = 0;
		while (at < a_line.size()) {
			if (a_line[at] == ' ') {
				++at;
				continue;
			}
			const auto end = (std::min)(a_line.find(' ', at), a_line.size());
			const auto entry = a_line.substr(at, end - at);
			at = end;
			const auto equals = entry.find('=');
			if (equals == std::string_view::npos || equals == 0) {
				continue;
			}
			const auto key = entry.substr(0, equals);
			for (const auto wanted : a_keys) {
				if (key == wanted) {
					if (!out.empty()) {
						out += ' ';
					}
					out += entry;
					break;
				}
			}
		}
		return out;
	}
}
