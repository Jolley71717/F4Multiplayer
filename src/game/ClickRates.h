#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

// How often each thing the plugin does that the game answers with a sound happens, so a repeating sound (a constant
// clicking reported after a friend joined or left, bead F4Multiplayer-v7mg) shows in the log and the status line by
// name instead of being guessed at. Each source counts what it does in windows of WINDOW; a window with more than the
// source's LIMIT is one a sound repeated in, and the first such is reported, then one every REPORT_EVERY (one report for all
// the sources together, so the log holds a line a minute at most however many repeat). The limits are
// well above what play does (a fight, a join with a settlement's worth of friends' lamps), and well below a repeat that
// never ends. Kept free of the game so it can be tested; not thread-safe (ClickWatch holds the lock).
namespace ClickRates
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto WINDOW = 10s;
	inline constexpr auto REPORT_EVERY = 60s;

	enum class Source : std::uint8_t
	{
		kDrawAsk,        // a stand-in's weapon drawn or holstered to match its friend's (plays the weapon's sound)
		kEquip,          // an item put on a stand-in or a mirrored NPC (the engine's equip sound)
		kEmote,          // an idle played on a stand-in (the Pip-Boy check, a point, a cheer, a clap)
		kShotSound,      // a friend's gunshot played from their stand-in or a mirrored NPC, as a new sound
		kVoiceStart,     // a friend's voice started from an empty device (a new sentence, or after an underrun)
		kVoiceUnderrun,  // ... of them, a piece that came after the last one ran out
		kHud,            // a message shown at the HUD's top left
		kCount,
	};

	struct Info
	{
		const char*   label;
		std::uint32_t limit;  // most a window holds without it being a repeat
	};

	inline constexpr std::array<Info, static_cast<std::size_t>(Source::kCount)> INFOS{ {
		{ "drawAsks", 12 },
		{ "equips", 40 },
		{ "emotes", 8 },
		{ "shotSounds", 60 },
		{ "voiceStarts", 30 },
		{ "voiceUnderruns", 8 },
		{ "hud", 6 },
	} };

	constexpr const Info& InfoOf(Source a_source)
	{
		return INFOS[static_cast<std::size_t>(a_source)];
	}

	class Rates
	{
	public:
		// One or a_count of the source's events at a_now. True when the window now holds more than the source's limit and
		// no report, of this source or any other, has been made within REPORT_EVERY: the caller says it (a line in the
		// log). A source held back by another's report says it at its next event after the gap, if it still repeats.
		bool Note(Source a_source, Clock::time_point a_now, std::uint32_t a_count = 1)
		{
			auto& entry = entries[static_cast<std::size_t>(a_source)];
			Roll(a_source, entry, a_now);
			entry.total += a_count;
			entry.inWindow += a_count;
			if (entry.inWindow <= InfoOf(a_source).limit) {
				return false;
			}
			if (reported && a_now - reportedAt < REPORT_EVERY) {
				return false;
			}
			reported = true;
			reportedAt = a_now;
			return true;
		}

		// What the source did in the window it is in.
		[[nodiscard]] std::uint32_t InWindow(Source a_source) const { return entries[static_cast<std::size_t>(a_source)].inWindow; }
		[[nodiscard]] std::uint64_t Total(Source a_source) const { return entries[static_cast<std::size_t>(a_source)].total; }
		// The most any window held, the one in progress included.
		[[nodiscard]] std::uint32_t Peak(Source a_source) const
		{
			const auto& entry = entries[static_cast<std::size_t>(a_source)];
			return (std::max)(entry.peak, entry.inWindow);
		}
		// Windows that held more than the limit, the one in progress included.
		[[nodiscard]] std::uint32_t Repeats(Source a_source) const
		{
			const auto& entry = entries[static_cast<std::size_t>(a_source)];
			return entry.repeats + (entry.inWindow > InfoOf(a_source).limit ? 1 : 0);
		}

		// "clicks: drawAsks=3(peak 3/12) hud=1(peak 1/6) repeats=0", sources that did nothing left out.
		[[nodiscard]] std::string Describe() const
		{
			std::string out = "clicks:";
			std::uint32_t repeats = 0;
			bool          any = false;
			for (std::size_t i = 0; i < entries.size(); ++i) {
				const auto source = static_cast<Source>(i);
				repeats += Repeats(source);
				if (entries[i].total == 0) {
					continue;
				}
				any = true;
				out += std::format(" {}={}(peak {}/{})", InfoOf(source).label, entries[i].total, Peak(source), InfoOf(source).limit);
			}
			if (!any) {
				out += " none";
			}
			out += std::format(" repeats={}", repeats);
			return out;
		}

	private:
		struct Entry
		{
			bool              started = false;
			Clock::time_point windowStart{};
			std::uint32_t     inWindow = 0;
			std::uint32_t     peak = 0;
			std::uint32_t     repeats = 0;
			std::uint64_t     total = 0;
		};

		void Roll(Source a_source, Entry& a_entry, Clock::time_point a_now)
		{
			if (!a_entry.started) {
				a_entry.started = true;
				a_entry.windowStart = a_now;
				return;
			}
			if (a_now - a_entry.windowStart < WINDOW) {
				return;
			}
			a_entry.peak = (std::max)(a_entry.peak, a_entry.inWindow);
			a_entry.repeats += a_entry.inWindow > InfoOf(a_source).limit ? 1 : 0;
			a_entry.inWindow = 0;
			a_entry.windowStart = a_now;
		}

		std::array<Entry, static_cast<std::size_t>(Source::kCount)> entries{};
		bool                                                        reported = false;
		Clock::time_point                                           reportedAt{};
	};
}
