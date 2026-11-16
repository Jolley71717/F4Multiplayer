#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>

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

		// Whether Take would let a line through now, nothing counted either way: for a line that can wait for the next
		// window (one said once for a thing that stays as it is) where leaving it out would lose it for good.
		[[nodiscard]] constexpr bool Open(Clock::time_point a_now) const { return !started || a_now - since >= window || used < limit; }

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

	// A line that comes again and again for one subject. A hurt copy of a friend's NPC that regains health by itself
	// every second, and has it taken back each time (CopyWatch), wrote one line a second for as long as it stayed hurt
	// and used up the kind's Budget for every other copy. So a line is said the first time for a subject and then at
	// most once every a_every, with how many went unsaid since and what they added up to. One a good deal bigger than
	// the line last said (BIGGER times it) is something else happening to the same subject (a legendary's mutation on
	// a copy that also regenerates) and is said at once.
	class Repeats
	{
	public:
		static constexpr float       BIGGER = 2.0f;
		static constexpr std::size_t MAX_SUBJECTS = 1024;  // a guard: past it a new subject's lines are all said, as before

		struct Line
		{
			bool          say = false;
			std::uint32_t unsaid = 0;        // lines like it left unsaid for this subject since its last one said
			float         unsaidSum = 0.0f;  // ... and what their amounts add up to
		};

		explicit Repeats(Clock::duration a_every = WINDOW / 2) :
			every(a_every)
		{}

		// A line of a_amount for a_subject now: whether to say it, and what went unsaid before it.
		Line Note(std::uint32_t a_subject, float a_amount, Clock::time_point a_now)
		{
			const auto it = subjects.find(a_subject);
			if (it == subjects.end()) {
				if (subjects.size() < MAX_SUBJECTS) {
					subjects.emplace(a_subject, Subject{ a_now, a_now, a_amount, 0, 0.0f });
				}
				return Line{ true, 0, 0.0f };
			}
			auto& subject = it->second;
			subject.lastAt = a_now;
			if (a_now - subject.saidAt < every && a_amount <= subject.said * BIGGER) {
				++subject.unsaid;
				subject.unsaidSum += a_amount;
				return Line{};
			}
			const Line line{ true, subject.unsaid, subject.unsaidSum };
			subject = Subject{ a_now, a_now, a_amount, 0, 0.0f };
			return line;
		}

		// Forgets the subjects nothing was noted for in the last a_every: their lines have stopped, and the next one is
		// a first again.
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(subjects, [&](const auto& a_entry) { return a_now - a_entry.second.lastAt >= every; });
		}

		void Clear() { subjects.clear(); }

		[[nodiscard]] std::size_t Size() const { return subjects.size(); }

	private:
		struct Subject
		{
			Clock::time_point saidAt{};     // when its last line was said
			Clock::time_point lastAt{};     // when its last line was noted, said or not
			float             said = 0.0f;  // the amount of the line last said
			std::uint32_t     unsaid = 0;
			float             unsaidSum = 0.0f;
		};

		Clock::duration                            every;
		std::unordered_map<std::uint32_t, Subject> subjects;
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
