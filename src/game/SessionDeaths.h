#pragma once

#include "Protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

// What our game knows of the session's deaths (WorldSync), and what NpcSync asks of it before offering to run
// an NPC. A raider the session has dead can be alive in our game in two ways: our game has not caught up yet
// (a save from before the kill was loaded, or we just connected, and the session's dead list is on its way),
// or the game brought it back (its cell reset after days away). The first must not be offered to run, or the
// server would take a stale copy for a living one; the second is the host's game's to bring back for everyone
// (Server.cpp, handleClaims). Pure, so it can be tested with the server.
namespace SessionDeaths
{
	using Clock = std::chrono::steady_clock;

	// The engine's ACTOR_LIFE_STATE values that matter here.
	inline constexpr std::int32_t LIFE_DYING = 1;
	inline constexpr std::int32_t LIFE_DEAD = 2;
	inline constexpr std::int32_t LIFE_UNCONSCIOUS = 3;
	inline constexpr std::int32_t LIFE_RESTRAINED = 6;
	inline constexpr std::int32_t LIFE_ESSENTIAL_DOWN = 7;
	inline constexpr std::int32_t LIFE_BLEEDOUT = 8;

	// Whether an NPC the game calls dead (a death event, or its IsDead, which answers true for one that is down too)
	// only went down and gets up again. Dogmeat, essential, was taken for dead as he went down, saved dead in the
	// session, and went unrun and dead in everyone else's game. Down (essential down, bleeding out,
	// unconscious, restrained) gets up; so does an essential one that is not dying or dead. An essential one really
	// dying was killed by a quest's script, which kills even those: that death is real.
	constexpr bool GetsUp(std::int32_t a_lifeState, bool a_essential)
	{
		const bool down = a_lifeState == LIFE_ESSENTIAL_DOWN || a_lifeState == LIFE_BLEEDOUT || a_lifeState == LIFE_UNCONSCIOUS || a_lifeState == LIFE_RESTRAINED;
		return down || (a_essential && a_lifeState != LIFE_DYING && a_lifeState != LIFE_DEAD);
	}

	// The session's dead list follows every welcome and every request after a load at once, on the reliable
	// channel. If it never comes (the server refuses world states asked for too often), we carry on after this.
	constexpr auto SETTLE_FALLBACK = std::chrono::seconds(15);
	// A cell resets days of game time after it was left; an actor alive this soon after we learned of its death
	// is our kill still landing (or one we never managed to kill), not a respawn.
	constexpr auto RESPAWN_AFTER = std::chrono::seconds(10);

	class Ledger
	{
	public:
		// Learned dead: our own death event, another game's report (a_from: that player), the session's list or found
		// dead when we host. True if it is news.
		bool Add(std::uint32_t a_id, Clock::time_point a_now, std::uint32_t a_from = 0) { return dead.try_emplace(a_id, Entry{ a_now, false, a_from }).second; }

		// Whether a repeat of a death from a_player says where the corpse came to rest (Protocol::CORPSE_REST_WINDOW):
		// the death was news to us from that player's report, a moment ago.
		[[nodiscard]] bool RestsFrom(std::uint32_t a_id, std::uint32_t a_player, Clock::time_point a_now) const
		{
			const auto it = dead.find(a_id);
			return it != dead.end() && a_player != 0 && it->second.from == a_player && a_now - it->second.learned < Protocol::CORPSE_REST_WINDOW;
		}

		// Our report of it went out as it began to fall (WorldSync::ReportFalling), with the corpse at a_at; its death
		// event, at the end of the fall, takes that back (TakeFell) to tell whether it came to rest elsewhere.
		void FellReported(std::uint32_t a_id, const std::array<float, 3>& a_at)
		{
			if (const auto it = dead.find(a_id); it != dead.end()) {
				it->second.fellAt = a_at;
			}
		}

		[[nodiscard]] std::optional<std::array<float, 3>> TakeFell(std::uint32_t a_id)
		{
			const auto it = dead.find(a_id);
			return it == dead.end() ? std::nullopt : std::exchange(it->second.fellAt, std::nullopt);
		}

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
		// may have it alive from a save older than its death), and never one known dead, unless our game keeps it
		// essential (a_essentialHere): then its death was never applied here (WorldSync::TryKill holds it), and the
		// claim, marked essential, is how the host's game undoes a death an older build saved by mistake (Server's
		// handleClaims; it ignores the claim for any other death).
		[[nodiscard]] bool MayClaim(std::uint32_t a_id, Clock::time_point a_now, bool a_essentialHere = false) const
		{
			return Settled(a_now) && (a_essentialHere || !dead.contains(a_id));
		}

		[[nodiscard]] std::uint32_t Revived() const { return revived; }

	private:
		struct Entry
		{
			Clock::time_point                   learned{};
			bool                                seenDead = false;
			std::uint32_t                       from = 0;  // the player whose report told us, or 0
			std::optional<std::array<float, 3>> fellAt;    // FellReported
		};

		std::unordered_map<std::uint32_t, Entry>             dead;
		Clock::time_point                                    expectedAt{};
		bool                                                 settled = false;
		std::uint32_t                                        revived = 0;
	};
}
