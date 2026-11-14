#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// Friends' reports of their spawns' health that found no copy of ours to set (WorldSync::ApplyRemoteHealth): the spawn
// not heard of yet, its copy not placed yet, or not loaded. A joiner hears of a hurt friend's spawn's health in the world
// state, which the server sends just ahead of the spawns themselves (kSpawned), and places the copy some frames later; a
// game that runs a spawn reports its health only when it changes (HealthShare), so a report dropped then left the copy
// at full health until the next hit. So the last word on each waits here, by spawn id, and is set once the copy stands
// loaded (Retry). One that is not a friend's spawn alive here (over, gone, ours, or never heard of) goes once it has
// waited GRACE (a spawn not heard of yet is heard of well within it), and any once it has waited KEEP. A plugin's NPC
// waits in WorldSync's pendingHealth, by its own reference. Pure, so it is tested.
namespace SpawnHealths
{
	using Clock = std::chrono::steady_clock;

	constexpr auto        KEEP = std::chrono::seconds(60);
	constexpr auto        GRACE = std::chrono::seconds(5);
	constexpr std::size_t MAX = 256;

	class Book
	{
	public:
		// A report that found nothing to set: the latest is kept.
		void Heard(std::uint32_t a_id, float a_fraction, Clock::time_point a_now)
		{
			if (entries.size() >= MAX && !entries.contains(a_id)) {
				auto oldest = entries.begin();
				for (auto it = entries.begin(); it != entries.end(); ++it) {
					if (it->second.at < oldest->second.at) {
						oldest = it;
					}
				}
				entries.erase(oldest);
			}
			entries[a_id] = { a_fraction, a_now };
		}

		// A report set it: what waited is older.
		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		// Tries each: a_expects(id) whether it is a friend's spawn alive here (Spawns::Expects); a_apply(id, fraction) true
		// once set (asked only of one expected). Those set go, as do those not expected after GRACE and any after KEEP.
		// How many were set.
		template <class Expects, class Apply>
		std::size_t Retry(Clock::time_point a_now, Expects a_expects, Apply a_apply)
		{
			std::size_t set = 0;
			for (auto it = entries.begin(); it != entries.end();) {
				const auto waited = a_now - it->second.at;
				const bool expected = a_expects(it->first);
				if (waited >= KEEP || (!expected && waited >= GRACE)) {
					it = entries.erase(it);
				} else if (expected && a_apply(it->first, it->second.fraction)) {
					++set;
					it = entries.erase(it);
				} else {
					++it;
				}
			}
			return set;
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] bool        Empty() const { return entries.empty(); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		[[nodiscard]] bool        Contains(std::uint32_t a_id) const { return entries.contains(a_id); }

	private:
		struct Entry
		{
			float             fraction = 1.0f;
			Clock::time_point at{};
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
