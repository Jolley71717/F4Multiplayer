#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Our game's offers to run NPCs (NpcSync's claims), and when to make them again. A claim the server has not answered
// is not repeated for RETRY. A companion claim the server refused (the NPC is another player's companion too: two
// players who both recruited Dogmeat) was repeated every RETRY for as long as both kept him, each one an event the
// server handles and answers; it waits COMPANION_BACKOFF now. Only a refused companion claim backs off: an NPC we
// just recruited from someone who merely runs it is claimed at once, and so is one nobody runs. Pure, so it can be
// tested with the server.
namespace NpcClaims
{
	using Clock = std::chrono::steady_clock;
	using Reason = Protocol::ClaimReason;

	constexpr auto RETRY = std::chrono::seconds(3);
	constexpr auto COMPANION_BACKOFF = std::chrono::seconds(15);

	class Book
	{
	public:
		// Whether a claim for this reason may go now.
		[[nodiscard]] bool Due(std::uint32_t a_id, Reason a_reason, Clock::time_point a_now) const
		{
			if (const auto it = claims.find(a_id); it != claims.end() && a_now - it->second.at < RETRY) {
				return false;
			}
			if (const auto it = refused.find(a_id); a_reason == Reason::kCompanion && it != refused.end() && a_now - it->second < COMPANION_BACKOFF) {
				return false;
			}
			return true;
		}

		void Claimed(std::uint32_t a_id, Reason a_reason, Clock::time_point a_now) { claims[a_id] = { a_now, a_reason }; }

		// NpcSync::Scan's decision for one loaded, living NPC: the claim to send for it now, if any, booked as sent.
		// a_owner: who the server says runs it (0: nobody). a_companion: it follows our player. a_mayClaim: the
		// session's deaths allow it (WorldSync::MayClaim: never one the session has dead, nothing until its dead list
		// is in after a load). Not one we run, nor one another player runs unless it is our companion, nor while a claim
		// awaits its answer or a refused companion claim backs off.
		[[nodiscard]] std::optional<Reason> Offer(std::uint32_t a_id, std::uint32_t a_owner, std::uint32_t a_localId, bool a_companion, bool a_mayClaim, Clock::time_point a_now)
		{
			const auto reason = a_companion ? Reason::kCompanion : Reason::kUnowned;
			if ((a_owner != 0 && a_owner == a_localId) || (a_owner != 0 && !a_companion) || !a_mayClaim || !Due(a_id, reason, a_now)) {
				return std::nullopt;
			}
			Claimed(a_id, reason, a_now);
			return reason;
		}

		// The server named the NPC's owner (0: nobody), in answer to a claim or to everyone.
		void Answered(std::uint32_t a_id, std::uint32_t a_owner, std::uint32_t a_localId, Clock::time_point a_now)
		{
			const auto it = claims.find(a_id);
			const bool companionClaim = it != claims.end() && it->second.reason == Reason::kCompanion;
			if (it != claims.end()) {
				claims.erase(it);
			}
			if (a_owner == 0 || a_owner == a_localId) {
				refused.erase(a_id);  // free, or ours: nothing to wait for
			} else if (companionClaim) {
				refused[a_id] = a_now;
				++refusals;
			}
		}

		// Drops what no longer holds anything back.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(claims, [&](const auto& a_entry) { return a_now - a_entry.second.at >= RETRY; });
			std::erase_if(refused, [&](const auto& a_entry) { return a_now - a_entry.second >= COMPANION_BACKOFF; });
		}

		void Clear()
		{
			claims.clear();
			refused.clear();
		}

		[[nodiscard]] std::uint32_t Refusals() const { return refusals; }

	private:
		struct Claim
		{
			Clock::time_point at{};
			Reason            reason = Reason::kUnowned;
		};

		std::unordered_map<std::uint32_t, Claim>             claims;   // awaiting an answer
		std::unordered_map<std::uint32_t, Clock::time_point> refused;  // companion claims refused, and when
		std::uint32_t                                        refusals = 0;
	};
}
