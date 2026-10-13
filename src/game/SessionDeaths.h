#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// What our game knows of the session's deaths (WorldSync), and what NpcSync asks of it before offering to run
// an NPC. A raider the session has dead can be alive in our game in two ways: our game has not caught up yet
// (a save from before the kill was loaded, or we just connected, and the session's dead list is on its way),
// or the game brought it back (its cell reset after days away). The first must not be offered to run, or the
// server would take a stale copy for a living one; the second is the host's game's to bring back for everyone
// (Server.cpp, handleClaims). Pure, so it can be tested with the server.
namespace SessionDeaths
{
	using Clock = std::chrono::steady_clock;

	// The session's dead list follows every welcome and every request after a load at once, on the reliable
	// channel. If it never comes (the server refuses world states asked for too often), we carry on after this.
	constexpr auto SETTLE_FALLBACK = std::chrono::seconds(15);
	// A cell resets days of game time after it was left; an actor alive this soon after we learned of its death
	// is our kill still landing (or one we never managed to kill), not a respawn.
	constexpr auto RESPAWN_AFTER = std::chrono::seconds(10);

	class Ledger
	{
	public:
		// Learned dead: our own death event, another game's report, the session's list or found dead when we host.
		// True if it is news.
		bool Add(std::uint32_t a_id, Clock::time_point a_now) { return dead.try_emplace(a_id, Entry{ a_now }).second; }

		// Our game has it dead: its death event, or its corpse loaded or found. A console kill that left it standing (an
		// NPC essential in our game only: it goes down and gets up) is not this, so a later load of it alive is not
		// taken for a respawn.
		void SawDead(std::uint32_t a_id)
		{
			if (const auto it = dead.find(a_id); it != dead.end()) {
				it->second.seenDead = true;
			}
		}

		// No longer dead for us: a story character a friend's game killed away from them lives on in ours
		// (WorldSync::TryKill), so a kill of it here later is news again.
		void Forget(std::uint32_t a_id) { dead.erase(a_id); }

		[[nodiscard]] bool Contains(std::uint32_t a_id) const { return dead.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return dead.size(); }

		// A load or a lost connection: what we knew described another world. Nothing is known until the session's
		// world state comes again.
		void Reset(Clock::time_point a_now)
		{
			dead.clear();
			Expect(a_now);
		}

		// Welcomed (again): the session's world state is on its way.
		void Expect(Clock::time_point a_now)
		{
			settled = false;
			expectedAt = a_now;
		}

		// The session's world state was applied: what we know is the session's.
		void Settle() { settled = true; }

		[[nodiscard]] bool Settled(Clock::time_point a_now) const { return settled || a_now - expectedAt >= SETTLE_FALLBACK; }

		// An actor's 3D loaded. a_aliveHere: our game has it alive. a_killPending: its death waits for it to load
		// (we have not killed it here yet). True when it came back: seen dead here (SawDead), learned of a while ago,
		// and alive again, which only a cell reset does. It is then forgotten as dead, so our game may offer to run it again; a
		// found-dead report is never sent for it, since it is alive.
		bool OnLoaded(std::uint32_t a_id, bool a_aliveHere, bool a_killPending, Clock::time_point a_now)
		{
			const auto it = dead.find(a_id);
			if (!a_aliveHere || a_killPending || it == dead.end() || !it->second.seenDead || a_now - it->second.learned < RESPAWN_AFTER) {
				return false;
			}
			dead.erase(it);
			++revived;
			return true;
		}

		// Whether our game may offer to run this NPC: once the session's dead list is in after a reset (our game
		// may have it alive from a save older than its death), and never one known dead.
		[[nodiscard]] bool MayClaim(std::uint32_t a_id, Clock::time_point a_now) const { return Settled(a_now) && !dead.contains(a_id); }

		[[nodiscard]] std::uint32_t Revived() const { return revived; }

	private:
		struct Entry
		{
			Clock::time_point learned{};
			bool              seenDead = false;
		};

		std::unordered_map<std::uint32_t, Entry>             dead;
		Clock::time_point                                    expectedAt{};
		bool                                                 settled = false;
		std::uint32_t                                        revived = 0;
	};
}
