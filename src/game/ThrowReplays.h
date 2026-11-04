#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

// A friend's throw is replayed on the actor that stands for the thrower in our game (their stand-in, or our copy of an
// NPC their game runs) with the engine's own throw action (WeaponFire::PlayAction). The actor's graph then plays the
// whole throw, and at its release ("WeaponFire" with payload "2") the engine launches the grenade the actor holds: a
// real one in our world, landing beside the actor on top of the owner's explosion, which comes as its own report. A
// copy of an NPC holds a grenade of its own, so its replayed throw throws one; the ghost watch that removes what the
// actor shot for a moment after (WeaponFire) caught a stand-in's, but in the live test it removed nothing of a host
// raider's copy that replayed the raider's throw in the two seconds it watched: whether that grenade left the hand later
// or not at all, nothing would have kept one from going off in the friend's game.
//
// So the release that follows a replayed throw is kept from the actor: WeaponFire's hook on NPC animation events does
// not pass it on to the engine, the throw plays, and nothing leaves the hand. This is the bookkeeping, pure so it can
// be tested without the game: each replay arms the actor, and its releases within the window are the ones kept, one
// per replay (a second throw replayed before the first one's release keeps the next one too).
namespace ThrowReplays
{
	using Clock = std::chrono::steady_clock;

	// How long after the replay the actor's release is waited for: the wind-up can wait on what the actor was doing
	// (a reload, a stagger, an attack still playing).
	constexpr Clock::duration WINDOW = std::chrono::seconds{ 5 };
	constexpr std::size_t     MAX_ARMED = 256;     // actors waited on at once (a guard)
	constexpr std::size_t     MAX_PER_ACTOR = 4;  // replays waited on for one actor (a guard)

	class Book
	{
	public:
		// A friend's throw is replayed on the actor now.
		void Arm(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (entries.size() >= MAX_ARMED && !entries.contains(a_id)) {
				Expire(a_now);
				if (entries.size() >= MAX_ARMED) {
					return;
				}
			}
			auto& replays = entries[a_id];
			if (replays.size() >= MAX_PER_ACTOR) {
				replays.pop_front();
			}
			replays.push_back(a_now);
		}

		// The actor's graph lets go of a throw: if it is a replayed throw's release, which is kept from the engine, how
		// long after that replay it came (the earliest one still waited for). A release with no replay waiting within
		// the window is the actor's own.
		std::optional<Clock::duration> Release(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return std::nullopt;
			}
			auto& replays = it->second;
			while (!replays.empty() && a_now - replays.front() > WINDOW) {
				replays.pop_front();
			}
			std::optional<Clock::duration> after;
			if (!replays.empty()) {
				after = a_now - replays.front();
				replays.pop_front();
			}
			if (replays.empty()) {
				entries.erase(it);
			}
			return after;
		}

		// Whether a replayed throw on the actor has not been let go of yet (within the window or not).
		[[nodiscard]] bool Unreleased(std::uint32_t a_id) const { return entries.contains(a_id); }

		// Whether a replayed throw on the actor, not let go of yet, was replayed less than a_within ago: its wind-up may
		// still be playing.
		[[nodiscard]] bool WindingUp(std::uint32_t a_id, Clock::time_point a_now, Clock::duration a_within) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() && std::ranges::any_of(it->second, [&](Clock::time_point a_at) { return a_now - a_at < a_within; });
		}

		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		void Expire(Clock::time_point a_now)
		{
			std::erase_if(entries, [&](auto& a_entry) {
				auto& replays = a_entry.second;
				while (!replays.empty() && a_now - replays.front() > WINDOW) {
					replays.pop_front();
				}
				return replays.empty();
			});
		}

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::unordered_map<std::uint32_t, std::deque<Clock::time_point>> entries;  // actor -> when its throws were replayed
	};
}
