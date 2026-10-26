#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
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
		// for it again once the gap before it has filled. The first missing number moves on its own only
		// when the gap is skipped (SKIP_AFTER).
		static constexpr std::size_t MAX_AHEAD = 8192;
		// How long a gap may stay open with nothing arriving before the server is asked again (a request it
		// refused, its budget being spent, is what this covers): 15 s, 30 s, then every 60 s.
		static constexpr std::array<Clock::duration, 3> ASK_AGAIN{ std::chrono::seconds{ 15 }, std::chrono::seconds{ 30 }, std::chrono::seconds{ 60 } };
		// Asked this many times for the same first missing change and it never came (the server never
		// answered, or answered without it), the gap is skipped when the next ask is due instead: its
		// numbers are given up, and what waited behind it goes in. About four minutes (15+30+60+60+60 s).
		static constexpr std::uint32_t SKIP_AFTER = 5;

		struct Counts
		{
			std::uint32_t duplicates = 0;    // a change we had already, or had waiting
			std::uint32_t gaps = 0;          // times changes started waiting for an earlier one
			std::uint32_t droppedAhead = 0;  // waiting changes let go at MAX_AHEAD (asked for again later)
			std::uint32_t asked = 0;         // times the server was asked again for a gap
			std::uint32_t rewinds = 0;       // times the server's log turned out shorter than our world's
			std::uint32_t lost = 0;          // changes those rewinds counted again from
			std::uint32_t strays = 0;        // waiting changes numbered past the end of the server's log, let go
			std::uint32_t skips = 0;         // gaps skipped after SKIP_AFTER asks
			std::uint32_t skipped = 0;       // the numbers those skips gave up
		};

		// The first change our world lacks.
		[[nodiscard]] std::uint32_t Next() const { return next; }
		// Changes waiting for an earlier one.
		[[nodiscard]] std::size_t   Waiting() const { return ahead.size(); }
		// We asked the server for changes (a hello's, a load's, a gap's) and no answer has come yet.
		[[nodiscard]] bool          Asking() const { return requested.has_value(); }
		[[nodiscard]] const Counts& Stats() const { return counts; }

		// Our world has every change before a_next and nothing after (a new session, a save's record).
		void Restart(std::uint32_t a_next)
		{
			next = a_next;
			ahead.clear();
			requested.reset();
			tries = 0;
			stuck = 0;
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
			Release(ready, a_now);
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
		// When it starts where we asked with less than a full chunk, the server's log ends after its last
		// change (it numbers a change as it logs it, and sends what it numbered after this behind it): changes
		// waiting with numbers from there on were never numbered by this server (they came from somewhere
		// else, like the fuzz command, or from a log it no longer has), and nothing would ever fill the gap
		// before them. They are let go (strays).
		// Returns how many numbers were counted again (0: none).
		std::uint32_t OnChunk(std::uint32_t a_firstIndex, std::size_t a_count, Clock::time_point a_now)
		{
			quietSince = a_now;
			const auto asked = std::exchange(requested, std::nullopt);
			if (!asked || a_firstIndex > *asked) {
				return 0;
			}
			if (a_firstIndex == *asked) {
				if (a_count < Protocol::WORLD_STATE_CHUNK) {
					const auto stray = ahead.lower_bound(a_firstIndex + static_cast<std::uint32_t>(a_count));
					counts.strays += static_cast<std::uint32_t>(std::distance(stray, ahead.end()));
					ahead.erase(stray, ahead.end());
					if (ahead.empty()) {
						tries = 0;
						stuck = 0;
					}
				}
				return 0;
			}
			if (a_count != 0) {
				return 0;
			}
			const auto lost = *asked - a_firstIndex;
			next = a_firstIndex;
			ahead.clear();
			tries = 0;
			stuck = 0;
			++counts.rewinds;
			counts.lost += lost;
			return lost;
		}

		// Whether to ask the server again now: a gap has stayed open with nothing arriving.
		bool Due(Clock::time_point a_now)
		{
			if (!Quiet(a_now)) {
				return false;
			}
			++tries;
			++stuck;
			++counts.asked;
			quietSince = a_now;
			return true;
		}

		// A gap asked for SKIP_AFTER times, due to be asked for again: it is given up instead. Our count moves
		// to the first waiting change, which goes in with the ones right behind it (returned, in order). The
		// changes the gap stood for never reach our world; were they the server's, a later catch-up (a load,
		// a rejoin) brings them no more either, as our world counts them in. Nothing otherwise.
		std::vector<Ready> Skip(Clock::time_point a_now)
		{
			std::vector<Ready> ready;
			if (stuck < SKIP_AFTER || !Quiet(a_now)) {
				return ready;
			}
			const auto first = ahead.begin()->first;
			++counts.skips;
			counts.skipped += first - next;
			next = first;
			Release(ready, a_now);
			return ready;
		}

	private:
		// A gap stayed open with nothing arriving for as long as it may before the next ask.
		[[nodiscard]] bool Quiet(Clock::time_point a_now) const
		{
			const auto wait = ASK_AGAIN[(std::min)(static_cast<std::size_t>(tries), ASK_AGAIN.size() - 1)];
			return !ahead.empty() && a_now - quietSince >= wait;
		}

		// Our count has moved to next: the waiting changes from there on, as far as they run unbroken, go in.
		void Release(std::vector<Ready>& a_ready, Clock::time_point a_now)
		{
			for (auto it = ahead.begin(); it != ahead.end() && it->first == next; it = ahead.erase(it)) {
				a_ready.push_back(std::move(it->second));
				++next;
			}
			quietSince = a_now;
			stuck = 0;
			if (ahead.empty()) {
				tries = 0;
			}
		}

		std::uint32_t                   next = 0;
		std::map<std::uint32_t, Ready>  ahead;
		std::optional<std::uint32_t>    requested;
		Clock::time_point               quietSince{};  // the last change that went in, chunk or request, or when the gap opened
		std::uint32_t                   tries = 0;     // times asked again for this gap
		std::uint32_t                   stuck = 0;     // times asked again since our count last moved
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
	// ours in it, and what of the changes that come goes into the world. Our own changes never go in: made
	// in the world we have, they are in it; made before the save we loaded, that save is from before them,
	// and replaying them would take loot from the container that isn't in our inventory any more (or put
	// in a second copy of what we put in). Those are counted (OwnSkipped, DupRisk). Everything else goes
	// to a_apply, in session order.
	class Receiver
	{
	public:
		using Apply = std::function<void(const Protocol::IndexedContainerChange&)>;
		// One of our own changes, next in order (none goes to Apply). a_lacking: our world lacks it (made before the save we
		// loaded, or in a world we no longer have), as OwnSkipped counts.
		using Own = std::function<void(const Protocol::IndexedContainerChange&, bool a_lacking)>;

		// Our reports remembered until they come back numbered, at most. Past it the oldest is forgotten
		// (its echo is then judged by how it came: see Deliver).
		static constexpr std::size_t MAX_REPORTS = 4096;

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
				reports.clear();
			}
			ownIds.insert(a_localId);
			return fresh;
		}

		// A save's record was read (or the save has none: session 0). The world is another one from here
		// on: what we reported before it isn't in it, unless the save was made after it.
		void Load(std::uint64_t a_session, std::uint32_t a_next)
		{
			session = a_session;
			ledger.Restart(a_next);
			++world;
		}

		// We reported one of our own moves. It comes back numbered (live, or in a catch-up when our world
		// lacks its number), maybe after a load: a move made in the frame before one goes out ahead of the
		// load's request (WorldSync::FlushLoot), and its echo can arrive after the load either way.
		void Reported(const Protocol::ContainerChange& a_change)
		{
			if (reports.size() >= MAX_REPORTS) {
				reports.pop_front();
			}
			reports.push_back({ a_change, world });
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

		void Live(const Protocol::IndexedContainerChange& a_change, Clock::time_point a_now, const Apply& a_apply, const Own& a_own = {})
		{
			for (const auto& ready : ledger.Offer(a_change, false, a_now)) {
				Deliver(ready, a_apply, a_own);
			}
		}

		// One WorldState's container changes. Returns how many numbers the server's log lost (see
		// Ledger::OnChunk); our count then starts again at a_firstIndex.
		std::uint32_t CatchUp(std::uint32_t a_firstIndex, const std::vector<Protocol::IndexedContainerChange>& a_changes, Clock::time_point a_now, const Apply& a_apply,
			const Own& a_own = {})
		{
			const auto lost = ledger.OnChunk(a_firstIndex, a_changes.size(), a_now);
			for (const auto& change : a_changes) {
				for (const auto& ready : ledger.Offer(change, true, a_now)) {
					Deliver(ready, a_apply, a_own);
				}
			}
			return lost;
		}

		// Whether to ask the server again now (see Ledger::Due); then send Request. A gap asked for
		// Ledger::SKIP_AFTER times is skipped instead (Ledger::Skip): what waited behind it goes to a_apply.
		bool Due(Clock::time_point a_now, const Apply& a_apply, const Own& a_own = {})
		{
			for (const auto& ready : ledger.Skip(a_now)) {
				Deliver(ready, a_apply, a_own);
			}
			return ledger.Due(a_now);
		}

		[[nodiscard]] std::uint64_t  SessionId() const { return session; }
		[[nodiscard]] bool           Ours(std::uint32_t a_playerId) const { return ownIds.contains(a_playerId); }
		[[nodiscard]] const Ledger&  Numbers() const { return ledger; }
		[[nodiscard]] std::uint32_t  OwnSkipped() const { return ownSkipped; }
		[[nodiscard]] std::uint32_t  DupRisk() const { return dups.Count(); }

	private:
		void Deliver(const Ready& a_ready, const Apply& a_apply, const Own& a_own)
		{
			const auto& change = a_ready.change;
			if (ownIds.contains(change.playerId)) {
				// Made before the world we have: the save we loaded is from before it (or we don't remember
				// making it, and it came in a catch-up). A move made in the moments before a save it was in
				// is counted too when its echo is late (rare: the save would have to come within a round trip).
				const auto made = MadeIn(change.change);
				const bool lacking = made ? *made != world : a_ready.catchUp;
				if (lacking) {
					++ownSkipped;
					dups.OwnSkipped(change.change);
				}
				if (a_own) {
					a_own(change, lacking);
				}
				return;
			}
			dups.Foreign(change.change);
			if (Protocol::IsValid(change.change)) {
				a_apply(change);
			}
		}

		// The world our report a_change was made in, if we remember it; forgets it and the reports before it
		// (those the server dropped, or whose number our world had already).
		std::optional<std::uint32_t> MadeIn(const Protocol::ContainerChange& a_change)
		{
			const auto it = std::ranges::find_if(reports, [&](const Report& a_report) {
				return a_report.change.container == a_change.container && a_report.change.item == a_change.item && a_report.change.count == a_change.count;
			});
			if (it == reports.end()) {
				return std::nullopt;
			}
			const auto made = it->world;
			reports.erase(reports.begin(), std::next(it));
			return made;
		}

		struct Report
		{
			Protocol::ContainerChange change;
			std::uint32_t             world = 0;
		};

		std::uint64_t                     session = 0;
		std::uint32_t                     world = 0;  // which world (counted by loads) our game has now
		std::deque<Report>                reports;    // ours, oldest first, not yet back
		std::unordered_set<std::uint32_t> ownIds;  // a reconnect normally gets the old ID back, but not while the server still holds the old connection
		Ledger                            ledger;
		DupWatch                          dups;
		std::uint32_t                     ownSkipped = 0;
	};
}
