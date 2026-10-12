#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The hang watch's judgement (HangWatch.cpp has the thread and the files): when a stalled main thread
// counts as a hang, what the report says, and which old report files go. Pure, so it is unit tested.
namespace HangWatch
{
	using Clock = std::chrono::steady_clock;

	// What the main thread is busy with, as far as the plugin hears from the engine's events. Anything
	// but play gets the longer limit: a save, a load or a loading screen can hold the main thread far
	// longer than a frame ever should. In order of how much they say about a stall: a stall keeps the
	// furthest down this list it was seen in (Detector).
	enum class Phase : std::uint8_t
	{
		kPlay,           // in play or in a menu
		kLoadingScreen,  // a loading screen is up
		kSave,           // a save is being written: from F4SE's message before it to the one after
		kLoad,           // a save is being loaded: the same, for a load
	};

	inline std::string_view Describe(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kLoadingScreen:
			return "on a loading screen";
		case Phase::kSave:
			return "while saving";
		case Phase::kLoad:
			return "while loading a save";
		default:
			return "in play or a menu";
		}
	}

	// How long a save or load counts at most, in long limits. F4SE sends both ends, and one that never
	// came must not leave the longer limit on for good; but a slow save or load is one until it ends,
	// so this is far past the point where a stall in it has been reported.
	constexpr int SAVE_OR_LOAD_STALE = 10;

	// The phase now. A save or load counts until its end message, or until SAVE_OR_LOAD_STALE times
	// a_longLimit after it began. It outranks the loading screen a load puts up, since it says more.
	inline Phase CurrentPhase(bool a_loadingScreen, Phase a_saveOrLoad, Clock::duration a_saveOrLoadFor, Clock::duration a_longLimit)
	{
		if ((a_saveOrLoad == Phase::kSave || a_saveOrLoad == Phase::kLoad) && a_saveOrLoadFor < a_longLimit * SAVE_OR_LOAD_STALE) {
			return a_saveOrLoad;
		}
		return a_loadingScreen ? Phase::kLoadingScreen : Phase::kPlay;
	}

	// What the watch thread saw on one of its once-a-second looks at the game.
	struct Sample
	{
		bool  responsive = true;     // Windows doesn't call the game's window "not responding"
		Phase phase = Phase::kPlay;  // what the main thread is busy with
		bool  paused = false;        // the watch thread itself woke up late: the PC slept, or a debugger stopped the game
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
			Clock::duration stalled{};             // how long the main thread has been (or, on kRecovered, was) stalled
			Phase           phase = Phase::kPlay;  // the stall's, which sets its limit
		};

		// a_longLimit: the limit while saving, loading or on a loading screen. a_lag: how long the main
		// thread has already been stalled when a sample first says it isn't responding (Windows waits five
		// seconds before it calls a window that).
		Detector(Clock::duration a_limit, Clock::duration a_longLimit, Clock::duration a_lag) :
			limit(a_limit), longLimit(a_longLimit), lag(a_lag)
		{}

		Result Tick(Clock::time_point a_now, const Sample& a_sample)
		{
			if (a_sample.responsive) {
				const auto stalled = since ? a_now - *since : Clock::duration{};
				since.reset();
				if (hung) {
					hung = false;
					return { Event::kRecovered, stalled, phase };
				}
				return {};
			}
			// A stall keeps the most telling phase it was seen in, and with it the longer limit. A save
			// whose end message came before Windows saw the main thread answer again is a slow save that
			// ended, not a hang in play; one that outlasts the long limit is a hang while saving, however
			// the save or the stall ends.
			if (!since || a_sample.phase > phase) {
				phase = a_sample.phase;
			}
			// After a pause the main thread had no chance to answer yet, so its stall counts from now. Once
			// a hang is reported, it stays the same hang until the game answers.
			if (!hung && (!since || a_sample.paused)) {
				since = a_sample.paused ? a_now : a_now - lag;
			}
			const auto stalled = a_now - *since;
			if (!hung && stalled >= Limit(phase)) {
				hung = true;
				return { Event::kHang, stalled, phase };
			}
			return { Event::kNone, stalled, phase };
		}

		[[nodiscard]] Clock::duration Limit(Phase a_phase) const { return a_phase == Phase::kPlay ? limit : longLimit; }
		[[nodiscard]] bool            Hung() const { return hung; }

	private:
		Clock::duration                  limit;
		Clock::duration                  longLimit;
		Clock::duration                  lag;
		std::optional<Clock::time_point> since;                 // when the current stall began (estimated)
		Phase                            phase = Phase::kPlay;  // the current stall's
		bool                             hung = false;
	};

	// --- the report ---

	// How many bytes before a return address EndsWithCall looks at: the longest call instruction.
	constexpr std::size_t CALL_BYTES = 7;

	// Whether a_before (the CALL_BYTES bytes just before an address on the stack) ends with a call
	// instruction, which makes the address a likely return address rather than a stray pointer into
	// code: call rel32 (E8), or call through a register or memory (FF /2, with an optional REX prefix).
	inline bool EndsWithCall(std::span<const std::uint8_t> a_before)
	{
		const auto n = a_before.size();
		if (n >= 5 && a_before[n - 5] == 0xE8) {
			return true;
		}
		for (std::size_t length = 2; length <= (std::min)(n, CALL_BYTES); ++length) {
			const auto start = n - length;
			auto       at = start;
			if ((a_before[at] & 0xF0) == 0x40) {
				++at;  // REX
			}
			if (at + 1 >= n || a_before[at] != 0xFF) {
				continue;
			}
			const auto modrm = a_before[at + 1];
			if (((modrm >> 3) & 7) != 2) {
				continue;  // FF /2 is the near call; the others are inc, dec, jmp and push
			}
			const auto  mod = modrm >> 6;
			const auto  rm = modrm & 7;
			std::size_t size = at + 2 - start;
			if (mod != 3) {
				if (rm == 4) {
					if (at + 2 >= n) {
						continue;
					}
					++size;  // SIB
					if (mod == 0 && (a_before[at + 2] & 7) == 5) {
						size += 4;
					}
				} else if (mod == 0 && rm == 5) {
					size += 4;  // [rip + disp32]
				}
				size += mod == 1 ? 1 : mod == 2 ? 4 : 0;
			}
			if (size == length) {
				return true;
			}
		}
		return false;
	}

	// The file name at the end of a module's path, in plain ASCII: the report names modules, never
	// where they are (a path can carry the Windows user name).
	inline std::string ModuleName(std::wstring_view a_path)
	{
		const auto slash = a_path.find_last_of(L"\\/");
		const auto name = slash == std::wstring_view::npos ? a_path : a_path.substr(slash + 1);
		std::string out;
		out.reserve(name.size());
		for (const auto c : name) {
			out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
		}
		return out;
	}

	// What a hang report says (HangWatch.cpp fills it in).
	struct Report
	{
		std::string              version;  // the plugin's
		std::string              time;     // local, as in the file's name
		Phase                    phase = Phase::kPlay;
		std::int64_t             stalledSeconds = 0;
		std::int64_t             limitSeconds = 0;
		std::uint32_t            threadId = 0;
		std::string              where;    // the main thread's instruction ("Fallout4.exe+0x1A2B3C"); empty: not looked at
		std::vector<std::string> callers;  // return addresses found on its stack, nearest first
	};

	inline std::string ReportText(const Report& a_report)
	{
		auto text = std::format(
			"F4Multiplayer {} hang report, {}\n"
			"The game's main thread (thread {}) hadn't read its window messages for {} s ({}; the limit there is {} s).\n",
			a_report.version, a_report.time, a_report.threadId, a_report.stalledSeconds, Describe(a_report.phase), a_report.limitSeconds);
		if (a_report.where.empty()) {
			text += "Where it was: unknown (the main thread couldn't be looked at).\n";
			return text;
		}
		text += std::format("Where it was: {}\n", a_report.where);
		text += "Return addresses on its stack, nearest first (a scan, so some may be left over from earlier calls):\n";
		for (const auto& caller : a_report.callers) {
			text += std::format("  {}\n", caller);
		}
		if (a_report.callers.empty()) {
			text += "  none found\n";
		}
		return text;
	}

	// --- the files ---

	constexpr std::string_view FILE_PREFIX = "F4Multiplayer-hang-";
	constexpr std::string_view REPORT_SUFFIX = ".txt";
	constexpr std::string_view DUMP_SUFFIX = ".dmp";  // only with bHangDump
	constexpr std::string_view PARTIAL_SUFFIX = ".tmp";  // being written (renamed when done)

	// "F4Multiplayer-hang-2026-10-11_23-05-09" followed by a_suffix: local time, so it matches the
	// log's times, and in an order that sorts by name.
	inline std::string HangFileName(int a_year, int a_month, int a_day, int a_hour, int a_minute, int a_second, std::string_view a_suffix)
	{
		return std::format("{}{:04}-{:02}-{:02}_{:02}-{:02}-{:02}{}", FILE_PREFIX, a_year, a_month, a_day, a_hour, a_minute, a_second, a_suffix);
	}

	// Whether a_name is a name HangFileName makes with a_suffix (and not, say, a note of the player's
	// that happens to start the same way).
	inline bool IsHangFile(std::string_view a_name, std::string_view a_suffix)
	{
		constexpr std::string_view TIME = "0000-00-00_00-00-00";
		if (a_name.size() != FILE_PREFIX.size() + TIME.size() + a_suffix.size() || !a_name.starts_with(FILE_PREFIX) || !a_name.ends_with(a_suffix)) {
			return false;
		}
		const auto time = a_name.substr(FILE_PREFIX.size(), TIME.size());
		for (std::size_t i = 0; i < TIME.size(); ++i) {
			if (TIME[i] == '0' ? (time[i] < '0' || time[i] > '9') : time[i] != TIME[i]) {
				return false;
			}
		}
		return true;
	}

	// Which of a_names (the files in the log folder) to delete so that only the newest a_keep reports
	// and the newest a_keep dumps stay. A file left half-written (the game was closed while it was
	// written) always goes.
	inline std::vector<std::string> HangFilesToRemove(std::vector<std::string> a_names, std::size_t a_keep)
	{
		const auto ours = [](std::string_view a_name) { return IsHangFile(a_name, REPORT_SUFFIX) || IsHangFile(a_name, DUMP_SUFFIX); };
		std::vector<std::string> remove;
		std::vector<std::string> reports;
		std::vector<std::string> dumps;
		for (auto& name : a_names) {
			const std::string_view view{ name };
			if (view.ends_with(PARTIAL_SUFFIX) && ours(view.substr(0, view.size() - PARTIAL_SUFFIX.size()))) {
				remove.push_back(std::move(name));
			} else if (IsHangFile(view, REPORT_SUFFIX)) {
				reports.push_back(std::move(name));
			} else if (IsHangFile(view, DUMP_SUFFIX)) {
				dumps.push_back(std::move(name));
			}
		}
		for (auto* kind : { &reports, &dumps }) {
			std::ranges::sort(*kind, std::greater{});
			for (std::size_t i = a_keep; i < kind->size(); ++i) {
				remove.push_back(std::move((*kind)[i]));
			}
		}
		return remove;
	}
}
