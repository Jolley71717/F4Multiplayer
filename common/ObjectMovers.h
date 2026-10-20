#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// Who moves a loose object right now (kReportObjectHold, kReportObjectRest): the server's book, game-free and tested
// (tests/MovedObjectsTests.cpp). One player at a time: the first to grab an object holds it until they let go, stop
// sending (HOLD_TIMEOUT) or leave; meanwhile anyone else's hold or rest of it is refused, and a refused grab is told who
// holds it, so that game lets go of its own copy. After a let-go the object's rest is still theirs to report while it
// flies and settles (FLIGHT), and for SETTLED after it came to rest: each game's physics moves it a little differently,
// and what the others' games then see of it is their own physics settling the copy just placed, not a move of theirs.
// A friend may grab it meanwhile (a broom handed over): from then on it is theirs.
namespace ObjectMovers
{
	using Clock = std::chrono::steady_clock;

	inline constexpr auto        HOLD_TIMEOUT = std::chrono::seconds(2);
	inline constexpr auto        FLIGHT = std::chrono::seconds(12);
	inline constexpr auto        SETTLED = std::chrono::seconds(3);
	inline constexpr std::size_t MAX_OBJECTS = 1024;  // objects being moved at once, in the whole session
	// Rests a player may report: a shelf knocked over at once, then a few a second.
	inline constexpr double REST_BURST = 24.0;
	inline constexpr double RESTS_PER_SECOND = 6.0;

	enum class Verdict : std::uint8_t
	{
		kRelay,    // pass it on to the others
		kRefused,  // another player moves it (a refused hold is answered with theirs: Book::Holder)
		kDropped,  // stale (an older hold than the let-go), or the book is full
	};

	struct Mover
	{
		std::uint32_t                       playerId = 0;
		bool                                holding = false;
		std::uint32_t                       sequence = 0;  // their newest hold of it, or the let-go's
		Clock::time_point                   until{};       // theirs until then
		std::optional<Protocol::ObjectHold> lastHold;      // what a refused grab is told
	};

	class Book
	{
	public:
		// a_player's hold of an object. The first grab wins: one another player still moves is refused.
		Verdict Hold(const Protocol::ObjectHold& a_hold, std::uint32_t a_player, Clock::time_point a_now)
		{
			auto it = movers.find(a_hold.refId);
			if (it != movers.end() && it->second.playerId != a_player && a_now < it->second.until && it->second.holding) {
				return Verdict::kRefused;
			}
			if (it != movers.end() && it->second.playerId == a_player && a_now < it->second.until && !Newer(a_hold.sequence, it->second.sequence)) {
				return Verdict::kDropped;  // came after their let-go (unreliable, it can overtake it), or a repeat
			}
			if (it == movers.end()) {
				if (!Room(a_now)) {
					return Verdict::kDropped;
				}
				it = movers.emplace(a_hold.refId, Mover{}).first;
			}
			auto& mover = it->second;
			mover.playerId = a_player;
			mover.holding = true;
			mover.sequence = a_hold.sequence;
			mover.until = a_now + HOLD_TIMEOUT;
			mover.lastHold = a_hold;
			mover.lastHold->playerId = a_player;
			return Verdict::kRelay;
		}

		// a_player let go of it, or says where it came to rest. Refused while another player moves it; a let-go only from
		// its holder.
		Verdict Rest(const Protocol::ObjectRest& a_rest, std::uint32_t a_player, Clock::time_point a_now)
		{
			auto it = movers.find(a_rest.refId);
			const bool theirs = it != movers.end() && a_now < it->second.until;
			if (theirs && it->second.playerId != a_player) {
				return Verdict::kRefused;
			}
			if (a_rest.kind == Protocol::ObjectRestKind::kLetGo && !(theirs && it->second.holding)) {
				return Verdict::kRefused;  // nobody let go of what they weren't holding
			}
			if (it == movers.end()) {
				if (!Room(a_now)) {
					return Verdict::kDropped;
				}
				it = movers.emplace(a_rest.refId, Mover{}).first;
			}
			auto& mover = it->second;
			mover.playerId = a_player;
			mover.holding = false;
			mover.lastHold.reset();
			if (a_rest.kind == Protocol::ObjectRestKind::kLetGo) {
				mover.sequence = a_rest.sequence;
				mover.until = a_now + FLIGHT;
			} else {
				mover.until = a_now + SETTLED;
			}
			return Verdict::kRelay;
		}

		// Who holds the object now, as their last hold of it says (for a refused grab), or none.
		[[nodiscard]] std::optional<Protocol::ObjectHold> Holder(std::uint32_t a_ref, Clock::time_point a_now) const
		{
			const auto it = movers.find(a_ref);
			if (it == movers.end() || !it->second.holding || a_now >= it->second.until) {
				return std::nullopt;
			}
			return it->second.lastHold;
		}

		// Who moves the object now (0: nobody).
		[[nodiscard]] std::uint32_t Owner(std::uint32_t a_ref, Clock::time_point a_now) const
		{
			const auto it = movers.find(a_ref);
			return it != movers.end() && a_now < it->second.until ? it->second.playerId : 0;
		}

		// A player left: what they moved is nobody's.
		void Leave(std::uint32_t a_player)
		{
			std::erase_if(movers, [&](const auto& a_entry) { return a_entry.second.playerId == a_player; });
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(movers, [&](const auto& a_entry) { return a_now >= a_entry.second.until; });
		}

		[[nodiscard]] std::size_t Size() const { return movers.size(); }

	private:
		// Sequence numbers wrap: newer is up to half the range ahead.
		static bool Newer(std::uint32_t a_seq, std::uint32_t a_than)
		{
			return a_seq != a_than && static_cast<std::uint32_t>(a_seq - a_than) < 0x80000000u;
		}

		bool Room(Clock::time_point a_now)
		{
			if (movers.size() >= MAX_OBJECTS) {
				Prune(a_now);
			}
			return movers.size() < MAX_OBJECTS;
		}

		std::unordered_map<std::uint32_t, Mover> movers;
	};
}
