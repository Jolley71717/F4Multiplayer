#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iterator>
#include <utility>
#include <vector>

// Reports that must not reach the server faster than it takes them. The server takes at most 500 events a
// second from a player (Server.cpp, MAX_EVENTS_PER_SECOND) and drops the rest, along with anything else of
// theirs that second: a kill, a pickup, a door. A blueprint placed, or "scrap all" in a big settlement, makes
// one report per piece in a single moment (well over a thousand), so those wait in line here and go out a
// few hundred a second. Kept free of the game so the rule can be tested.
namespace SendPacer
{
	using Clock = std::chrono::steady_clock;

	// Settlement reports a second: well under the server's 500, which kills, pickups, loot and the like
	// share. (The host's walk for junk its save had scrapped goes through the same line, at 100 a second.)
	inline constexpr std::size_t WORKSHOP_PER_SECOND = 200;
	// Reports that may wait in line: a scrap-all of the biggest settlement, with room to spare. A report is
	// a few dozen bytes; past this, more are dropped (the others won't see those pieces).
	inline constexpr std::size_t WORKSHOP_QUEUED = 8192;

	// At most a_perSecond sends in any rolling second (in any window that long, wherever it starts: the
	// server counts from a moment of its own).
	class Pacer
	{
	public:
		explicit Pacer(std::size_t a_perSecond, Clock::duration a_window = std::chrono::seconds(1)) :
			perSecond(a_perSecond), window(a_window)
		{}

		// How many may go now.
		[[nodiscard]] std::size_t Allowance(Clock::time_point a_now)
		{
			while (!sent.empty() && a_now - sent.front() >= window) {
				sent.pop_front();
			}
			return perSecond > sent.size() ? perSecond - sent.size() : 0;
		}

		// a_count went now.
		void Sent(Clock::time_point a_now, std::size_t a_count) { sent.insert(sent.end(), a_count, a_now); }

	private:
		std::size_t                   perSecond;
		Clock::duration               window;
		std::deque<Clock::time_point> sent;  // within the last window, oldest first (at most perSecond)
	};

	// Things to send, in the order they came, let out no faster than the pacer allows; at most a_max wait.
	template <class T>
	class Queue
	{
	public:
		Queue(std::size_t a_perSecond, std::size_t a_max, Clock::duration a_window = std::chrono::seconds(1)) :
			pacer(a_perSecond, a_window), max(a_max)
		{}

		// False if it's dropped (a_max already wait).
		bool Push(T a_item)
		{
			if (items.size() >= max) {
				return false;
			}
			items.push_back(std::move(a_item));
			return true;
		}

		// What may go now, oldest first: the rest stays in line, in order.
		[[nodiscard]] std::vector<T> TakeDue(Clock::time_point a_now)
		{
			const auto count = (std::min)(items.size(), pacer.Allowance(a_now));
			std::vector<T> out(std::make_move_iterator(items.begin()), std::make_move_iterator(items.begin() + static_cast<std::ptrdiff_t>(count)));
			items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(count));
			pacer.Sent(a_now, count);
			return out;
		}

		// Everything waiting, at once and not sent (it can't be: we're not in a session).
		[[nodiscard]] std::vector<T> TakeAll()
		{
			std::vector<T> out(std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
			items.clear();
			return out;
		}

		// Forgets what waits. What went out lately still counts: a line filled again right away goes no
		// faster.
		void Clear() { items.clear(); }

		[[nodiscard]] std::size_t Size() const { return items.size(); }
		[[nodiscard]] bool        Empty() const { return items.empty(); }

	private:
		Pacer         pacer;
		std::size_t   max;
		std::deque<T> items;
	};
}
