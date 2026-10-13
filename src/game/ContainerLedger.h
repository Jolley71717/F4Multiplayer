#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

// Which of the session's numbered container changes our world has, and the order they go into it in.
// The server numbers every change and sends it live to everyone (the sender too); a player who joins,
// reconnects or loads a save asks for the changes from the first number its world lacks and gets them
// in a WorldState. Live changes can arrive ahead of that reply (made while the save loaded), and a
// world can be ahead of the server (the host's game stopped before writing its last changes to the
// session file). Kept free of the game so it can be tested; WorldSync puts what is ready into the world.
namespace ContainerLedger
{
	using Clock = std::chrono::steady_clock;

	// A change ready to go into the world, in session order.
	struct Ready
	{
		Protocol::IndexedContainerChange change;
		bool                             catchUp = false;  // it first came in a WorldState, not live
	};

	// The numbering: what is in, and what came early and waits for the changes before it.
	class Ledger
	{
	public:
		// Changes that wait for an earlier one, at most. Past it the newest is let go: the server is asked
		// for it again once the gap before it has filled. The first missing number never moves on its own.
		static constexpr std::size_t MAX_AHEAD = 8192;
		// How long a gap may stay open with nothing arriving before the server is asked again (a request it
		// refused, its budget being spent, is what this covers): 15 s, 30 s, then every 60 s.
		static constexpr std::array<Clock::duration, 3> ASK_AGAIN{ std::chrono::seconds{ 15 }, std::chrono::seconds{ 30 }, std::chrono::seconds{ 60 } };

		struct Counts
		{
			std::uint32_t duplicates = 0;    // a change we had already, or had waiting
			std::uint32_t gaps = 0;          // times changes started waiting for an earlier one
			std::uint32_t droppedAhead = 0;  // waiting changes let go at MAX_AHEAD (asked for again later)
			std::uint32_t asked = 0;         // times the server was asked again for a gap
			std::uint32_t rewinds = 0;       // times the server's log turned out shorter than our world's
			std::uint32_t lost = 0;          // changes those rewinds counted again from
		};

		// The first change our world lacks.
		[[nodiscard]] std::uint32_t Next() const { return next; }
		// Changes waiting for an earlier one.
		[[nodiscard]] std::size_t   Waiting() const { return ahead.size(); }
		[[nodiscard]] const Counts& Stats() const { return counts; }

		// Our world has every change before a_next and nothing after (a new session, a save's record).
		void Restart(std::uint32_t a_next)
		{
			next = a_next;
			ahead.clear();
			requested.reset();
			tries = 0;
		}

		// A numbered change reached us. Returns what can go into the world now, in order (it, and those that
		// waited for it), or nothing.
		std::vector<Ready> Offer(const Protocol::IndexedContainerChange& a_change, bool a_catchUp, Clock::time_point a_now)
		{
			std::vector<Ready> ready;
			const auto         index = a_change.index;
			if (index < next || ahead.contains(index)) {
				++counts.duplicates;
				return ready;
			}
			if (index > next) {
				if (ahead.empty()) {
					quietSince = a_now;
					tries = 0;
					++counts.gaps;
				}
				if (ahead.size() >= MAX_AHEAD) {
					++counts.droppedAhead;
					const auto newest = std::prev(ahead.end());
					if (newest->first < index) {
						return ready;
					}
					ahead.erase(newest);
				}
				ahead.emplace(index, Ready{ a_change, a_catchUp });
				return ready;
			}
			ready.push_back({ a_change, a_catchUp });
			++next;
			for (auto it = ahead.begin(); it != ahead.end() && it->first == next; it = ahead.erase(it)) {
				ready.push_back(std::move(it->second));
				++next;
			}
			quietSince = a_now;
			if (ahead.empty()) {
				tries = 0;
			}
			return ready;
		}

		// We asked the server for every change from a_from (Hello, or RequestWorldState).
		void OnRequest(std::uint32_t a_from, Clock::time_point a_now)
		{
			requested = a_from;
			quietSince = a_now;
		}

		// A WorldState came, whose container changes start at a_firstIndex (a_count of them). Called before
		// they are offered. The first one after our request answers it: when it starts below what we asked
		// for and has nothing, the server's log is shorter than our world's (the host's game stopped without
		// writing its last changes to the session file; it writes every 15 s). Its next change takes the
		// first number it lacks, so ours counts from there; what our world got of the lost ones stays in it.
		// Only a request we sent can do that: a WorldState nobody asked for never moves the count back.
		// Returns how many numbers were counted again (0: none).
		std::uint32_t OnChunk(std::uint32_t a_firstIndex, std::size_t a_count, Clock::time_point a_now)
		{
			quietSince = a_now;
			const auto asked = std::exchange(requested, std::nullopt);
			if (!asked || a_firstIndex >= *asked || a_count != 0) {
				return 0;
			}
			const auto lost = *asked - a_firstIndex;
			next = a_firstIndex;
			ahead.clear();
			tries = 0;
			++counts.rewinds;
			counts.lost += lost;
			return lost;
		}

		// Whether to ask the server again now: a gap has stayed open with nothing arriving.
		bool Due(Clock::time_point a_now)
		{
			const auto wait = ASK_AGAIN[(std::min)(static_cast<std::size_t>(tries), ASK_AGAIN.size() - 1)];
			if (ahead.empty() || a_now - quietSince < wait) {
				return false;
			}
			++tries;
			++counts.asked;
			quietSince = a_now;
			return true;
		}

