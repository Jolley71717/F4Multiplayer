#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// The session's container changes as a client keeps them (WorldSync). The server numbers them; everything
// before Next() is in our world already or waiting here, because its container isn't loaded. Changes to
// one container are applied in order; different containers don't wait for each other. Pure bookkeeping,
// so it can be tested without the game: applying a change is the caller's (TryApply returns false to try
// again later).
class PendingContainers
{
public:
	struct Entry
	{
		std::uint32_t             index = 0;
		Protocol::ContainerChange change;

		friend bool operator==(const Entry& a_lhs, const Entry& a_rhs)
		{
			return a_lhs.index == a_rhs.index && a_lhs.change.container == a_rhs.change.container && a_lhs.change.item == a_rhs.change.item &&
			       a_lhs.change.count == a_rhs.change.count;
		}
	};

	[[nodiscard]] std::uint32_t Next() const { return next; }
	[[nodiscard]] std::size_t   Size() const { return count; }
	[[nodiscard]] bool          Empty() const { return count == 0; }
	[[nodiscard]] bool          Waiting(std::uint32_t a_container) const { return byContainer.contains(a_container); }

	// A change reached us (live or in a world state). a_alreadyInWorld: ours, made before the save we
	// loaded, so our world has it. One we already have (index before Next()) is ignored.
	template <class TryApply>
	void Offer(std::uint32_t a_index, const Protocol::ContainerChange& a_change, bool a_alreadyInWorld, TryApply&& a_tryApply)
	{
		if (a_index < next) {
			return;
		}
		next = a_index + 1;
		if (a_alreadyInWorld || !Protocol::IsValid(a_change)) {
			return;
		}
		// Behind another change to the same container: after it, whatever its container's state.
		if (Waiting(a_change.container) || !a_tryApply(a_change)) {
			byContainer[a_change.container].push_back({ a_index, a_change });
			++count;
		}
	}

	// Applies what waits for a_container, in order, until one can't be. Returns how many were applied.
	template <class TryApply>
	std::size_t Retry(std::uint32_t a_container, TryApply&& a_tryApply)
	{
		const auto it = byContainer.find(a_container);
		if (it == byContainer.end()) {
			return 0;
		}
		std::size_t applied = 0;
		auto&       queue = it->second;
		while (!queue.empty() && a_tryApply(queue.front().change)) {
			queue.pop_front();
			++applied;
		}
		count -= applied;
		if (queue.empty()) {
			byContainer.erase(it);
		}
		return applied;
	}

	// Every container that has changes waiting (for a full pass, or a sweep cursor).
	[[nodiscard]] std::vector<std::uint32_t> Containers() const
	{
		std::vector<std::uint32_t> containers;
		containers.reserve(byContainer.size());
		for (const auto& [container, queue] : byContainer) {
			containers.push_back(container);
		}
		return containers;
	}

	// What waits, in session order: what a save keeps (the co-save record's layout is unchanged).
	[[nodiscard]] std::vector<Entry> Sorted() const
	{
		std::vector<Entry> entries;
		entries.reserve(count);
		for (const auto& [container, queue] : byContainer) {
			entries.insert(entries.end(), queue.begin(), queue.end());
		}
		std::ranges::sort(entries, {}, &Entry::index);
		return entries;
	}

	// What a save kept (in any order: each container's changes are put back in session order).
	void Load(std::uint32_t a_next, std::vector<Entry> a_entries)
	{
		Clear();
		next = a_next;
		std::ranges::sort(a_entries, {}, &Entry::index);
		for (const auto& entry : a_entries) {
			byContainer[entry.change.container].push_back(entry);
		}
		count = a_entries.size();
	}

	// A new session (or none): nothing of it is in our world.
	void Clear()
	{
		next = 0;
		byContainer.clear();
		count = 0;
	}

private:
	std::uint32_t                                         next = 0;
	std::unordered_map<std::uint32_t, std::deque<Entry>> byContainer;
	std::size_t                                           count = 0;
};

// A slow sweep over waiting entries that touches at most a few per pass, carrying on where the last pass
// stopped. Entries are named by a key ordered with <; entries that come and go between passes don't make
// it skip one that stays: the next pass starts after the last key visited, not at a position.
template <class Key>
class RetryCursor
{
public:
	explicit RetryCursor(std::size_t a_perPass) :
		perPass(a_perPass) {}

	// The keys this pass visits, from a_keys (any order): at most perPass, from after the last key the
	// previous pass visited, wrapping round to the start.
	[[nodiscard]] std::vector<Key> Pass(std::vector<Key> a_keys)
	{
		std::ranges::sort(a_keys);
		a_keys.erase(std::unique(a_keys.begin(), a_keys.end()), a_keys.end());
		auto start = last ? std::ranges::upper_bound(a_keys, *last) : a_keys.begin();
		std::vector<Key> visit;
		for (std::size_t i = 0; i < a_keys.size() && visit.size() < perPass; ++i) {
			if (start == a_keys.end()) {
				start = a_keys.begin();
			}
			visit.push_back(*start++);
		}
		if (!visit.empty()) {
			last = visit.back();
		}
		return visit;
	}

private:
	std::size_t        perPass;
	std::optional<Key> last;
};

// How full the session's shared history looks from what our game was told (dead NPCs, picked-up items,
// container changes), in percent of what a session can hold: each of those stops being shared once its
// table is full.
[[nodiscard]] inline std::size_t SharedHistoryPercent(std::size_t a_dead, std::size_t a_pickups, std::uint32_t a_containerNext)
{
	const auto most = (std::max)({ a_dead, a_pickups, static_cast<std::size_t>(a_containerNext) });
	return most * 100 / Protocol::MAX_WORLD_STATE_ACTORS;
}
