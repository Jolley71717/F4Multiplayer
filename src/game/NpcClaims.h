#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

// Our game's offers to run NPCs (NpcSync's claims), and when to make them again. A claim the server has not answered
// is not repeated for RETRY. A companion claim the server refused (the NPC is another player's companion too: two
// players who both recruited Dogmeat) was repeated every RETRY for as long as both kept him, each one an event the
// server handles and answers; it waits COMPANION_BACKOFF now. Only a refused companion claim backs off: an NPC we
// just recruited from someone who merely runs it is claimed at once, and so is one nobody runs.
//
// And what our player's hit on an NPC asks for (OnHit, NpcSync::OnLocalInteraction: a talk, a press and the blow that
// killed it ask the same). One we run is claimed again, HIT_KEEP_INTERVAL apart at most, so a friend's hit doesn't take
// it over while we keep hitting it (the server refuses takeovers within its cooldown of the runner's claim): two players
// shooting one raider used to trade it every 5 s, its AI starting over in the taker's game each time. A plugin's NPC
// another game runs is asked for, TAKEOVER_COOLDOWN apart at most. A spawn (an NPC a friend's game made at runtime,
// Protocol::IsSpawnRef) is never ours to run: what we hit is a copy placed from its template, no NPC of their game, and
// it is in no save but theirs; the server leaves it with the game that made it for good. The claim still goes, as often
// as a takeover would: it notes us fighting it, so our kill of it is believed (a grenade's blows name no attacker, and
// then no damage report said so). Our hits reach it as damage meanwhile, as any non-runner's do. Pure, so it can be
// tested with the server.
namespace NpcClaims
{
	using Clock = std::chrono::steady_clock;
	using Reason = Protocol::ClaimReason;

	constexpr auto RETRY = std::chrono::seconds(3);
	constexpr auto COMPANION_BACKOFF = std::chrono::seconds(15);
	constexpr auto TAKEOVER_COOLDOWN = std::chrono::seconds(5);  // between takeover requests for the same NPC (the server's Options::takeoverCooldown)
	constexpr auto HIT_KEEP_INTERVAL = std::chrono::seconds(2);  // between the claims our hits on one we run make

	// What a hit asks for.
	enum class Hit
	{
		kNone,      // nothing: nobody runs it (Scan offers it), we're not in a session, or it was asked lately
		kKeep,      // one we run: claimed again, so it stays ours while we hit it
		kTakeover,  // a plugin's NPC another game runs: ours to run from this hit, unless the server refuses
		kFight,     // a spawn another game made: theirs to run for good; the claim notes us fighting it
	};

	struct HitClaim
	{
		Hit  hit = Hit::kNone;
		bool first = false;  // kFight: our first hit on this spawn (NpcSync logs once why the copy stays theirs)
	};

	class Book
	{
	public:
		// Our player's hit on a_id, which a_owner's game runs (0: nobody's), at a_now: the claim to send, if any, booked.
		[[nodiscard]] HitClaim OnHit(std::uint32_t a_id, std::uint32_t a_owner, std::uint32_t a_localId, Clock::time_point a_now)
		{
			if (a_owner == 0 || a_localId == 0) {
				return {};
			}
			if (a_owner == a_localId) {
				if (const auto it = keeps.find(a_id); it != keeps.end() && a_now - it->second < HIT_KEEP_INTERVAL) {
					return {};
				}
				keeps[a_id] = a_now;
				return { Hit::kKeep };
			}
			if (const auto it = asked.find(a_id); it != asked.end() && a_now - it->second < TAKEOVER_COOLDOWN) {
				return {};
			}
			asked[a_id] = a_now;
			Claimed(a_id, Reason::kInteract, a_now);
			if (!Protocol::IsSpawnRef(a_id)) {
				return { Hit::kTakeover };
			}
			const bool first = foughtSpawns.insert(a_id).second;
			spawnFights += first;
			return { Hit::kFight, first };
		}

		// Whether a hit asked to take a_id over within TAKEOVER_COOLDOWN (it waits that out before anything asks again).
		[[nodiscard]] bool TakeoverPending(std::uint32_t a_id) const { return asked.contains(a_id); }

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
			if (a_owner == 0) {
				foughtSpawns.erase(a_id);  // a spawn over (nobody's now): its id may name another later, said again then
			}
		}

		// Drops what no longer holds anything back.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(claims, [&](const auto& a_entry) { return a_now - a_entry.second.at >= RETRY; });
			std::erase_if(refused, [&](const auto& a_entry) { return a_now - a_entry.second >= COMPANION_BACKOFF; });
			std::erase_if(asked, [&](const auto& a_entry) { return a_now - a_entry.second >= TAKEOVER_COOLDOWN; });
			std::erase_if(keeps, [&](const auto& a_entry) { return a_now - a_entry.second >= HIT_KEEP_INTERVAL; });
		}

		void Clear()
		{
			claims.clear();
			refused.clear();
			asked.clear();
			keeps.clear();
			foughtSpawns.clear();
		}

		[[nodiscard]] std::uint32_t Refusals() const { return refusals; }
		// Spawns of friends' our player hit (each once, however many hits): kept theirs, every one.
		[[nodiscard]] std::uint32_t SpawnFights() const { return spawnFights; }

	private:
		struct Claim
		{
			Clock::time_point at{};
			Reason            reason = Reason::kUnowned;
		};

		std::unordered_map<std::uint32_t, Claim>             claims;        // awaiting an answer
		std::unordered_map<std::uint32_t, Clock::time_point> refused;       // companion claims refused, and when
		std::unordered_map<std::uint32_t, Clock::time_point> asked;         // NPCs others run our hits asked for, and when
		std::unordered_map<std::uint32_t, Clock::time_point> keeps;         // NPCs we run, claimed again with our hit, and when
		std::unordered_set<std::uint32_t>                    foughtSpawns;  // friends' spawns our hits noted us fighting
		std::uint32_t                                        refusals = 0;
		std::uint32_t                                        spawnFights = 0;
	};
}