	private:
		std::uint32_t                   next = 0;
		std::map<std::uint32_t, Ready>  ahead;
		std::optional<std::uint32_t>    requested;
		Clock::time_point               quietSince{};  // the last change that went in, chunk or request, or when the gap opened
		std::uint32_t                   tries = 0;     // times asked again for this gap
		Counts                          counts;
	};

	// The duplication loop that is still open: our own deposit skipped in a catch-up (the save we loaded
	// is from before it, so the item is back in our inventory), then another player taking the same item
	// out of the same container (so they have it too). Counted so a soak measures it.
	class DupWatch
	{
	public:
		static constexpr std::size_t MAX_REMEMBERED = 4096;

		void OwnSkipped(const Protocol::ContainerChange& a_change)
		{
			if (a_change.count > 0 && deposits.size() < MAX_REMEMBERED) {
				deposits.insert({ a_change.container, a_change.item });
			}
		}

		// Another player's change. True when it takes what our skipped deposit put in.
		bool Foreign(const Protocol::ContainerChange& a_change)
		{
			if (a_change.count >= 0 || !deposits.erase({ a_change.container, a_change.item })) {
				return false;
			}
			++count;
			return true;
		}

		void Clear() { deposits.clear(); }

		[[nodiscard]] std::uint32_t Count() const { return count; }

	private:
		std::set<std::pair<std::uint32_t, std::uint32_t>> deposits;  // container, item
		std::uint32_t                                     count = 0;
	};

	// The receiving end, as WorldSync runs it: which session our world belongs to, which player IDs are
	// ours in it, and what of the changes that come goes into the world. Our own changes are in our world
	// already: live, we made them; in a catch-up, the save we loaded is from before them, and replaying
	// them would take loot from the container that isn't in our inventory any more (or put in a second
	// copy of what we put in). Everything else goes to a_apply, in session order.
	class Receiver
	{
	public:
		using Apply = std::function<void(const Protocol::IndexedContainerChange&)>;

		// The server welcomed us. True when it is another session than our world's: none of its changes
		// are in our world yet.
		bool OnWelcome(std::uint64_t a_session, std::uint32_t a_localId)
		{
			const bool fresh = a_session != session;
			if (fresh) {
				session = a_session;
				ledger.Restart(0);
				ownIds.clear();
				dups.Clear();
			}
			ownIds.insert(a_localId);
			return fresh;
		}

		// A save's record was read (or the save has none: session 0).
		void Load(std::uint64_t a_session, std::uint32_t a_next)
		{
			session = a_session;
			ledger.Restart(a_next);
		}

		// After a load while connected: a save from no session (made alone, or another character's) or
		// from another one takes the session we are in, from its first change, so the saves made from it
		// on carry a record. True when it did (none of the session's changes are in that world).
		bool Adopt(std::uint64_t a_session, std::uint32_t a_localId)
		{
			ownIds.insert(a_localId);
			if (a_session == 0 || a_session == session) {
				return false;
			}
			session = a_session;
			ledger.Restart(0);
			dups.Clear();
			return true;
		}

		// What to ask the server for: everything our world lacks. Counts as asked.
		Protocol::WorldRequest Request(Clock::time_point a_now)
		{
			ledger.OnRequest(ledger.Next(), a_now);
			return { session, ledger.Next() };
		}

		// What our world has, without asking (for the save's record).
		[[nodiscard]] Protocol::WorldRequest Point() const { return { session, ledger.Next() }; }

		void Live(const Protocol::IndexedContainerChange& a_change, Clock::time_point a_now, const Apply& a_apply)
		{
			for (const auto& ready : ledger.Offer(a_change, false, a_now)) {
				Deliver(ready, a_apply);
			}
		}

		// One WorldState's container changes. Returns how many numbers the server's log lost (see
		// Ledger::OnChunk); our count then starts again at a_firstIndex.
		std::uint32_t CatchUp(std::uint32_t a_firstIndex, const std::vector<Protocol::IndexedContainerChange>& a_changes, Clock::time_point a_now, const Apply& a_apply)
		{
			const auto lost = ledger.OnChunk(a_firstIndex, a_changes.size(), a_now);
			for (const auto& change : a_changes) {
				for (const auto& ready : ledger.Offer(change, true, a_now)) {
					Deliver(ready, a_apply);
				}
			}
			return lost;
		}

		// Whether to ask the server again now (see Ledger::Due); then send Request.
		bool Due(Clock::time_point a_now) { return ledger.Due(a_now); }

		[[nodiscard]] std::uint64_t  SessionId() const { return session; }
		[[nodiscard]] bool           Ours(std::uint32_t a_playerId) const { return ownIds.contains(a_playerId); }
		[[nodiscard]] const Ledger&  Numbers() const { return ledger; }
		[[nodiscard]] std::uint32_t  OwnSkipped() const { return ownSkipped; }
		[[nodiscard]] std::uint32_t  DupRisk() const { return dups.Count(); }

	private:
		void Deliver(const Ready& a_ready, const Apply& a_apply)
		{
			const auto& change = a_ready.change;
			if (ownIds.contains(change.playerId)) {
				if (a_ready.catchUp) {
					++ownSkipped;
					dups.OwnSkipped(change.change);
				}
				return;
			}
			dups.Foreign(change.change);
			if (Protocol::IsValid(change.change)) {
				a_apply(change);
			}
		}

		std::uint64_t                     session = 0;
		std::unordered_set<std::uint32_t> ownIds;  // a reconnect normally gets the old ID back, but not while the server still holds the old connection
		Ledger                            ledger;
		DupWatch                          dups;
		std::uint32_t                     ownSkipped = 0;
	};
}
