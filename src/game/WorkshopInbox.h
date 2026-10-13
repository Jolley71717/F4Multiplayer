#pragma once

#include "game/WorkshopCells.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <type_traits>
#include <utility>
#include <vector>

// What other players built, moved and scrapped, on its way into our world: in line, oldest first, and
// placed a few a frame (a settlement of a thousand pieces, sent on joining or after every load, froze
// the game while it was placed at once); and what waits for its cell to load. Kept free of the game so
// the order rules can be tested.
namespace WorkshopInbox
{
	inline constexpr std::size_t APPLY_PER_FRAME = 32;
	inline constexpr auto        APPLY_TIME_PER_FRAME = std::chrono::microseconds(2000);

	template <class Key, class Item>
	class Inbox
	{
	public:
		using Waiting = WorkshopCells::Waiting<Key, Item>;

		// A report from another player: it goes after everything before it. One under the same key waiting
		// for its cell is older, and goes: in line again after this one, it would undo it (a piece moved
		// back, a scrapped one standing again).
		void Arrive(const Key& a_key, const Item& a_item)
		{
			waiting.Erase(a_key);
			queue.emplace_back(a_key, a_item);
		}

		// A cell came in: what waits under these keys gets in line again (a key can come up twice).
		void Retry(const std::vector<Key>& a_keys)
		{
			for (const auto& key : a_keys) {
				if (const auto item = waiting.Find(key)) {
					queue.emplace_back(key, *item);
					waiting.Erase(key);
				}
			}
		}

		// Hands up to a_max reports, oldest first, to a_apply(key, item), stopping early once a_budget has
		// passed by a_now() (at least one goes each time, so the line always moves). a_apply files what
		// can't be placed yet under Wait(). The number handed over.
		template <class Now, class Apply>
		std::size_t Drain(std::size_t a_max, typename std::invoke_result_t<Now>::duration a_budget, Now&& a_now, Apply&& a_apply)
		{
			const auto  start = a_now();
			std::size_t done = 0;
			while (!queue.empty() && done < a_max) {
				if (done > 0 && a_now() - start >= a_budget) {
					break;
				}
				auto [key, item] = std::move(queue.front());
				queue.pop_front();
				a_apply(key, item);
				++done;
			}
			return done;
		}

		// Files a report until its cell loads (see WorkshopCells::Waiting::Put).
		void Wait(const WorkshopCells::Where& a_where, const Key& a_key, const Item& a_item, std::uint32_t a_ref = 0) { waiting.Put(a_where, a_key, a_item, a_ref); }

		[[nodiscard]] const Waiting& Pending() const { return waiting; }
		[[nodiscard]] std::size_t    Queued() const { return queue.size(); }

		void Clear()
		{
			queue.clear();
			waiting.Clear();
		}

	private:
		std::deque<std::pair<Key, Item>> queue;
		Waiting                          waiting;
	};
}
