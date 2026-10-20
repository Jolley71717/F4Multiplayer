#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// The health of the NPCs our game runs, shared with the friends' copies of them whenever it changes, not only when our
// player hits them. A health report is the NPC's health as a fraction of its maximum (WorldSync's kReportHealth), and
// before, one went out only after a hit: what an NPC lost to anything else (the fire a Molotov leaves, a burn, poison,
// a bleed, an explosion's splash with no cause, a fall) stayed in the game that runs it. The friend's copy never lost it
// (CopyWatch gives a copy back what no hit of ours took), so it stood higher than the NPC, and the friend's next shot
// reported the copy's health less that shot: the NPC was healed back up by whatever the fire had taken, and the friend's
// shot seemed to do nothing. As players saw it, a Molotov moved the ability to deal damage from one of them to the other.
//
// So each frame our game reads the health of every loaded NPC it runs, and one that stands a STEP or more from what it
// last told the session is reported again, at most every THROTTLE for one NPC (a burn takes a little every few frames).
// The last report, whatever sent it (a hit's, or this), is what the friends have. A friend's report applied to our NPC
// is reported back once it settles, so a copy that heard an older report of ours after its own hit is put right.
// Pure, so it can be tested without the game.
namespace HealthShare
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration THROTTLE = std::chrono::milliseconds{ 250 };
	constexpr float           STEP = 0.005f;      // of the maximum: half a point of a 100-health raider
	constexpr std::size_t     MAX_TRACKED = 1024;  // NPCs watched at once (a guard)

	class Book
	{
	public:
		// The NPC our game runs reads a_fraction of its maximum now: whether to report it now (booked as sent if so).
		// The first reading of one is where it stands: nothing changed yet, nothing to tell.
		bool Due(std::uint32_t a_id, float a_fraction, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				if (entries.size() < MAX_TRACKED) {
					entries[a_id] = Entry{ .sent = a_fraction, .at = a_now - THROTTLE };
				}
				return false;
			}
			auto& entry = it->second;
			if (std::abs(a_fraction - entry.sent) < STEP || a_now - entry.at < THROTTLE) {
				return false;
			}
			entry.sent = a_fraction;
			entry.at = a_now;
			++shared;
			return true;
		}

		// A report of a_fraction for the NPC went out now some other way (after a hit of ours): what the friends have.
		void Sent(std::uint32_t a_id, float a_fraction, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second = Entry{ .sent = a_fraction, .at = a_now };
			} else if (entries.size() < MAX_TRACKED) {
				entries[a_id] = Entry{ .sent = a_fraction, .at = a_now };
			}
		}

		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		// Forgets every NPC a_keep says no to (no longer ours to run, dead, or gone).
		template <class Keep>
		void Prune(Keep a_keep)
		{
			std::erase_if(entries, [&](const auto& a_entry) { return !a_keep(a_entry.first); });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t   Size() const { return entries.size(); }
		[[nodiscard]] std::uint32_t Shared() const { return shared; }

	private:
		struct Entry
		{
			float             sent = 0.0f;  // the fraction last reported
			Clock::time_point at{};         // when
		};

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint32_t                            shared = 0;  // reports Due sent
	};
}
