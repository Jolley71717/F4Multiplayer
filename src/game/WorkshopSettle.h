#pragma once

#include "game/WorkshopMove.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// A piece just placed or moved is looked at again a few times over the next seconds, and reported again
// if it stands somewhere else by then. The workshop tells of a placement or a move when the player puts
// the piece down, but the game can still set where it stands after that: a bed or a container a player
// placed turned up for their friend away from where it stood for them. Whatever the game does with it
// after the event, the friend's copy follows where it ends up.
//
// The engine is behind Engine (WorkshopSync.cpp's, or a test's stand-in). An Engine gives:
//
//   std::optional<WorkshopMove::Transform> TransformOf(std::uint32_t ref)   none once it's gone
//   bool Report(std::uint32_t ref, std::uint32_t workshop)                  reported again as placed
namespace WorkshopSettle
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// When a piece is looked at again, after it was reported: by then the game has long finished with it.
	inline constexpr std::array<Clock::duration, 4> AFTER{ 500ms, 2s, 5s, 15s };
	inline constexpr std::size_t                    MAX_WATCHED = 512;  // a blueprint placed at once: the oldest go unwatched

	class Watch
	{
	public:
		// The piece was reported standing there (placed or moved).
		void Reported(std::uint32_t a_ref, std::uint32_t a_workshop, const WorkshopMove::Transform& a_at, Clock::time_point a_now)
		{
			std::erase_if(watched, [&](const Entry& a_entry) { return a_entry.ref == a_ref; });
			if (watched.size() >= MAX_WATCHED) {
				watched.erase(watched.begin());
			}
			watched.push_back({ a_ref, a_workshop, a_at, a_now, 0 });
		}

		// Looks again at the pieces due, reporting again those that stand somewhere else now. Returns how
		// many were reported again.
		template <class Engine>
		std::size_t Check(Engine& a_engine, Clock::time_point a_now)
		{
			std::size_t again = 0;
			std::erase_if(watched, [&](Entry& a_entry) {
				if (a_now < a_entry.since + AFTER[a_entry.step]) {
					return false;
				}
				const auto now = a_engine.TransformOf(a_entry.ref);
				if (!now) {
					return true;  // scrapped or stored: its own report tells of that
				}
				if (!WorkshopMove::Same(*now, a_entry.at) && a_engine.Report(a_entry.ref, a_entry.workshop)) {
					++again;
					a_entry.at = *now;
					a_entry.since = a_now;  // it may not be done moving: watched as long again
					a_entry.step = 0;
					return false;
				}
				return ++a_entry.step == AFTER.size();
			});
			return again;
		}

		[[nodiscard]] std::size_t Watched() const { return watched.size(); }

		void Clear() { watched.clear(); }

	private:
		struct Entry
		{
			std::uint32_t           ref;
			std::uint32_t           workshop;
			WorkshopMove::Transform at;     // where it was reported standing
			Clock::time_point       since;  // when
			std::size_t             step;   // the next look (AFTER)
		};

		std::vector<Entry> watched;  // oldest first
	};
}
