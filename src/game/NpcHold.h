#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// When our game keeps running an NPC it would otherwise give up (NpcSync::Scan). One our player hit or talked to within
// FOUGHT is not handed to a nearer friend's game (ShouldHandOff): the fight is here, and our killing blow would land on
// a copy. One we run that starts dying is kept until its death is reported, for at most DYING: the death event comes
// seconds after the killing blow (the fall), and releasing it at once let a friend's game, where it was still alive,
// take it over, so our report of its death came from a game that no longer ran it and was dropped (it lived on in the
// friend's game). The server ends its ownership when the death comes (Server.cpp, handleDeath). Pure, so it can be
// tested with the server.
namespace NpcHold
{
	using Clock = std::chrono::steady_clock;

	constexpr auto FOUGHT = std::chrono::seconds(10);  // as long as the server believes a fighter's kill (Options::deathGrace)
	constexpr auto DYING = std::chrono::seconds(15);   // the longest fall seen was 6 s
	constexpr std::size_t MAX_FOUGHT = 256;            // NPCs remembered (a big fight is a dozen)

	class Book
	{
	public:
		// Our player hit or talked to the NPC (NpcSync::OnLocalInteraction).
		void Fought(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (fought.size() >= MAX_FOUGHT && !fought.contains(a_id)) {
				Expire(a_now);
				if (fought.size() >= MAX_FOUGHT) {
					return;
				}
			}
			fought[a_id] = a_now;
		}

		[[nodiscard]] bool FoughtLately(std::uint32_t a_id, Clock::time_point a_now) const
		{
			const auto it = fought.find(a_id);
			return it != fought.end() && a_now - it->second < FOUGHT;
		}

		// Scan's decision for an NPC we run that is no longer one to run (not loaded, dead, disabled): true to keep
		// running it. a_dying: it is still here, dead or dying, not unloaded, disabled or deleted.
		[[nodiscard]] bool Keep(std::uint32_t a_id, bool a_dying, Clock::time_point a_now)
		{
			if (!a_dying) {
				dying.erase(a_id);
				return false;
			}
			const auto [it, added] = dying.try_emplace(a_id, a_now);
			return a_now - it->second < DYING;
		}

		// We no longer run it: released, or the server named another runner or none (its death ends it).
		void Forget(std::uint32_t a_id) { dying.erase(a_id); }

		// Drops fights long over.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(fought, [&](const auto& a_entry) { return a_now - a_entry.second >= FOUGHT; });
		}

		void Clear()
		{
			fought.clear();
			dying.clear();
		}

		[[nodiscard]] std::size_t Dying() const { return dying.size(); }

	private:
		std::unordered_map<std::uint32_t, Clock::time_point> fought;  // our player's last hit or word
		std::unordered_map<std::uint32_t, Clock::time_point> dying;   // NPCs we run, kept since they fell
	};

	// WorldSync::ReportFalling's decision: true to report the death of an NPC the moment it starts to fall, not when the
	// death event comes seconds later (the friends' copies stood on, fighting, all the while). Only one our game runs
	// (the server believes its runner without asking who fought it), and not one that goes down and gets up again. The
	// event that follows adds nothing: the death is known by then.
	[[nodiscard]] constexpr bool ReportAtFall(bool a_ours, bool a_falling, bool a_getsUp)
	{
		return a_ours && a_falling && !a_getsUp;
	}

	// WorldSync::CheckFoundDead's decision for an NPC dead in our world that the session does not know is dead: true
	// to leave it to its death event for now, as a death seen happen (our kill, if it is). a_falling: it is still
	// falling (dying, not yet dead). a_foughtLately: our player hit it within FOUGHT. a_waits keeps when each began to
	// wait: after DYING it is found dead after all (its death event never came). A found-dead report is believed only
	// from the host's game and only of an NPC it never had alive (Server.cpp, handleDeath), so a kill of ours reported
	// as one is lost.
	[[nodiscard]] inline bool LeaveToEvent(Book& a_waits, std::uint32_t a_id, bool a_falling, bool a_foughtLately, Clock::time_point a_now)
	{
		return (a_falling || a_foughtLately) && a_waits.Keep(a_id, true, a_now);
	}
}
