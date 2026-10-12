#pragma once

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The hang watch's judgement (HangWatch.cpp has the thread and the dump): when a stalled main thread
// counts as a hang, and which old dumps go. Pure, so it is unit tested.
namespace HangWatch
{
	using Clock = std::chrono::steady_clock;

	// What the watch thread saw on one of its once-a-second looks at the game.
	struct Sample
	{
		bool responsive = true;  // Windows doesn't call the game's window "not responding"
		bool loading = false;    // a loading screen is up
		bool paused = false;     // the watch thread itself woke up late: the PC slept, or a debugger stopped the game
	};

	class Detector
	{
	public:
		enum class Event
		{
			kNone,
			kHang,       // the main thread has been stalled past the limit (once per hang)
			kRecovered,  // it answers again after a kHang
		};

		struct Result
		{
			Event           event = Event::kNone;
			Clock::duration stalled{};  // how long the main thread has been (or, on kRecovered, was) stalled
		};

		// a_lag: how long the main thread has already been stalled when a sample first says it isn't
		// responding (Windows waits five seconds before it calls a window that).
		Detector(Clock::duration a_limit, Clock::duration a_loadingLimit, Clock::duration a_lag) :
			limit(a_limit), loadingLimit(a_loadingLimit), lag(a_lag)
		{}

		Result Tick(Clock::time_point a_now, const Sample& a_sample)
		{
			if (a_sample.responsive) {
				const auto stalled = since ? a_now - *since : Clock::duration{};
				since.reset();
				if (hung) {
					hung = false;
					return { Event::kRecovered, stalled };
				}
				return {};
			}
			// After a pause the main thread had no chance to answer yet, so its stall counts from now. Once
			// a hang is reported, it stays the same hang until the game answers.
			if (!hung && (!since || a_sample.paused)) {
				since = a_sample.paused ? a_now : a_now - lag;
			}
			const auto stalled = a_now - *since;
			// A loading screen is judged by its own limit: the engine can hold the main thread through
			// a big load for longer than it ever should in play.
			if (!hung && stalled >= (a_sample.loading ? loadingLimit : limit)) {
				hung = true;
				return { Event::kHang, stalled };
			}
			return { Event::kNone, stalled };
		}

		[[nodiscard]] bool Hung() const { return hung; }

	private:
		Clock::duration                  limit;
		Clock::duration                  loadingLimit;
		Clock::duration                  lag;
		std::optional<Clock::time_point> since;  // when the current stall began (estimated)
		bool                             hung = false;
	};

	constexpr std::string_view DUMP_PREFIX = "F4Multiplayer-hang-";
	constexpr std::string_view DUMP_SUFFIX = ".dmp";
	constexpr std::string_view PARTIAL_SUFFIX = ".dmp.tmp";  // being written (renamed when done)

	// "F4Multiplayer-hang-2026-10-11_23-05-09.dmp": local time, so it matches the log's times, and
	// in an order that sorts by name.
	inline std::string DumpName(int a_year, int a_month, int a_day, int a_hour, int a_minute, int a_second)
	{
		return std::format("{}{:04}-{:02}-{:02}_{:02}-{:02}-{:02}{}", DUMP_PREFIX, a_year, a_month, a_day, a_hour, a_minute, a_second, DUMP_SUFFIX);
	}

	// Which of a_names (the files in the log folder) to delete so that only the newest a_keep hang
	// dumps stay. A dump left half-written (the game was closed while it was written) always goes.
	inline std::vector<std::string> DumpsToRemove(std::vector<std::string> a_names, std::size_t a_keep)
	{
		std::vector<std::string> remove;
		std::vector<std::string> dumps;
		for (auto& name : a_names) {
			if (!name.starts_with(DUMP_PREFIX)) {
				continue;
			}
			if (name.ends_with(PARTIAL_SUFFIX)) {
				remove.push_back(std::move(name));
			} else if (name.ends_with(DUMP_SUFFIX)) {
				dumps.push_back(std::move(name));
			}
		}
		std::ranges::sort(dumps, std::greater{});
		for (std::size_t i = a_keep; i < dumps.size(); ++i) {
			remove.push_back(std::move(dumps[i]));
		}
		return remove;
	}
}
