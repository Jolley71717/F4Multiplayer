#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

// An NPC our game runs and hands to a friend's (NpcSync: the distance hand-off, FriendlyHandOff, PauseHandOff) is let
// go of, and their game claims it as nobody's. Until it does, nobody runs the NPC: it stands still in every game. Their
// game looks four times a second, so its claim is back within a second; one that has not come after CLAIM_WAIT is
// either not coming (their game has no reference of the NPC, only a copy of ours: Spawns::Absent; or its story keeps
// it off) or late (a hitch as a cell loads, a slow link, a menu opened in that moment). Our game used to wait 10 s,
// then took it back, and handed it off again 30 s after the first time: an NPC a friend's game could not run stood
// still a third of the time for as long as they stood near it, and their copy of it went and came back each time.
//
// So: our game claims it back after CLAIM_WAIT, and may hand it off again RETRY after the last time, as before. Each
// hand-off is for one player (the nearer one, or the one standing next to it). One that comes back to us with no game
// having run it is a strike against that player for that NPC, and STRIKES of them in a row (none where their game ran
// the NPC in between) mean their game can't run it: it is not handed off for that player again for BACKOFF. Another
// player near it may still be handed it. A late claim alone is one strike, and costs nothing but the RETRY. No strike
// where the player's game was stopped by a menu as it came back (their status says so: the caller's a_excused); a
// game that is loading says nothing we can read, which the two strikes are for. After a BACKOFF one more unclaimed
// return backs it off again (it is tried once each BACKOFF, not twice).
//
// A game stopped by a menu hands its NPCs off too (PauseHandOff): those wait the same CLAIM_WAIT but are for no one
// player and never a strike (the stopped game claims nothing back while it is stopped, and a friend who could not take
// one this time says nothing about the next menu).
//
// The friend's side of the same wait is SpawnBook::UNRUN_GRACE: their copy of an NPC their game has no reference of is
// kept while nobody runs it, for that long. Kept free of the game so it can be tested.
namespace HandOffClaim
{
	using Clock = std::chrono::steady_clock;

	constexpr auto         CLAIM_WAIT = std::chrono::seconds(2);  // we don't claim it back meanwhile
	constexpr auto         RETRY = std::chrono::seconds(30);      // between hand-offs of the same NPC
	constexpr auto         BACKOFF = std::chrono::minutes(5);     // for one that came back unclaimed from the same player STRIKES times in a row
	constexpr std::uint8_t STRIKES = 2;

	enum class Kind : std::uint8_t
	{
		kNearer,    // we are far from it and a friend much nearer (NpcSync's HANDOFF_FROM)
		kFriendly,  // a friendly NPC a friend stands next to (FriendlyHandOff)
		kPaused,    // our game is stopped by a menu (PauseHandOff)
	};

	// A hand-off that came back to us with no game having run the NPC since.
	struct Back
	{
		Kind          kind = Kind::kNearer;
		std::uint32_t player = 0;         // the player it was let go for
		std::uint8_t  strikes = 0;        // how many times in a row it has now come back from them so; 0 for one not counted
		bool          backedOff = false;  // STRIKES of them: not handed off for that player again for BACKOFF

		bool operator==(const Back&) const = default;
	};

	class Book
	{
	public:
		// Our game let go of it for a friend's to claim: a_for's, or with a paused game's any friend's (0).
		void HandedOff(std::uint32_t a_id, Kind a_kind, std::uint32_t a_for, Clock::time_point a_now)
		{
			handed[a_id] = { a_now, a_kind, a_for, a_kind != Kind::kPaused && a_for != 0 };
		}

		// Handed off within RETRY: not again yet, to anyone.
		[[nodiscard]] bool Lately(std::uint32_t a_id, Clock::time_point a_now) const
		{
			const auto it = handed.find(a_id);
			return it != handed.end() && a_now - it->second.at < RETRY;
		}

		// Just handed off: nobody's, and not claimed back by us yet (their game's claim is on its way).
		[[nodiscard]] bool Waits(std::uint32_t a_id, Clock::time_point a_now) const
		{
			const auto it = handed.find(a_id);
			return it != handed.end() && a_now - it->second.at < CLAIM_WAIT;
		}

		// The player a hand-off no game has answered yet was for, or 0: asked before Ours, to see whether their game is
		// stopped by a menu.
		[[nodiscard]] std::uint32_t PendingFor(std::uint32_t a_id) const
		{
			const auto it = handed.find(a_id);
			return it != handed.end() && it->second.pending ? it->second.player : 0;
		}

		// The session says our game runs it now. The hand-off it came back from with no other game having run it since, if
		// it did: a strike against the player it was for, unless a_excused (their game is stopped by a menu, or they are
		// gone). Nothing for one that another game ran meanwhile, or a paused game's.
		std::optional<Back> Ours(std::uint32_t a_id, bool a_excused, Clock::time_point a_now)
		{
			const auto it = handed.find(a_id);
			if (it == handed.end() || !std::exchange(it->second.pending, false)) {
				return std::nullopt;
			}
			Back back{ it->second.kind, it->second.player };
			if (a_excused) {
				return back;
			}
			auto& strike = strikes[Key(a_id, back.player)];
			strike.count = static_cast<std::uint8_t>((std::min)(static_cast<int>(STRIKES), strike.count + 1));
			strike.at = a_now;
			back.strikes = strike.count;
			back.backedOff = strike.count >= STRIKES;
			return back;
		}

		// The session says another player's game runs it: the hand-off took, and that player's game can run it.
		void Theirs(std::uint32_t a_id, std::uint32_t a_player)
		{
			if (const auto it = handed.find(a_id); it != handed.end()) {
				it->second.pending = false;
			}
			strikes.erase(Key(a_id, a_player));
		}

		// It came back to us unclaimed from this player STRIKES times in a row, the last within BACKOFF: handed off for
		// them neither as the nearer player nor as the one standing next to it.
		[[nodiscard]] bool BackedOff(std::uint32_t a_id, std::uint32_t a_player) const
		{
			const auto it = strikes.find(Key(a_id, a_player));
			return it != strikes.end() && it->second.count >= STRIKES;
		}

		void Expire(Clock::time_point a_now)
		{
			std::erase_if(handed, [&](const auto& a_entry) { return a_now - a_entry.second.at >= RETRY; });
			for (auto it = strikes.begin(); it != strikes.end();) {
				if (a_now - it->second.at < BACKOFF) {
					++it;
				} else if (it->second.count >= STRIKES) {
					// Backed off until now: tried again, and one more unclaimed return backs it off again.
					it->second = { a_now, static_cast<std::uint8_t>(STRIKES - 1) };
					++it;
				} else {
					it = strikes.erase(it);
				}
			}
		}

		void Clear()
		{
			handed.clear();
			strikes.clear();
		}

		[[nodiscard]] std::size_t BackedOffCount() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(strikes, [](const auto& a_entry) { return a_entry.second.count >= STRIKES; }));
		}

	private:
		struct Entry
		{
			Clock::time_point at;
			Kind              kind = Kind::kNearer;
			std::uint32_t     player = 0;       // whom it was let go for
			bool              pending = false;  // no game has been named its runner since
		};

		struct Strike
		{
			Clock::time_point at;         // the last one
			std::uint8_t      count = 0;  // in a row
		};

		static constexpr std::uint64_t Key(std::uint32_t a_id, std::uint32_t a_player) { return (static_cast<std::uint64_t>(a_id) << 32) | a_player; }

		std::unordered_map<std::uint32_t, Entry>  handed;
		std::unordered_map<std::uint64_t, Strike> strikes;  // by NPC and player
	};
}
