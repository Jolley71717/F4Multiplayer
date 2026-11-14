#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
	// the window is put back on the primary screen, and a click first puts the cursor on a free point of the window.
	struct Rect
	{
		long left = 0;
		long top = 0;
		long right = 0;
		long bottom = 0;
	};

	// A screen point (a POINT, which WindowFromPoint takes by value).
	struct Point
	{
		long x;
		long y;
	};

	[[nodiscard]] constexpr bool Contains(const Rect& a_rect, long a_x, long a_y)
	{
		return a_x >= a_rect.left && a_x < a_rect.right && a_y >= a_rect.top && a_y < a_rect.bottom;
	}

	// A click goes to the window under the cursor, which is not always the game even with the game in front: a window kept on
	// top of every other one (a system error box, "cl.exe - Application Error" from a compiler crash on 2026-11-10) covers part
	// of it, and a press there lands on that window, which takes the front. So a click's cursor goes on a point of the game's
	// client area (never its title bar or borders, where a press drags the window) that the game's own main window is under: the
	// cursor's own point when it is one, else the first free one of these, else none and the click is refused. The points, in
	// order: the middle, then a grid over the area kept off its edges (where a taskbar can sit): the same points as
	// tools/input.ps1's Set-ClickSpot.
	[[nodiscard]] inline std::vector<std::pair<long, long>> ClickSpots(const Rect& a_client)
	{
		constexpr std::array<long, 5>      percent{ 50, 35, 65, 20, 80 };
		const auto                         width = a_client.right - a_client.left;
		const auto                         height = a_client.bottom - a_client.top;
		std::vector<std::pair<long, long>> out;
		for (const auto fy : percent) {
			for (const auto fx : percent) {
				out.emplace_back(a_client.left + width * fx / 100, a_client.top + height * fy / 100);
			}
		}
		return out;
	}

	enum class Spot : std::uint8_t
	{
		kKept,     // the cursor's own point: on the client area, the game's main window under it
		kMoved,    // the cursor goes to the first free point of ClickSpots
		kCovered,  // no point is free: nothing is clicked
	};

	struct SpotFound
	{
		Spot spot;
		long x;  // the point to click (kCovered: the cursor's own point)
		long y;
	};

	// a_gameAt(x, y): whether the window at that screen point is the game's main window (the caller's WindowFromPoint).
	template <class GameAt>
	[[nodiscard]] SpotFound FindSpot(const Rect& a_client, long a_cursorX, long a_cursorY, GameAt&& a_gameAt)
	{
		if (Contains(a_client, a_cursorX, a_cursorY) && a_gameAt(a_cursorX, a_cursorY)) {
			return { Spot::kKept, a_cursorX, a_cursorY };
		}
		for (const auto& [x, y] : ClickSpots(a_client)) {
			if (a_gameAt(x, y)) {
				return { Spot::kMoved, x, y };
			}
		}
		return { Spot::kCovered, a_cursorX, a_cursorY };
	}

	// Text as the dev channel's one-line answers can carry it: printable ASCII, no quotes, at most 60 characters ("" for none).
	[[nodiscard]] inline std::string Printable(std::string_view a_text)
	{
		std::string out;
		for (const char c : a_text) {
			if (out.size() >= 60) {
				break;
			}
			out += (c >= 0x20 && c < 0x7F && c != '\'') ? c : '?';
		}
		return out;
	}

	// A window named for an answer, as tools/input.ps1 names one: its process (the image's file name without .exe, or "pid N"
	// when that can't be read), its window class in brackets, and whether it is kept on top of every other window. Never its
	// title: a chat's or a browser tab's title can hold a person's name, and these answers go into rig results pasted into PRs.
	[[nodiscard]] inline std::string WindowName(std::string_view a_image, unsigned long a_pid, std::string_view a_class, bool a_topmost)
	{
		auto base = a_image.substr(a_image.find_last_of("\\/") + 1);
		if (base.size() > 4) {
			const auto ext = base.substr(base.size() - 4);
			if (ext[0] == '.' && (ext[1] == 'e' || ext[1] == 'E') && (ext[2] == 'x' || ext[2] == 'X') && (ext[3] == 'e' || ext[3] == 'E')) {
				base.remove_suffix(4);
			}
		}
		const auto process = base.empty() ? std::format("pid {}", a_pid) : Printable(base);
		return std::format("{} [{}]{}", process, Printable(a_class), a_topmost ? " (kept on top)" : "");
	}

	class Tally
	{
	public:
		// a_what: the command ("key 57"); a_front: for a dropped one, the window that was in front instead, or the one
		// over a click's point (named by the caller: see WindowName).
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
