#pragma once

#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

// What became of the input the dev channel's key, mouse and click commands send through Windows. Windows
// hands a key to whichever window is in front, so a press sent while something else is (a scheduled task's
// console on the VM, a screenshot helper, or the window the person at the PC is using) never reaches the
// game and lands in that window instead. The commands used to answer "pressing" either way, and a harness
// read a press that never arrived as a menu that ignored it (2026-10-11: a dialogue option "not
// taken" after a screenshot task had taken the front). Now a press goes out only with the game in front
// (brought there first if need be), one that can't is dropped, and the tally says which happened. Pure (a
// lock for the input threads), so it is unit tested.
namespace WindowFocus
{
	enum class Outcome : std::uint8_t
	{
		kSent,            // the game was in front: sent
		kSentAfterFront,  // the game was brought to the front first, then it was sent
		kDropped,         // the game could not be brought to the front: not sent
	};

	// An input waits this long at most for the game to come to the front, trying every STEP_MS.
	constexpr int FRONT_TRIES = 4;
	constexpr int FRONT_STEP_MS = 100;

	// The outcome of one input, from whether the game was in front before, and (when it wasn't) whether it
	// was once the input had asked for it.
	[[nodiscard]] constexpr Outcome Decide(bool a_frontBefore, bool a_frontAfter)
	{
		return a_frontBefore ? Outcome::kSent : a_frontAfter ? Outcome::kSentAfterFront : Outcome::kDropped;
	}

	// The game's window can be in front and still off every screen: a game minimised during its first load into the world keeps
	// the spot Windows parks minimised windows at, (-32000, -32000), once restored (2026-11-01, the rig's host). Keys still go to
	// it, but a real click lands on whatever window is under the cursor, which then takes the front: a click "fires nothing". So
	// the window is put back on the primary screen, and a click first puts the cursor over the window if it isn't.
	struct Rect
	{
		long left = 0;
		long top = 0;
		long right = 0;
		long bottom = 0;
	};

	[[nodiscard]] constexpr bool Contains(const Rect& a_rect, long a_x, long a_y)
	{
		return a_x >= a_rect.left && a_x < a_rect.right && a_y >= a_rect.top && a_y < a_rect.bottom;
	}

	// Where the cursor goes for a click that would land outside the window: its middle.
	[[nodiscard]] constexpr std::pair<long, long> Middle(const Rect& a_rect)
	{
		return { a_rect.left + (a_rect.right - a_rect.left) / 2, a_rect.top + (a_rect.bottom - a_rect.top) / 2 };
	}

	// A window title as the dev channel's one-line answers can carry it: printable ASCII, no quotes, at most
	// 60 characters ("" for none).
	[[nodiscard]] inline std::string Printable(std::string_view a_title)
	{
		std::string out;
		for (const char c : a_title) {
			if (out.size() >= 60) {
				break;
			}
			out += (c >= 0x20 && c < 0x7F && c != '\'') ? c : '?';
		}
		return out;
	}

	class Tally
	{
	public:
		// a_what: the command ("key 57"); a_front: the window that was in front instead, for a dropped one (made
		// printable by the caller: see Printable).
		void Record(Outcome a_outcome, std::string_view a_what, std::string_view a_front = {})
		{
			std::scoped_lock lock{ mutex };
			switch (a_outcome) {
			case Outcome::kSent:
				++sent;
				break;
			case Outcome::kSentAfterFront:
				++sent;
				++broughtFront;
				break;
			case Outcome::kDropped:
				++dropped;
				lastDropped = std::string{ a_what };
				lastFront = std::string{ a_front };
				break;
			}
		}

		// "sent=N broughtFront=M dropped=K", and the last one dropped with what was in front instead.
		[[nodiscard]] std::string Describe() const
		{
			std::scoped_lock lock{ mutex };
			auto out = std::format("sent={} broughtFront={} dropped={}", sent, broughtFront, dropped);
			if (dropped != 0) {
				out += std::format(" lastDropped='{}' over {}", lastDropped, lastFront);
			}
			return out;
		}

		[[nodiscard]] std::uint64_t Dropped() const
		{
			std::scoped_lock lock{ mutex };
			return dropped;
		}

	private:
		mutable std::mutex mutex;
		std::uint64_t      sent = 0;
		std::uint64_t      broughtFront = 0;
		std::uint64_t      dropped = 0;
		std::string        lastDropped;
		std::string        lastFront;
	};
}
