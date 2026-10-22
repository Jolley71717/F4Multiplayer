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
//
// The first reading of an NPC our game has just come to run (a takeover, a claim of one nobody ran, our game back from
// a load) is reported too. In 0.9.0 it was taken as where the friends stood, and they did not: a player who took an NPC
// over with a hit sends the claim at the end of the frame and the hit's damage at the start of the next
// (Session::Frame), so the server gave them the NPC first, and the game that had run it dropped the damage as no longer
// its to deal; and a host back from a death's reload ran the NPCs at its save's health while the friends' copies kept
// the session's. Each friend's next shot then came off health their copy did not show, and the owner's next report took
// it back: as the players saw it, their shots healed the NPC. At most FIRST_PER_THROTTLE such reports go in a THROTTLE
// (a load brings many NPCs at once); the rest are read again the next frame. An NPC out of sight for a moment (its
// process level changed, a frame without 3D) is remembered for FORGET_AFTER, so it is not taken for a new one.
// Pure, so it can be tested without the game.
namespace HealthShare
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration THROTTLE = std::chrono::milliseconds{ 250 };
	constexpr float           STEP = 0.005f;      // of the maximum: half a point of a 100-health raider
	constexpr std::size_t     MAX_TRACKED = 1024;  // NPCs watched at once (a guard)
	constexpr std::uint32_t   FIRST_PER_THROTTLE = 16;  // first readings reported in a THROTTLE, at most
	constexpr Clock::duration FORGET_AFTER = std::chrono::seconds{ 10 };

	class Book
	{
	public:
		// The NPC our game runs reads a_fraction of its maximum now: whether to report it now (booked as sent if so).
		// The first reading of one is reported (see above) when the budget for those allows; else it is read again.
		bool Due(std::uint32_t a_id, float a_fraction, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				if (entries.size() >= MAX_TRACKED) {
					return false;
				}
				if (a_now - firstWindow >= THROTTLE) {
					firstWindow = a_now;
					firstInWindow = 0;
				}
				if (firstInWindow >= FIRST_PER_THROTTLE) {
					return false;
				}
				++firstInWindow;
				entries[a_id] = Entry{ .sent = a_fraction, .at = a_now, .seen = a_now };
				++shared;
				++firsts;
				return true;
			}
			auto& entry = it->second;
			entry.seen = a_now;
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
				it->second = Entry{ .sent = a_fraction, .at = a_now, .seen = a_now };
			} else if (entries.size() < MAX_TRACKED) {
				entries[a_id] = Entry{ .sent = a_fraction, .at = a_now, .seen = a_now };
			}
		}

		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		// Forgets every NPC a_keep says no to (no longer ours to run, dead, or gone) that went unread for FORGET_AFTER.
		template <class Keep>
		void Prune(Keep a_keep, Clock::time_point a_now)
		{
			std::erase_if(entries, [&](const auto& a_entry) { return !a_keep(a_entry.first) && a_now - a_entry.second.seen >= FORGET_AFTER; });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t   Size() const { return entries.size(); }
		[[nodiscard]] std::uint32_t Shared() const { return shared; }
		[[nodiscard]] std::uint32_t Firsts() const { return firsts; }

	private:
		struct Entry
		{
			float             sent = 0.0f;  // the fraction last reported
			Clock::time_point at{};         // when
			Clock::time_point seen{};       // when last read (Due) or reported (Sent)
		};

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint32_t                            shared = 0;  // reports Due sent
		std::uint32_t                            firsts = 0;  // ... of them first readings
		Clock::time_point                        firstWindow{};
		std::uint32_t                            firstInWindow = 0;
	};
}
