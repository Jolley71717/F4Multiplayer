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

	// WorldSync::ReportFalling's decision, on the engine's dying event (as the fall begins): true to report the death of
	// an NPC then, not when the death event comes seconds later (the friends' copies stood on, fighting, all the while).
	// Only one our game runs (the server believes its runner without asking who fought it), and not one that goes down
	// and gets up again. a_killerNamed: the dying event names who killed it, so the report's kill credit is the
	// engine's, as the death event's would be. a_friendsBlow: a friend's blow took the last of its health here (their
	// damage names no attacker), so the kill is theirs and the report names them (CreditOf). With neither (a fall, a
	// script) it is left to the death event as before: no guessing. Before the protocol version that added killerId a
	// friend's blow was left to the death event too, as only the friend's own report could carry their kill and ours,
	// early, would beat it to the server: a robot's fall is ten seconds long, their report came too late to be
	// believed, and nobody had the kill.
	[[nodiscard]] constexpr bool ReportAtFall(bool a_ours, bool a_killerNamed, bool a_friendsBlow, bool a_getsUp)
	{
		return a_ours && (a_killerNamed || a_friendsBlow) && !a_getsUp;
	}

	// Whose kill a death our game reports is, as the report says it (Protocol::ActorDeath's killed and killerId).
	struct Credit
	{
		bool          killed = false;
		std::uint32_t killerId = 0;

		bool operator==(const Credit&) const = default;
	};

	// The friend's whose blow took the last of its health here (a_friend: their player id, 0 for none), else our player's
	// when the engine names them (a_byUs). The friend's blow comes first: their damage names no attacker, so the engine
	// names whoever hit the NPC before it, which is our player whenever both fought it. Never both: the wire refuses it.
	[[nodiscard]] constexpr Credit CreditOf(bool a_byUs, std::uint32_t a_friend)
	{
		return { a_friend == 0 && a_byUs, a_friend };
	}

	// Whether the blow that took an NPC's health from a_before to a_after is the one that felled it: it took the last of
	// it. Not a blow on one with none left (the engine marks an NPC dead at its next update, so a second hit of the same
	// frame lands on one at 0 or below: the kill is the first blow's, not that one's), and not one it still stands after.
	[[nodiscard]] constexpr bool FellBy(float a_before, float a_after)
	{
		return a_before > 0.0f && a_after <= 0.0f;
	}

	// The NPCs our game runs whose last health a friend's damage took here (WorldSync's ApplyRemoteDamage), when and
	// whose, by our game's own id (a spawn's too): the kill is that friend's, and our report of the death names them
	// (CreditOf, Protocol::ActorDeath::killerId). The first blow that fells an NPC (FellBy) stands for KEEP: a later hit
	// never replaces it, a friend's or anyone's, so an overkill does not take the kill, and a friend's hit on an NPC our
	// own player's blow already took to nothing is not their kill. Damage with no player named (a_from 0: the bot's, a
	// script's) is nobody's blow. An entry no fall took (an essential or protected NPC kneels, the NPC unloads) counts
	// for nothing after KEEP, which is as long as the server takes a player to have been fighting an NPC
	// (Server::Options::deathGrace): a death later than that is not that blow's.
	class FriendsBlows
	{
	public:
		static constexpr std::size_t MAX = 256;
		static constexpr auto        KEEP = std::chrono::seconds(10);

		// Damage of player a_from's applied to an NPC we run, its health before and after. True: it is kept as the blow
		// that felled it.
		bool Took(std::uint32_t a_local, std::uint32_t a_from, float a_before, float a_after, Clock::time_point a_now)
		{
			if (a_from == 0 || !FellBy(a_before, a_after)) {
				return false;
			}
			std::erase_if(blows, [&](const auto& a_entry) { return a_now - a_entry.second.at > KEEP; });
			if (blows.size() >= MAX) {
				return false;  // its death is then the engine's naming, as before
			}
			return blows.try_emplace(a_local, Blow{ a_now, a_from }).second;
		}

		// The friend whose damage felled this NPC here, if lately (0: none): its death is their kill.
		[[nodiscard]] std::uint32_t On(std::uint32_t a_local, Clock::time_point a_now) const
		{
			const auto it = blows.find(a_local);
			return it != blows.end() && a_now - it->second.at <= KEEP ? it->second.from : 0;
		}

		void Forget(std::uint32_t a_local) { blows.erase(a_local); }
		void Clear() { blows.clear(); }
		[[nodiscard]] std::size_t Size() const { return blows.size(); }

	private:
		struct Blow
		{
			Clock::time_point at{};
			std::uint32_t     from = 0;  // the player whose damage it was
		};
		std::unordered_map<std::uint32_t, Blow> blows;
	};

	// The same decision for one of our own spawns (WorldSync::ReportRuntimeFalling): as ReportAtFall, and never one our
	// game killed for a friend's report (a_killedHere: their kill of their copy, reported by them already).
	[[nodiscard]] constexpr bool ReportSpawnAtFall(bool a_ours, bool a_killedHere, bool a_killerNamed, bool a_friendsBlow, bool a_getsUp)
	{
		return !a_killedHere && ReportAtFall(a_ours, a_killerNamed, a_friendsBlow, a_getsUp);
	}

	// WorldSync::ReportRuntimeDeaths' decision for our copy of a friend's spawn that died here before its owner's game
	// said so: true to report the death under its spawn id (our kill of it, or one of an actor our game runs: the death
	// event names them). Not one whose last health a report of its owner's game took here (a_byOwnersWord) with nobody
	// named by the death event: it has no health there, so it is dying there, and its owner's game reports it with the
	// kill it saw. Ours, sent first, was a death from a player who never fought it, which the server dropped with a log
	// line each time; from one who had fought it, the server took it in place of its owner's.
	[[nodiscard]] constexpr bool ReportCopyDeath(bool a_byOwnersWord, bool a_killerNamed)
	{
		return a_killerNamed || !a_byOwnersWord;
	}

	// Our spawns reported dead as they began to fall (ReportSpawnAtFall), by our game's own id for them: their death
	// events at the end of the fall are not reported again. A spawn's death is not kept with the session's dead (its id
	// may name another spawn later), so this book stands in for the ledger's FellReported. An entry whose death event
	// never came (the corpse unloaded or was deleted mid-fall, a fast travel right after the kill) counts for nothing
	// after DYING, and goes at the next report: the engine may give its id to a later spawn, whose own fall and death
	// must be reported.
	class SpawnFalls
	{
	public:
		static constexpr std::size_t MAX = 256;

		void Reported(std::uint32_t a_local, Clock::time_point a_now)
		{
			std::erase_if(reported, [&](const auto& a_entry) { return a_now - a_entry.second >= DYING; });
			if (reported.size() >= MAX && !reported.contains(a_local)) {
				return;  // its death event reports it again: harmless (the server knows the spawn no more)
			}
			reported[a_local] = a_now;
		}

		// Its death event: true when its fall was reported within DYING (it is not reported again). Forgets it either way.
		[[nodiscard]] bool TakeReported(std::uint32_t a_local, Clock::time_point a_now)
		{
			const auto it = reported.find(a_local);
			if (it == reported.end()) {
				return false;
			}
			const bool fresh = a_now - it->second < DYING;
			reported.erase(it);
			return fresh;
		}

		// Its fall was reported within DYING.
		[[nodiscard]] bool Contains(std::uint32_t a_local, Clock::time_point a_now) const
		{
			const auto it = reported.find(a_local);
			return it != reported.end() && a_now - it->second < DYING;
		}

		void Clear() { reported.clear(); }

		[[nodiscard]] std::size_t Size() const { return reported.size(); }

	private:
		std::unordered_map<std::uint32_t, Clock::time_point> reported;
	};

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
