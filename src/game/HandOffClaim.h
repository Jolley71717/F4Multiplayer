#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

// An NPC our game runs and hands to a friend's (NpcSync: the distance hand-off, FriendlyHandOff, PauseHandOff) is let
// go of, and their game claims it as nobody's. Until it does, nobody runs the NPC: it stands still in every game. Their
// game looks four times a second, so its claim is back within a second; one that has not come after CLAIM_WAIT is not
// coming (their game has no reference of the NPC, only a copy of ours: Spawns::Absent; or its story keeps it off, or it
// is stopped by a menu we don't count). Our game used to wait 10 s, then took it back, and handed it off again 30 s
// after the first time: an NPC a friend's game could not run stood still a third of the time for as long as they stood
// near it, and their copy of it went and came back each time.
//
// So: our game claims it back after CLAIM_WAIT, and one that comes back to us so, unclaimed, is not handed off again
// (to a nearer player, or to one standing next to it) for BACKOFF. One a friend's game did take is theirs, and may be
// handed off again RETRY after the last time, as before. A game stopped by a menu hands its NPCs off too (PauseHandOff):
// those wait the same CLAIM_WAIT but are never backed off (the stopped game claims nothing back while it is stopped,
// and a friend who could not take one this time says nothing about the next menu).
//
// The friend's side of the same wait is SpawnBook::UNRUN_GRACE: their copy of an NPC their game has no reference of is
// kept while nobody runs it, for that long. Kept free of the game so it can be tested.
namespace HandOffClaim
{
	using Clock = std::chrono::steady_clock;

	constexpr auto CLAIM_WAIT = std::chrono::seconds(2);  // we don't claim it back meanwhile
	constexpr auto RETRY = std::chrono::seconds(30);      // between hand-offs of the same NPC
	constexpr auto BACKOFF = std::chrono::minutes(5);     // for one that came back to us unclaimed

	enum class Kind : std::uint8_t
	{
		kNearer,    // we are far from it and a friend much nearer (NpcSync's HANDOFF_FROM)
		kFriendly,  // a friendly NPC a friend stands next to (FriendlyHandOff)
		kPaused,    // our game is stopped by a menu (PauseHandOff)
	};

	class Book
	{
	public:
		// Our game let go of it for a friend's to claim.
		void HandedOff(std::uint32_t a_id, Kind a_kind, Clock::time_point a_now) { handed[a_id] = { a_now, a_kind, a_kind != Kind::kPaused }; }

		// Handed off within RETRY: not again yet.
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

		// The session says our game runs it now. The kind of hand-off it came back from with no other game having run it
		// since, if it did: backed off from here on. Nothing for one that another game ran meanwhile, or a paused game's.
		std::optional<Kind> Ours(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = handed.find(a_id);
			if (it == handed.end() || !std::exchange(it->second.pending, false)) {
				return std::nullopt;
			}
			refused[a_id] = a_now;
			return it->second.kind;
		}

		// The session says another player's game runs it: the hand-off took.
		void Theirs(std::uint32_t a_id)
		{
			if (const auto it = handed.find(a_id); it != handed.end()) {
				it->second.pending = false;
			}
		}

		// It came back to us unclaimed within BACKOFF: neither handed to a nearer player nor to one standing next to it.
		[[nodiscard]] bool BackedOff(std::uint32_t a_id) const { return refused.contains(a_id); }

		void Expire(Clock::time_point a_now)
		{
			std::erase_if(handed, [&](const auto& a_entry) { return a_now - a_entry.second.at >= RETRY; });
			std::erase_if(refused, [&](const auto& a_entry) { return a_now - a_entry.second >= BACKOFF; });
		}

		void Clear()
		{
			handed.clear();
			refused.clear();
		}

		[[nodiscard]] std::size_t BackedOffCount() const { return refused.size(); }

	private:
		struct Entry
		{
			Clock::time_point at;
			Kind              kind = Kind::kNearer;
			bool              pending = false;  // no game has been named its runner since
		};

		std::unordered_map<std::uint32_t, Entry>             handed;
		std::unordered_map<std::uint32_t, Clock::time_point> refused;
	};
}
