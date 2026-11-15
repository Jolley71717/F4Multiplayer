#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Shared storage, a game's side (Protocol::SessionRules::sharedStorage, the host's bSharedStorage): a home's or a
// settlement's container has one true copy, the host's game's, and the server keeps what it holds (common/StorageBook.h).
//
// - The host's game lists each storage container it has loaded (LootSync::Loot::Listing), which claims it, and applies
//   friends' takes and puts as it applies any loot. A take its container held less of than taken (its world was
//   reloaded since it last listed) goes back as a shortfall, and the taker's game gives it back.
// - A friend's game shows the true copy in its own copy of the container while its menu is open on it, and only then:
//   at the open, the shared things its copy held (the player's own stash from before, or the host's things it mirrored
//   in an older session) are set aside, out of the copy and out of reach, and the server's contents come in their place;
//   at the close the session's things leave the copy and what was set aside comes back (Client). Nothing set aside ever
//   goes to the player. Their takes and puts are reported as any loot is, and the server checks each against what it
//   holds. One it refuses is undone here: a take given back, a put returned. One with no answer stays on its way: its
//   number or refusal may still come, and should the session never have numbered it (the server dropped it, the
//   connection went), it goes again late with our next settle.
//   While the menu is closed nothing of the session's goes into that copy, and the player's own moves there stay theirs.
// - Settling (a load, a rejoin; not a gap asked for again: our world is the same, WorldRequest::kGap). A player can always move an item out of their own inventory
//   before a correction reaches it, never out of the host's container without the server, so what a load or a lost
//   connection leaves wrong is put right in the true copy, never on our player: our world keeps what its save has. From
//   the request until its catch-up has come, nothing is done at a friend's storage (the menu closes; the server refuses
//   the view too). Then our game settles (Protocol::StorageSettle): our numbered moves there our world lacks (a load
//   from before them: Side::Own), which the server undoes in the true copy, and our moves still unnumbered (late). The
//   moves a save was made with still unanswered, and those its world lacked, are kept with it (the co-save), so a later
//   save loaded has the server do again what an earlier one had it undo. Only takes numbered since the container was
//   claimed are undone (StorageOwner::since): one made while nobody kept it was ordinary loot, which the host's world
//   and the loaded one agree on.
// - We take back from our player only for a refusal or a shortfall (the keeper's container held less than the server
//   knew), an answer that comes as the move is made; what they no longer hold of it is counted, not made up for.
// - A shared item our copy holds a quest's alias of stays the player's own while the menu is open (Engine::Aliased): not
//   set aside, the host's stack of it not shown, and moves of it kept to our copy.
// - Storage nobody keeps yet (the host's game hasn't loaded it since the rule came on) is ordinary loot, as in 0.9.2.
//
// Kept free of the game so it can be tested; Storage.cpp is the Engine, and WorldSync's loot asks it who keeps a
// container (LootSync::World).
namespace StorageView
{
	using Clock = std::chrono::steady_clock;

	enum class Keeper : std::uint8_t
	{
		kNone,    // no rule, or nobody keeps it: ordinary loot
		kUs,      // our game keeps the true copy (the host's)
		kFriend,  // another game keeps it: our copy shows it while our menu is open
	};

	// A view's change is not yet numbered when its contents haven't come.
	inline constexpr std::uint32_t NOT_YET = 0xFFFFFFFF;

	// Our moves at friends' storage not yet numbered or refused, at most: a Take All and a Store All of the fullest
	// container the server keeps (Protocol::MAX_STORAGE_STACKS stacks each), and more. Past it a move is undone, not sent.
	inline constexpr std::size_t MAX_IN_FLIGHT = Protocol::MAX_STORAGE_MOVES_UNANSWERED;
	// A move with no answer after this is counted unanswered (for the log). It stays on its way: its number or refusal may
	// still come, and one the session never numbered goes late with the next settle.
	inline constexpr Clock::duration ANSWER_WITHIN = std::chrono::seconds{ 30 };
	// The same notice at most this often (a Take All refused is one notice, not a thousand).
	inline constexpr Clock::duration NOTICE_EVERY = std::chrono::seconds{ 2 };
	// A build in workshop mode paid from a friend's workbench (Client::BuildPlaced): the piece placed and the engine's takes
	// of its components reach us within a frame or two of each other, in either order. What comes within this of the piece
	// is counted the piece's own.
	inline constexpr Clock::duration BUILD_PAIR_WITHIN = std::chrono::milliseconds{ 250 };
	// The takes of a build are answered within a round trip. One still unanswered this long after the piece was placed is
	// waited for no longer: the build stands.
	inline constexpr Clock::duration BUILD_SETTLE_WITHIN = std::chrono::seconds{ 3 };
	// Builds followed at once, and takes (or uses of the player's own things) remembered for them, at most.
	inline constexpr std::size_t MAX_BUILDS = 16;
	inline constexpr std::size_t MAX_BUILD_TAKES = 64;

	// One stack to add to (count > 0) or take out of (count < 0) an inventory.
	struct Adjust
	{
		std::uint32_t item = 0;
		std::int32_t  count = 0;

		bool operator==(const Adjust&) const = default;
	};

	// What undoing (or doing again) one of our moves does.
	struct Undo
	{
		bool          matched = false;    // one of our moves (anything else is left alone)
		bool          closeView = false;  // the server closed our view
		bool          refresh = false;    // we view that container: ask for its contents again (others' changes may have gone into our copy meanwhile)
		std::uint32_t container = 0;
		std::uint32_t item = 0;
		std::int64_t  player = 0;  // to our player's inventory: > 0 given, < 0 taken
		std::int64_t  copy = 0;    // to our copy of the container (only while it shows the true copy)
	};

	// What the copy of a container should hold (want) against what it holds (have), each item's count: the changes that
	// make it so, in item order.
	inline std::vector<Adjust> Reconcile(const std::map<std::uint32_t, std::int64_t>& a_want, const std::map<std::uint32_t, std::int64_t>& a_have)
	{
		std::vector<Adjust> out;
		const auto          clamp = [](std::int64_t a_count) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(a_count, -Protocol::MAX_ITEM_COUNT, Protocol::MAX_ITEM_COUNT));
		};
		for (const auto& [item, want] : a_want) {
			const auto it = a_have.find(item);
			const auto have = it != a_have.end() ? it->second : 0;
			if (want != have) {
				out.push_back({ item, clamp((std::max<std::int64_t>)(want, 0) - have) });
			}
		}
		for (const auto& [item, have] : a_have) {
			if (have > 0 && !a_want.contains(item)) {
				out.push_back({ item, clamp(-have) });
			}
		}
		std::erase_if(out, [](const Adjust& a_adjust) { return a_adjust.count == 0; });
		std::ranges::sort(out, {}, &Adjust::item);
		return out;
	}

	// The host's game: the workbenches it keeps whose settlement its player seems to own no longer. One look can be wrong
	// (the workbench's owner is read from afar, and in the moments around a load), so a workbench is given back only when
	// two looks in a row found it so.
	class LostLooks
	{
	public:
		// a_unowned: the kept workbenches this look found not our player's. Those the look before found so too; they are
		// forgotten here (given back now, they are looked at afresh).
		std::vector<std::uint32_t> Look(std::vector<std::uint32_t> a_unowned)
		{
			std::ranges::sort(a_unowned);
			a_unowned.erase(std::unique(a_unowned.begin(), a_unowned.end()), a_unowned.end());
			std::vector<std::uint32_t>        twice;
			std::unordered_set<std::uint32_t> once;
			for (const auto bench : a_unowned) {
				if (before.contains(bench)) {
					twice.push_back(bench);
				} else {
					once.insert(bench);
				}
			}
			before = std::move(once);
			return twice;
		}

		// Nothing seen counts (a welcome, a load, a lost connection).
		void Clear() { before.clear(); }

	private:
		std::unordered_set<std::uint32_t> before;
	};

	// The bookkeeping: who keeps what, our view, our moves at friends' storage until they are answered, and our numbered
	// moves there our world lacks.
	class Side
	{
	public:
		static constexpr std::size_t MAX_OWNERS = 4096;    // as the server's book (StorageBook::Book::MAX_CONTAINERS)
		static constexpr std::size_t MAX_NUMBERED = MAX_IN_FLIGHT;  // our numbered takes there a shortfall may still come for
		static constexpr std::size_t MAX_LACKS = Protocol::MAX_STORAGE_LACKING;

		struct Counts
		{
			std::uint32_t views = 0;          // menus opened on a friend's storage
			std::uint32_t closedByHost = 0;   // views the server closed (its keeper away, or its world reloaded)
			std::uint32_t settlingClosed = 0; // menus closed as they opened: our moves not settled yet since our world loaded
			std::uint32_t refusedTakes = 0;   // our takes refused, given back
			std::uint32_t refusedPuts = 0;    // our puts refused, returned
			std::uint32_t shortTakes = 0;     // numbered takes the keeper's container held less of: given back
			std::uint32_t unmatched = 0;      // refusals for no move we remember (made before a load): left alone
			std::uint32_t unanswered = 0;     // our moves with no answer after ANSWER_WITHIN: kept on their way (late at a settle)
			std::uint32_t unsent = 0;         // our moves undone because they couldn't be sent (an item the session lacks)
			std::uint32_t overflow = 0;       // our moves undone unsent at MAX_IN_FLIGHT
			std::uint32_t lacking = 0;        // our numbered moves a catch-up found our world lacks (the true copy undoes them)
			std::uint32_t lacksDropped = 0;   // of those, forgotten at MAX_LACKS
			std::uint32_t corrected = 0;      // our takes kept with a save, numbered smaller than made: the rest taken back
			std::uint32_t settles = 0;        // settles sent (after a load, a hello, a gap asked for again)
			std::uint32_t late = 0;           // our moves the session never numbered, sent again with a settle
			std::uint32_t listed = 0;         // our storage listings sent (the host's game)
			std::uint32_t shorts = 0;         // friends' takes our container held less of (the host's game)
		};

		// The view that ended (Close).
		struct Ended
		{
			std::uint32_t           container = 0;
			bool                    closed = false;  // the server had closed it already: our copy holds nothing of the session's
			std::set<std::uint32_t> aliased;         // items left as our own for the view (see Open)
		};

		// ---- the session ----------------------------------------------------------------------------------

		// Welcomed into a_session under its rule; a_host: our game is the host's (its world is the session's). The owner
		// table comes after the welcome, then the hello's world state, and we settle after it (CaughtUp). Our moves still
		// unanswered and those our world lacks stay when it is the same session: the settle tells the server of them.
		void OnWelcome(bool a_rule, bool a_host, std::uint64_t a_session, Clock::time_point a_now)
		{
			rule = a_rule;
			host = a_host;
			marker.reset();
			owners.clear();
			listed.clear();
			views.clear();
			if (a_session != movesSession) {
				flights.clear();
				numbered.clear();
				lacks.clear();
				lacksFrom = 0;
				movesSession = a_session;
				known.clear();
			}
			for (auto& flight : flights) {
				flight.due = a_now + ANSWER_WITHIN;
			}
			Asked();
		}

		// The connection went: our moves still unanswered stay on their way. The rejoin's catch-up brings the numbers of
		// those the session had; the rest go again late with its settle.
		void OnDisconnected()
		{
			rule = false;
			marker.reset();
			owners.clear();
			listed.clear();
			views.clear();
			for (auto& flight : flights) {
				flight.settle = true;
			}
		}

		// Our world is a save's from here (a load) in a_session: a_unanswered, the moves the save was made with still
		// unanswered, and a_lacks (from a_lacksFrom on), our numbered moves its world lacked (its co-save record; none when
		// it is from another session). Nothing else we did before is in it: the catch-up finds what it lacks since.
		void OnLoad(std::uint64_t a_session, std::span<const Protocol::ContainerChange> a_unanswered, std::span<const std::uint32_t> a_lacks, std::uint32_t a_lacksFrom,
			Clock::time_point a_now)
		{
			views.clear();
			listed.clear();
			flights.clear();
			numbered.clear();
			lacks.clear();
			lacksFrom = a_lacksFrom;
			movesSession = a_session;
			for (const auto& change : a_unanswered) {
				if (flights.size() < MAX_IN_FLIGHT) {
					flights.push_back({ .change = change, .due = a_now + ANSWER_WITHIN, .settle = true, .restored = true });
				}
			}
			for (const auto index : a_lacks) {
				if (index >= lacksFrom && lacks.size() < MAX_LACKS) {
					lacks.insert(index);
				}
			}
			settling = true;
		}

		// Our world asked the server for the session's changes (a hello, a load, a gap): the moves made before it go late
		// if its catch-up doesn't number them, and nothing more is done at a friend's storage until we have settled.
		void Asked()
		{
			settling = true;
			for (auto& flight : flights) {
				flight.settle = true;
			}
		}

		// The server's world state answered a_asked of our requests (Protocol::StorageCaughtUp); a_ours: how many we made
		// (LootSync), a_next: the first numbered change our world lacks, a_waiting: changes still wait for an earlier one.
		// The settle to send once it answers our latest request and nothing of it is missing (a gap is asked for again, and
		// its answer settles; a gap given up on instead settles at a later frame: see Marker); nothing otherwise. Moves of ours
		// our world lacks numbered from a_next on are forgotten: the session's log was rewound past them (its host's game quit
		// before saving them), and those numbers will be others' moves.
		std::optional<Protocol::StorageSettle> CaughtUp(std::uint32_t a_asked, std::uint32_t a_ours, std::uint32_t a_next, bool a_waiting)
		{
			marker = a_asked;
			if (!rule || !settling || a_asked != a_ours || a_waiting) {
				return std::nullopt;
			}
			settling = false;
			lacks.erase(lacks.lower_bound(a_next), lacks.end());
			lacksFrom = (std::min)(lacksFrom, a_next);
			Protocol::StorageSettle settle{ a_asked, a_next, lacksFrom, {}, {} };
			for (auto it = lacks.lower_bound(lacksFrom); it != lacks.end() && *it < a_next; ++it) {
				settle.lacking.push_back(*it);
			}
			for (auto& flight : flights) {
				if (std::exchange(flight.settle, false) && settle.late.size() < Protocol::MAX_STORAGE_MOVES_UNANSWERED) {
					settle.late.push_back(flight.change);
				}
			}
			++counts.settles;
			counts.late += static_cast<std::uint32_t>(settle.late.size());
			return settle;
		}

		// The last world state's marker (CaughtUp's a_asked) on this connection: while we are settling, the game tries it again
		// each frame, as what it waited for (a gap) may have been given up on since, with no other world state to come.
		[[nodiscard]] std::optional<std::uint32_t> Marker() const { return marker; }

		// Our moves are being settled: nothing is done at a friend's storage meanwhile.
		[[nodiscard]] bool Settling() const { return rule && settling; }
		void NoteSettlingClosed() { ++counts.settlingClosed; }

		void Owners(const Protocol::StorageOwners& a_owners)
		{
			for (const auto& owner : a_owners.entries) {
				if (owner.ownerId == 0) {
					Release(owner.container);  // given back: nobody keeps it
				} else if (owners.size() < MAX_OWNERS || owners.contains(owner.container)) {
					owners[owner.container] = { owner.ownerId, owner.since };
				}
			}
		}

		// A container nobody keeps any more (the host's game gave it back: a workbench of a settlement its player no longer
		// owns; Protocol::StorageRelease). It is each player's own again, here as everywhere, and the host's game lists it
		// afresh if it is ever its storage again.
		void Release(std::uint32_t a_container)
		{
			owners.erase(a_container);
			listed.erase(a_container);
			known.erase(a_container);
		}

		[[nodiscard]] bool Rule() const { return rule; }
		[[nodiscard]] bool Host() const { return host; }

		// Who keeps a container. a_ours: a player ID of ours.
		[[nodiscard]] Keeper KeeperOf(std::uint32_t a_container, const std::function<bool(std::uint32_t)>& a_ours) const
		{
			if (!rule) {
				return Keeper::kNone;
			}
			if (const auto it = owners.find(a_container); it != owners.end()) {
				return a_ours(it->second.id) ? Keeper::kUs : Keeper::kFriend;
			}
			return Keeper::kNone;
		}

		// Whether a settlement's workbench is shared storage here (LootScope's sharedBench): a game keeps it for the session (the
		// owner table names it), or ours is the host's and lists it (a_listing: the listing that claims it is being made). Any
		// other workbench stays each player's own, as with the rule off.
		[[nodiscard]] bool KeepsBench(std::uint32_t a_container, bool a_listing = false) const
		{
			return rule && (owners.contains(a_container) || (host && (a_listing || known.contains(a_container))));
		}

		[[nodiscard]] std::optional<std::uint32_t> OwnerOf(std::uint32_t a_container) const
		{
			const auto it = owners.find(a_container);
			return it != owners.end() ? std::optional{ it->second.id } : std::nullopt;
		}

		// The first change numbered while a_container was kept: a move of ours before it was ordinary loot, never undone in
		// the true copy by a settle (Own); the claim took our puts then back out of it (StorageBook::BeforeClaim).
		[[nodiscard]] std::uint32_t KeptSince(std::uint32_t a_container) const
		{
			const auto it = owners.find(a_container);
			return it != owners.end() ? it->second.since : 0;
		}

		[[nodiscard]] std::size_t OwnerCount() const { return owners.size(); }

		// ---- a friend's storage, viewed ----------------------------------------------------------------------

		// Our copy of a friend's container is to show the true copy: true when the server is to be told (not when we view it
		// already). A container menu views one container (Client::Open ends the others first); workshop mode and a crafting
		// menu view the host's workbench and the workbenches the host's supply lines link to it (Client::OpenAlso:
		// SupplyLines.h). a_aliased: shared items our copy holds a stack of that fills a quest alias (it can't leave the
		// copy): those stay our own for the view, all of them. The true copy's are not shown, and our moves of them stay in our
		// copy, so no move of one can be told from a move of the other.
		bool Open(std::uint32_t a_container, std::set<std::uint32_t> a_aliased = {})
		{
			if (Find(a_container)) {
				return false;
			}
			views.push_back(View{ a_container, NOT_YET, false, std::move(a_aliased) });
			++counts.views;
			return true;
		}

		// A view ended (our menu closed): what it was.
		std::optional<Ended> Close(std::uint32_t a_container)
		{
			const auto it = std::ranges::find(views, a_container, &View::container);
			if (it == views.end()) {
				return std::nullopt;
			}
			Ended ended{ it->container, it->closed, std::move(it->aliased) };
			views.erase(it);
			return ended;
		}

		// An item that stays our own in a container we view (see Open).
		[[nodiscard]] bool Aliased(std::uint32_t a_container, std::uint32_t a_item) const
		{
			const auto view = Find(a_container);
			return view && view->aliased.contains(a_item);
		}

		// Our copy of a container we view is to be shown afresh (after a refusal): true when the server is asked again (not
		// while its contents are still to come: they answer this too). Its changes wait for the contents, as at the open.
		bool Refresh(std::uint32_t a_container)
		{
			const auto view = Find(a_container);
			if (!view || view->closed || view->from == NOT_YET) {
				return false;
			}
			view->from = NOT_YET;
			return true;
		}

		// The container our menu is open on as a friend's storage (shown, or closed by the server): the first view (with a
		// workbench's supply lines, the workbench).
		[[nodiscard]] std::optional<std::uint32_t> Viewing() const { return views.empty() ? std::nullopt : std::optional{ views.front().container }; }

		// Every container we view, in the order the views opened.
		[[nodiscard]] std::vector<std::uint32_t> Viewed() const
		{
			std::vector<std::uint32_t> out;
			for (const auto& view : views) {
				out.push_back(view.container);
			}
			return out;
		}

		// Whether we view a_container (shown, or closed by the server).
		[[nodiscard]] bool Views(std::uint32_t a_container) const { return Find(a_container) != nullptr; }

		// Our copy of a_container shows the true copy (its contents asked for, or come), not closed by the server.
		[[nodiscard]] bool Shows(std::uint32_t a_container) const
		{
			const auto view = Find(a_container);
			return view && !view->closed;
		}

		// While our copy of a_container shows the true copy: the first numbered change its contents lack (NOT_YET until they
		// come). Changes from there on go into our copy; those before are in the contents.
		[[nodiscard]] std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container) const
		{
			const auto view = Find(a_container);
			return view && !view->closed ? std::optional{ view->from } : std::nullopt;
		}

		// What the server says the container we view holds: what our copy should show of the shared things, with our moves
		// on their way to it. Nothing for a container we don't show (an answer to a view since closed).
		std::optional<std::map<std::uint32_t, std::int64_t>> Contents(const Protocol::StorageListing& a_contents)
		{
			const auto view = Find(a_contents.container);
			if (!view || view->closed) {
				return std::nullopt;
			}
			view->from = a_contents.asOf;
			std::map<std::uint32_t, std::int64_t> want;
			for (const auto& stack : a_contents.stacks) {
				want[stack.item] += stack.count;
			}
			for (const auto& flight : flights) {
				if (flight.change.container == a_contents.container) {
					want[flight.change.item] += flight.change.count;
				}
			}
			std::erase_if(want, [&](const auto& a_entry) { return a_entry.second <= 0 || view->aliased.contains(a_entry.first); });
			return want;
		}

		// ---- our moves at a friend's storage -------------------------------------------------------------------

		// Our move is to go to the server: false (it isn't to be sent, and is undone) when MAX_IN_FLIGHT are unanswered.
		bool Reported(const Protocol::ContainerChange& a_change, Clock::time_point a_now)
		{
			if (flights.size() >= MAX_IN_FLIGHT) {
				++counts.overflow;
				return false;
			}
			flights.push_back({ .change = a_change, .due = a_now + ANSWER_WITHIN });
			return true;
		}

		// One of our moves at a friend's storage came back numbered, in session order (live, or in a catch-up). a_lacking:
		// the ledger says our world doesn't have it (it came in a catch-up from before it). One on its way from this world (a
		// flight) is in it. One that isn't, our world lacks: the true copy undoes it when we settle (StorageBook::Settle),
		// and our player keeps what our save has. So a quickload neither dupes what was put nor loses what was taken, and
		// nothing is taken back from a player who could have moved it away first.
		// A move numbered before the container was kept (StorageOwner::since) was ordinary loot: never lacking here (the
		// claim took the puts then back out of the true copy, whatever our world has).
		// A take kept with a save can come back smaller than made (the keeper's container held less, and the server's log
		// keeps the take as it was in the end; its shortfall went to the game we left): the rest is taken back.
		std::optional<Undo> Own(const Protocol::IndexedContainerChange& a_change, bool a_lacking)
		{
			const auto& change = a_change.change;
			if (change.count < 0) {
				if (numbered.size() >= MAX_NUMBERED) {
					numbered.erase(numbered.begin());
				}
				numbered[a_change.index] = change;
			}
			if (const auto it = std::ranges::find(flights, change, &Flight::change); it != flights.end()) {
				flights.erase(it);
				// The contents we show were made after the server numbered this move (a listing sent again while it was on its
				// way): they hold it already, and Contents counted it a second time on top. Taken out of our copy once more.
				if (const auto view = Find(change.container); view && !view->closed && view->from != NOT_YET && a_change.index < view->from) {
					return Undo{ .matched = true, .container = change.container, .item = change.item, .copy = -static_cast<std::int64_t>(change.count) };
				}
				return std::nullopt;
			}
			if (change.count <= 0) {
				const auto smaller = [&](const Flight& a_made) {
					return a_made.restored && a_made.change.container == change.container && a_made.change.item == change.item && a_made.change.count < change.count;
				};
				if (const auto it = std::ranges::find_if(flights, smaller); it != flights.end()) {
					const auto made = it->change;
					flights.erase(it);
					++counts.corrected;
					return Undo{ .matched = true, .refresh = Shows(change.container), .container = change.container, .item = change.item,
						.player = static_cast<std::int64_t>(made.count) - change.count };
				}
				if (change.count == 0) {
					return std::nullopt;  // nothing was taken
				}
			}
			if (a_lacking && a_change.index >= KeptSince(change.container) && a_change.index >= lacksFrom && !lacks.contains(a_change.index)) {
				if (lacks.size() >= MAX_LACKS) {
					lacksFrom = *lacks.begin() + 1;  // the server hears of it no more: a later save loaded doesn't have it done again
					lacks.erase(lacks.begin());
					++counts.lacksDropped;
				}
				lacks.insert(a_change.index);
				++counts.lacking;
			}
			return std::nullopt;
		}

		// The server refused one of our moves, or closed our view (item 0). What to undo; nothing for a move we don't
		// remember making in this world.
		Undo Refused(const Protocol::StorageRefusal& a_refusal)
		{
			Undo undo{ .container = a_refusal.container, .item = a_refusal.item };
			if (a_refusal.item == 0) {
				undo.matched = undo.closeView = Shows(a_refusal.container);
				if (undo.closeView) {
					Find(a_refusal.container)->closed = true;
					++counts.closedByHost;
				}
				return undo;
			}
			if (a_refusal.reason == Protocol::StorageReason::kShort) {
				const auto it = numbered.find(a_refusal.index);
				if (it == numbered.end() || it->second.container != a_refusal.container || it->second.item != a_refusal.item || a_refusal.count >= 0 ||
					a_refusal.count < it->second.count) {
					++counts.unmatched;
					return undo;
				}
				numbered.erase(it);
				undo.matched = true;
				undo.player = a_refusal.count;  // what the container never had: taken back from our player
				undo.refresh = Shows(a_refusal.container);
				++counts.shortTakes;
				return undo;
			}
			const Protocol::ContainerChange move{ a_refusal.container, a_refusal.item, a_refusal.count };
			if (a_refusal.item == Protocol::CONTAINER_EMPTIED) {
				++counts.unmatched;
				return undo;
			}
			if (const auto it = std::ranges::find(flights, move, &Flight::change); it != flights.end()) {
				flights.erase(it);
				++(a_refusal.count < 0 ? counts.refusedTakes : counts.refusedPuts);
				return Back(move);
			}
			++counts.unmatched;
			return undo;
		}

		// A move of ours the server refused that we never counted as on its way: it was made at a container nobody kept yet, and
		// reported as ordinary loot, as the host's game claimed it. It was made at our copy with no view of it: given back
		// (a take out of our player, a put to them), and put back in our copy.
		Undo Unflown(const Protocol::ContainerChange& a_move)
		{
			if (counts.unmatched > 0) {
				--counts.unmatched;  // Refused counted this refusal as one for no move we remember, just before
			}
			++(a_move.count < 0 ? counts.refusedTakes : counts.refusedPuts);
			return { .matched = true, .container = a_move.container, .item = a_move.item, .player = a_move.count, .copy = -static_cast<std::int64_t>(a_move.count) };
		}

		// Our moves with no answer past ANSWER_WITHIN, counted once each. They stay on their way: their number or refusal
		// may still come, and one the session never had goes late with our next settle.
		void Expired(Clock::time_point a_now)
		{
			for (auto& flight : flights) {
				if (!flight.counted && flight.due <= a_now) {
					flight.counted = true;
					++counts.unanswered;
				}
			}
		}

		// Our move couldn't be sent after all (the session's numbering has no ID for its item): undone, the newest such.
		std::optional<Undo> Unsent(const Protocol::ContainerChange& a_change)
		{
			const auto it = std::find_if(flights.rbegin(), flights.rend(), [&](const Flight& a_flight) { return a_flight.change == a_change; });
			if (it == flights.rend()) {
				return std::nullopt;
			}
			flights.erase(std::next(it).base());
			++counts.unsent;
			return Back(a_change);
		}

		[[nodiscard]] std::size_t InFlight() const { return flights.size(); }
		[[nodiscard]] std::size_t Lacks() const { return lacks.size(); }

		// For the co-save: our moves still unanswered, our numbered moves our world lacks (from LacksFrom on), and the
		// session they were made in.
		[[nodiscard]] std::vector<Protocol::ContainerChange> Unanswered() const
		{
			std::vector<Protocol::ContainerChange> out;
			out.reserve(flights.size());
			for (const auto& flight : flights) {
				out.push_back(flight.change);
			}
			return out;
		}

		[[nodiscard]] std::vector<std::uint32_t> Lacking() const { return { lacks.begin(), lacks.end() }; }
		[[nodiscard]] std::uint32_t              LacksFrom() const { return lacksFrom; }
		[[nodiscard]] std::uint64_t              MovesSession() const { return movesSession; }

		// ---- our own storage (the host's game) -----------------------------------------------------------------

		// Whether a container is to be listed now: not yet since the session began or our world last loaded (or a
		// friend's take found it short).
		[[nodiscard]] bool Listed(std::uint32_t a_container) const { return listed.contains(a_container); }

		void MarkListed(std::uint32_t a_container)
		{
			if (listed.size() < MAX_OWNERS) {
				listed.insert(a_container);
				if (known.size() < MAX_OWNERS) {
					known.insert(a_container);
				}
				++counts.listed;
			}
		}

		// Containers listed before (in this session) that are to be listed again, the server holding them stale (a load, a
		// rejoin): the scan tries these wherever the host is, so friends at a settlement the host isn't standing in are not
		// shut out of its chests until the host walks back. Not the ones listed already.
		[[nodiscard]] std::vector<std::uint32_t> ToRelist() const
		{
			std::vector<std::uint32_t> out;
			for (const auto container : known) {
				if (!listed.contains(container)) {
					out.push_back(container);
				}
			}
			return out;
		}

		// Ours listed at any time in this session and not given back since.
		[[nodiscard]] std::vector<std::uint32_t> Known() const { return { known.begin(), known.end() }; }

		// Listed again soon (a shortfall, our own menu closed on it).
		void Unlist(std::uint32_t a_container) { listed.erase(a_container); }

		// Every container is listed again (the server holds our listings stale: a load or a hello; not a gap asked for again).
		void UnlistAll() { listed.clear(); }

		void NoteShort() { ++counts.shorts; }

		[[nodiscard]] const Counts& Stats() const { return counts; }

	private:
		struct View
		{
			std::uint32_t           container = 0;
			std::uint32_t           from = NOT_YET;
			bool                    closed = false;  // the server closed it: the menu is still open on our own copy
			std::set<std::uint32_t> aliased;         // shared items left our own for the view (Open)
		};

		struct Owner
		{
			std::uint32_t id = 0;     // the player whose game keeps it
			std::uint32_t since = 0;  // the first change numbered while kept
		};

		struct Flight
		{
			Protocol::ContainerChange change;
			Clock::time_point         due{};             // counted unanswered if no answer has come by then
			bool                      settle = false;    // made before our last request: late at its settle if still unnumbered
			bool                      restored = false;  // kept with the save we loaded
			bool                      counted = false;   // counted unanswered
		};

		View* Find(std::uint32_t a_container)
		{
			const auto it = std::ranges::find(views, a_container, &View::container);
			return it != views.end() ? &*it : nullptr;
		}

		const View* Find(std::uint32_t a_container) const
		{
			const auto it = std::ranges::find(views, a_container, &View::container);
			return it != views.end() ? &*it : nullptr;
		}

		// Our move undone: a take comes back out of our player (and into our copy, while it shows the true copy); a put goes
		// back to them.
		Undo Back(const Protocol::ContainerChange& a_change) const
		{
			const bool shown = Shows(a_change.container);
			return { .matched = true, .refresh = shown, .container = a_change.container, .item = a_change.item, .player = a_change.count, .copy = shown ? -static_cast<std::int64_t>(a_change.count) : 0 };
		}

		bool                                             rule = false;
		bool                                             host = false;
		bool                                             settling = false;  // asked the server, not settled since (CaughtUp)
		std::optional<std::uint32_t>                     marker;            // the last kStorageCaughtUp's (Marker)
		std::unordered_map<std::uint32_t, Owner>         owners;  // container -> whose game keeps it, since when
		std::vector<View>                                views;  // in the order they opened (a container menu's one; a workbench and its supply lines')
		std::uint64_t                                    movesSession = 0;  // the session our flights and lacks are in
		std::deque<Flight>                               flights;   // ours, oldest first, not yet numbered or refused
		std::set<std::uint32_t>                          lacks;     // our numbered moves our world lacks (from lacksFrom on)
		std::uint32_t                                    lacksFrom = 0;
		std::map<std::uint32_t, Protocol::ContainerChange> numbered;  // our numbered takes, by index
		std::unordered_set<std::uint32_t>                listed;    // ours listed since our world last loaded
		std::unordered_set<std::uint32_t>                known;     // ours listed at any time in this session (ToRelist)
		Counts                                           counts;
	};

	// What a friend's game tells its player.
	enum class Notice : std::uint8_t
	{
		kClosed,       // the true copy can't be shown now (its keeper away, or their world reloaded): the menu closes
		kTakeRefused,  // a take undone: given back
		kPutRefused,   // a put undone: returned
		kShort,        // a take the keeper's container held less of: what it lacked is given back
		kSettleCut,    // the server could not put our moves right after a load: storage others keep stays closed to us until a later load
		kBuildShort,   // a build whose components another player used first, and we couldn't pay for: taken back, what it took returned
		kCount,
	};

	// What became of a build Client asked the game to take back (Engine::TakeBackBuild).
	enum class TakeBack : std::uint8_t
	{
		kDone,   // the piece is gone
		kLater,  // not now (the player holds it): asked again
		kNo,     // it stays (not a piece that can be taken back, or gone on to the host already)
	};

	// A change to our player's inventory: made only by Client, for one of our own moves put right (OwnGear's
	// kStorageUndo, the one change any of the session's code makes to the local player).
	class PlayerChange
	{
	public:
		[[nodiscard]] std::uint32_t Item() const { return item; }
		[[nodiscard]] std::int64_t  Count() const { return count; }

	private:
		friend class Client;

		PlayerChange(std::uint32_t a_item, std::int64_t a_count) :
			item(a_item), count(a_count)
		{}

		std::uint32_t item = 0;
		std::int64_t  count = 0;
	};

	// What Client needs of the game (Storage.cpp; LootGame in the tests).
	class Engine
	{
	public:
		virtual ~Engine() = default;

		// What our copy of a container holds of what the session shares (LootScope's: ammo, aid, junk), item by item.
		virtual std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_container) = 0;
		// The shared items our copy of a container holds a stack of that fills a quest alias (LootSync::Aliased).
		virtual std::set<std::uint32_t> Aliased(std::uint32_t) { return {}; }
		// Whether the session shares an item held in a container (LootScope's kShared, as SharedIn counts it).
		virtual bool SharedItem(std::uint32_t a_container, std::uint32_t a_item) { return SharedIn(a_container).contains(a_item); }
		// Whether a refused take at a_container is paid from what we set aside there before our player: a workbench our
		// workshop mode or crafting menu reads, where the take was a build's (the engine's, never into our player), and our
		// game would have used its own stash there first.
		virtual bool PaysFromStash(std::uint32_t) { return false; }
		// Whether a take the engine makes at a_container now is a build's: workshop mode reads it (the workbench, or one
		// the host's supply lines link to it).
		virtual bool BuildsFrom(std::uint32_t) { return false; }
		// A build Client followed (BuildPlaced) can't be paid for: the piece is to go, with nothing told to the session.
		virtual TakeBack TakeBackBuild(std::uint32_t) { return TakeBack::kNo; }
		// A build Client followed stands (paid, or no longer waited for): what the game held back for it goes on.
		virtual void BuildSettled(std::uint32_t) {}
		// Adds to our copy of a container we view, as a put of ours there (reported with our other moves: a build taken
		// back returns what the true copy gave it); what was added.
		virtual std::int64_t PutBack(std::uint32_t, std::uint32_t, std::int64_t) { return 0; }
		// Adds to (> 0) or takes out of (< 0) our copy of a container; what was changed (a take takes no more than there is).
		virtual std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count) = 0;
		// The same for our player's inventory.
		virtual std::int64_t ChangePlayer(const PlayerChange& a_change) = 0;
		// The player's moves since the last frame are reported now (WorldSync::FlushLoot): they come back through
		// Client::Reporting.
		virtual void FlushLoot() = 0;
		// WorldSync reported this move of ours as ordinary loot, from the world we have now (forgets the report).
		virtual bool Reported(const Protocol::ContainerChange&) { return false; }
		virtual void Send(std::vector<std::uint8_t> a_packet) = 0;
		virtual void Notify(Notice a_notice) = 0;
		// The container menu closes, if it is open (Client::MenuClosed follows).
		virtual void HideMenu() = 0;
		virtual Clock::time_point Now() = 0;
	};

	// A friend's game, all of what it does with a friend's storage: the rules the engine glue follows, kept here so the
	// tests run them as the game does.
	class Client
	{
	public:
		struct Counts
		{
			std::uint32_t setAside = 0;   // stacks of our copy set aside as a view opened
			std::uint32_t shown = 0;      // contents shown in our copy
			std::uint32_t adjusts = 0;    // stacks our copy was changed by to show them
			std::uint32_t takenBack = 0;  // refused and short takes: out of our player
			std::int64_t  usedUp = 0;     // of those, items our player no longer had
			std::uint32_t givenBack = 0;  // refused puts: to our player
			std::uint32_t notices = 0;    // notices shown (NOTICE_EVERY at most, each)
			std::int64_t  localKept = 0;  // items our own scripts added to or took from a copy we viewed, kept out of the true copy (Local)
			std::int64_t  localUnpaid = 0;  // of those taken, what our set-aside stash lacked (put back in the copy all the same)
			std::int64_t  stashPaid = 0;  // of refused takes at a workbench we view, what our set-aside stash there paid (PaysFromStash)
			std::uint32_t buildsFollowed = 0;    // pieces placed in workshop mode at a workbench we view (BuildPlaced)
			std::uint32_t buildsTakenBack = 0;   // of those, taken back: a refused take neither our stash nor our pockets could pay
			std::uint32_t buildsKept = 0;        // short all the same, and standing: the game couldn't take the piece back
			std::uint32_t buildsUnsettled = 0;   // let go unjudged: a take still unanswered after BUILD_SETTLE_WITHIN, or two pieces placed in one moment
		};

		explicit Client(Engine& a_engine) :
			engine(a_engine)
		{}

		Side&                             State() { return side; }
		[[nodiscard]] const Side&         State() const { return side; }
		[[nodiscard]] const Counts&       Stats() const { return counts; }
		// What our copy's own shared things are, set aside while it shows the true copy: of the container our menu is open
		// on (Side::Viewing), or of any container we view.
		[[nodiscard]] const std::map<std::uint32_t, std::int64_t>& Aside() const
		{
			const auto viewing = side.Viewing();
			return viewing ? Aside(*viewing) : NOTHING;
		}

		[[nodiscard]] const std::map<std::uint32_t, std::int64_t>& Aside(std::uint32_t a_container) const
		{
			const auto it = asides.find(a_container);
			return it != asides.end() ? it->second : NOTHING;
		}

		// ---- the session ----------------------------------------------------------------------------------

		void OnWelcome(bool a_rule, bool a_host, std::uint64_t a_session)
		{
			EndViews(false);
			ForgetBuilds();
			side.OnWelcome(a_rule, a_host, a_session, engine.Now());
		}

		// The connection went: the moves made in the menu go with the rest, every unanswered one stays on its way (the
		// rejoin settles it), and our copy is our own again.
		void OnDisconnected()
		{
			if (side.Viewing()) {
				engine.FlushLoot();
			}
			EndViews(false);
			ForgetBuilds();
			side.OnDisconnected();
		}

		// Before a save or a load (after WorldSync::FlushLoot): no save holds the session's things in our copy, nor misses
		// what was set aside. A build still followed stands as it is (the game sends on what it held back for it).
		void BeforeSaveOrLoad()
		{
			EndViews(true);
			ForgetBuilds();
		}

		// A save loaded, in a_session (see Side::OnLoad).
		void OnLoad(std::uint64_t a_session, std::span<const Protocol::ContainerChange> a_unanswered, std::span<const std::uint32_t> a_lacks = {},
			std::uint32_t a_lacksFrom = 0)
		{
			asides.clear();  // the save's copy holds it
			ForgetBuilds();
			side.OnLoad(a_session, a_unanswered, a_lacks, a_lacksFrom, engine.Now());
		}

		// Every frame: moves unanswered too long are counted (they stay on their way: see Side::Expired), and the builds
		// followed are judged (BuildPlaced).
		void Frame()
		{
			const auto now = engine.Now();
			side.Expired(now);
			BuildsFrame(now);
		}

		// ---- a build paid from a friend's workbench ---------------------------------------------------------------
		//
		// Workshop mode at a friend's workbench builds with the true copy our copy shows: the engine takes the components
		// out of our copy (moves of ours, numbered or refused by the server as any) and, for what the workbenches lack, out
		// of our player. A take the server refuses (another player used those in the same moment) is paid from our own stash
		// there, then from our pockets (Apply). When neither holds enough the piece would stand unpaid, so the build is taken
		// back instead: the game is told a piece was placed (BuildPlaced: it holds back what it would tell the session of
		// it), the takes and the player's own things used within BUILD_PAIR_WITHIN of that are the piece's, and once every
		// one of them is answered the build is judged. Paid in full: it stands (Engine::BuildSettled). Short: the piece goes
		// (Engine::TakeBackBuild), what the true copy gave goes back into it, what our stash and pockets paid comes back, and
		// the player is told (Notice::kBuildShort) in place of the refusals' own notices.
		//
		// Left as before (the build stands, what was lacking counted in usedUp): a take answered only after
		// BUILD_SETTLE_WITHIN, a shortfall the keeper's game finds later (kShort), two pieces placed within one
		// BUILD_PAIR_WITHIN (whose the takes were can't be told), and a piece the game can't take back.

		// Our workshop mode placed a_piece at a_bench. True when the build is followed: the caller holds back what it would
		// tell the session until BuildSettled or TakeBackBuild is called for it. False: nothing to follow (we don't view
		// a_bench, or too many are followed).
		bool BuildPlaced(std::uint32_t a_piece, std::uint32_t a_bench)
		{
			if (a_piece == 0 || !side.Shows(a_bench) || !engine.BuildsFrom(a_bench)) {
				return false;
			}
			if (std::ranges::any_of(builds, [&](const Build& a_build) { return a_build.piece == a_piece; })) {
				return true;
			}
			if (builds.size() >= MAX_BUILDS) {
				return false;
			}
			builds.push_back(Build{ .piece = a_piece, .placedAt = engine.Now() });
			++counts.buildsFollowed;
			return true;
		}

		// The engine used a_count of our player's own a_item with nothing to show for it, in workshop mode at a workbench we
		// view: maybe a build's, for what the workbenches lacked. Returned if that build is taken back.
		void PlayerUsed(std::uint32_t a_item, std::int64_t a_count)
		{
			if (a_item != 0 && a_count > 0 && !side.Viewed().empty() && looseUsed.size() < MAX_BUILD_TAKES) {
				looseUsed.push_back(Used{ a_item, a_count, engine.Now() });
			}
		}

		// The pieces followed and not yet judged.
		[[nodiscard]] std::size_t BuildsFollowed() const { return builds.size(); }

		// Our world asked the server for the session's changes again (a gap): see Side::Asked.
		void Asked() { side.Asked(); }

		// The server's world state answered a_asked of our requests: our moves are settled (Side::CaughtUp).
		void OnCaughtUp(std::uint32_t a_asked, std::uint32_t a_ours, std::uint32_t a_next, bool a_waiting)
		{
			if (const auto settle = side.CaughtUp(a_asked, a_ours, a_next, a_waiting)) {
				engine.Send(Protocol::Encode(*settle));
			}
		}

		// Each frame while settling: the last marker again (Side::Marker). True when it settled.
		bool RetryCaughtUp(std::uint32_t a_ours, std::uint32_t a_next, bool a_waiting)
		{
			const auto marker = side.Marker();
			if (!side.Settling() || !marker || *marker != a_ours || a_waiting) {
				return false;
			}
			OnCaughtUp(*marker, a_ours, a_next, a_waiting);
			return !side.Settling();
		}

		// ---- the container menu ----------------------------------------------------------------------------

		// Our menu shows a_container; a_friends: another game keeps it.
		void Open(std::uint32_t a_container, bool a_friends)
		{
			if (!side.Rule() || !a_friends) {
				return;
			}
			// Our moves since our world last loaded aren't settled yet (the server refuses the view meanwhile): the menu closes.
			if (side.Settling()) {
				side.NoteSettlingClosed();
				engine.HideMenu();
				Notify(Notice::kClosed);
				return;
			}
			// Our moves before it (at another container, or this one before it was a friend's) go first, as theirs.
			engine.FlushLoot();
			for (const auto viewed : side.Viewed()) {
				if (viewed != a_container) {
					EndView(viewed, true);
				}
			}
			OpenView(a_container);
		}

		// Our workshop mode or crafting menu, viewing a friend's workbench, views a_container too: a workbench the host's
		// supply lines link to it (SupplyLines.h). The views already open stay. True when it opened (not with the rule off,
		// while our moves are settling, nor when we view it already).
		bool OpenAlso(std::uint32_t a_container)
		{
			if (!side.Rule() || side.Settling() || side.Views(a_container)) {
				return false;
			}
			engine.FlushLoot();
			OpenView(a_container);
			return side.Views(a_container);
		}

		// One view ends (a workbench the host's supply lines no longer link to ours, or one the server closed); the rest stay.
		void CloseOne(std::uint32_t a_container)
		{
			if (!side.Views(a_container)) {
				return;
			}
			engine.FlushLoot();
			EndView(a_container, true);
		}

		// The server says who keeps what. a_menuOn: the container our menu is open on, if any. The moves made before we knew go
		// out first as what they were (ordinary loot: the server refuses the ones that came after a claim, and OnRefused undoes
		// them); then, if our menu is open on a container a friend keeps now, the view opens as if the menu had just been shown:
		// our copy's stacks are set aside and the true copy comes (the menu was shown before the claim, so nothing else would).
		void OnOwners(const Protocol::StorageOwners& a_owners, std::optional<std::uint32_t> a_menuOn, const std::function<bool(std::uint32_t)>& a_ours)
		{
			engine.FlushLoot();
			// One given back (no owner) that we still view: the view ends as when the server closes it (its word that it closed
			// comes first; this is for one that was lost on the way).
			for (const auto& owner : a_owners.entries) {
				if (owner.ownerId == 0 && side.Shows(owner.container)) {
					OnRefused(Protocol::StorageRefusal{ owner.container, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed });
				}
			}
			side.Owners(a_owners);
			if (a_menuOn && !side.Viewing() && side.KeeperOf(*a_menuOn, a_ours) == Keeper::kFriend) {
				Open(*a_menuOn, true);
			}
		}

		// Our menu closed. Nothing to do unless it showed a friend's storage.
		void MenuClosed()
		{
			if (!side.Viewing()) {
				return;
			}
			engine.FlushLoot();  // the moves made in the menu go first: they were made while it showed the session's things
			EndViews(true);
		}

		// ---- the server ------------------------------------------------------------------------------------

		void OnContents(const Protocol::StorageListing& a_contents)
		{
			if (!side.Shows(a_contents.container)) {
				return;
			}
			engine.FlushLoot();  // our moves since the last frame: on their way, they count in what our copy should show
			const auto want = side.Contents(a_contents);
			if (!want) {
				return;
			}
			++counts.shown;
			for (const auto& adjust : Reconcile(*want, SharedIn(a_contents.container))) {
				engine.ChangeCopy(a_contents.container, adjust.item, adjust.count);
				++counts.adjusts;
			}
		}

		void OnRefused(const Protocol::StorageRefusal& a_refusal)
		{
			if (a_refusal.item == 0) {
				if (a_refusal.container == 0) {
					// Not one container: our settle was cut. The server refuses every view and move of ours until a later load.
					Notify(Notice::kSettleCut);
					return;
				}
				if (!side.Shows(a_refusal.container)) {
					return;
				}
				// The server closed our view. The moves made in the menu go first (they are refused, and undone); then the
				// session's things leave our copy, and the menu closes. What was set aside comes back as it does.
				engine.FlushLoot();
				if (side.Refused(a_refusal).closeView) {
					for (const auto& [item, count] : SharedIn(a_refusal.container)) {
						engine.ChangeCopy(a_refusal.container, item, -count);
					}
					engine.HideMenu();
					Notify(Notice::kClosed);
				}
				return;
			}
			const auto undo = side.Refused(a_refusal);
			if (!undo.matched && a_refusal.index == Protocol::STORAGE_NOT_NUMBERED && a_refusal.item != Protocol::CONTAINER_EMPTIED &&
				(a_refusal.reason == Protocol::StorageReason::kNotViewing || a_refusal.reason == Protocol::StorageReason::kClosed)) {
				const Protocol::ContainerChange move{ a_refusal.container, a_refusal.item, a_refusal.count };
				if (engine.Reported(move)) {
					// Reported as ordinary loot in the moment the host's game claimed the container (nobody kept it when we made the
					// move): given back to our player and to our copy, as a refused move on its way is.
					UndoUnflown(side.Unflown(move));
					return;
				}
			}
			if (a_refusal.reason == Protocol::StorageReason::kShort) {
				Apply(undo, Notice::kShort);
				return;
			}
			// A build's take, refused whole: what it lacks decides whether the build stands (BuildsFrame).
			const auto take = undo.matched && undo.player < 0 ? FlyingTake({ a_refusal.container, a_refusal.item, a_refusal.count }) : nullptr;
			ApplyTake(take, undo, undo.player < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
		}

		// ---- WorldSync's loot ----------------------------------------------------------------------------------

		// Our move at a friend's storage, about to be reported: false when it isn't (too many unanswered), and it is undone.
		bool Reporting(const Protocol::ContainerChange& a_change)
		{
			const bool shown = side.Shows(a_change.container);
			// The engine's take at a workbench workshop mode reads: maybe a build's (BuildPlaced).
			const bool build = a_change.count < 0 && shown && engine.BuildsFrom(a_change.container);
			if (side.Reported(a_change, engine.Now())) {
				if (build) {
					NoteTake(a_change);
				}
				return true;
			}
			ApplyTake(build ? NoteTake(a_change) : nullptr,
				{ .matched = true, .container = a_change.container, .item = a_change.item, .player = a_change.count, .copy = shown ? -static_cast<std::int64_t>(a_change.count) : 0 },
				a_change.count < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
			return false;
		}

		// One of our moves at a friend's storage, numbered (see Side::Own).
		void OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking)
		{
			if (const auto redo = side.Own(a_change, a_lacking)) {
				Apply(*redo, std::nullopt);
			}
			if (a_change.change.count < 0) {
				if (const auto take = FlyingTake(a_change.change)) {
					take->state = BuildTake::State::kNumbered;  // the true copy gave it
				}
			}
		}

		// Our move couldn't be sent (Session: the session's numbering has no ID for its item).
		void Unsent(const Protocol::ContainerChange& a_change)
		{
			if (const auto undo = side.Unsent(a_change)) {
				ApplyTake(undo->player < 0 ? FlyingTake(a_change) : nullptr, *undo, undo->player < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
			}
		}

		// A change our own game's scripts made to our copy of a container while it shows the true copy (a settlement's daily
		// food and water, its settlers' meals, at the host's workbench): our world's, never the session's, or every game
		// would make the same day's food once more in the one pile. What a script added goes from the copy to what was set
		// aside (ours again as the view ends); what it took out of the true copy shown is put back and taken from what was
		// set aside instead, as far as that holds (what it lacks is counted, localUnpaid). Only the session's shared things
		// (a weapon a script adds stays in our copy, as it would with no view). Nothing without a view.
		void Local(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count)
		{
			if (a_count == 0 || !side.Shows(a_container) || side.Aliased(a_container, a_item) || !engine.SharedItem(a_container, a_item)) {
				return;
			}
			auto& aside = asides[a_container];
			if (a_count > 0) {
				const auto moved = -engine.ChangeCopy(a_container, a_item, -a_count);
				if (moved > 0) {
					aside[a_item] += moved;
					counts.localKept += moved;
				}
				return;
			}
			const auto back = engine.ChangeCopy(a_container, a_item, -a_count);
			auto&      held = aside[a_item];
			const auto paid = (std::min)(held, back);
			held -= paid;
			if (held <= 0) {
				aside.erase(a_item);
			}
			counts.localKept += back;
			counts.localUnpaid += back - paid;
		}

	private:
		// What our copy holds of the session's things, less what stays our own for the view (Side::Open).
		std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_container)
		{
			auto out = engine.SharedIn(a_container);
			std::erase_if(out, [&](const auto& a_entry) { return side.Aliased(a_container, a_entry.first); });
			return out;
		}

		// A move of ours made at our copy with no view of it, refused (Side::Unflown). If a view of the container has opened since
		// (OnOwners), our copy shows the true copy now and its own stacks are set aside: the move is put back in those, so they
		// come back as the copy had them before it.
		void UndoUnflown(const Undo& a_undo)
		{
			const auto done = engine.ChangePlayer(PlayerChange{ a_undo.item, a_undo.player });
			if (a_undo.player < 0) {
				++counts.takenBack;
				counts.usedUp += done - a_undo.player;
			} else {
				++counts.givenBack;
			}
			if (side.Shows(a_undo.container)) {
				auto& aside = asides[a_undo.container];
				auto& held = aside[a_undo.item];
				held = (std::max<std::int64_t>)(held + a_undo.copy, 0);
				if (held == 0) {
					aside.erase(a_undo.item);
				}
			} else {
				engine.ChangeCopy(a_undo.container, a_undo.item, a_undo.copy);
			}
			Notify(a_undo.player < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
		}

		// What a take put right cost us (Apply): from our stash set aside at the workbench, from our player, and what
		// neither held.
		struct Paid
		{
			std::int64_t stash = 0;
			std::int64_t pockets = 0;
			std::int64_t lacked = 0;
		};

		// One of the engine's takes at a workbench workshop mode reads (Reporting): a build's, if a piece is placed within
		// BUILD_PAIR_WITHIN of it.
		struct BuildTake
		{
			enum class State : std::uint8_t
			{
				kFlying,    // reported, not answered
				kNumbered,  // the true copy gave it
				kRefused,   // undone: paid by our stash and pockets, as far as they held it
			};

			Protocol::ContainerChange change;  // count < 0
			State                     state = State::kFlying;
			Paid                      paid;    // kRefused
			std::optional<Notice>     notice;  // kRefused: its notice, held until the build is judged
			Clock::time_point         at{};
		};

		struct Used
		{
			std::uint32_t     item = 0;
			std::int64_t      count = 0;
			Clock::time_point at{};
		};

		struct Build
		{
			std::uint32_t          piece = 0;
			Clock::time_point      placedAt{};
			bool                   paired = false;  // its takes are known (BUILD_PAIR_WITHIN has passed)
			bool                   judged = true;   // false: two pieces in one moment, it only waits to be let go
			std::vector<BuildTake> takes;
			std::vector<Used>      used;
		};

		// A build's take, remembered until a piece claims it or BUILD_PAIR_WITHIN passes. None when too many are.
		BuildTake* NoteTake(const Protocol::ContainerChange& a_change)
		{
			if (looseTakes.size() >= MAX_BUILD_TAKES) {
				return nullptr;
			}
			looseTakes.push_back(BuildTake{ .change = a_change, .at = engine.Now() });
			return &looseTakes.back();
		}

		// The oldest take like a_change still unanswered, a followed build's before one no piece has claimed.
		BuildTake* FlyingTake(const Protocol::ContainerChange& a_change)
		{
			const auto in = [&](std::vector<BuildTake>& a_takes) -> BuildTake* {
				const auto it = std::ranges::find_if(a_takes, [&](const BuildTake& a_take) { return a_take.state == BuildTake::State::kFlying && a_take.change == a_change; });
				return it != a_takes.end() ? &*it : nullptr;
			};
			for (auto& build : builds) {
				if (const auto take = in(build.takes)) {
					return take;
				}
			}
			return in(looseTakes);
		}

		// A refused (or unsent) move is undone. A build's take (a_take) keeps what the undo cost, and while a piece is
		// followed its notice waits: a build taken back shows its own instead.
		void ApplyTake(BuildTake* a_take, const Undo& a_undo, Notice a_notice)
		{
			if (!a_take) {
				Apply(a_undo, a_notice);
				return;
			}
			const bool hold = !builds.empty();
			a_take->state = BuildTake::State::kRefused;
			a_take->paid = Apply(a_undo, hold ? std::nullopt : std::optional<Notice>{ a_notice });
			if (hold) {
				a_take->notice = a_notice;
			}
		}

		void ShowHeld(const std::vector<BuildTake>& a_takes)
		{
			for (const auto& take : a_takes) {
				if (take.notice) {
					Notify(*take.notice);
				}
			}
		}

		// A build is taken back: everything its takes cost goes back where it came from.
		void Refund(const Build& a_build)
		{
			for (const auto& take : a_build.takes) {
				const auto container = take.change.container;
				const auto item = take.change.item;
				const bool shown = side.Shows(container);
				if (take.state == BuildTake::State::kNumbered) {
					// The true copy gave these: back into it, as a put of ours. What our copy couldn't take (or all, if we no
					// longer view it) goes to our pockets.
					const auto count = -static_cast<std::int64_t>(take.change.count);
					const auto back = shown ? engine.PutBack(container, item, count) : 0;
					if (count > back) {
						engine.ChangePlayer(PlayerChange{ item, count - back });
					}
					continue;
				}
				if (take.paid.stash > 0) {
					if (shown) {
						asides[container][item] += take.paid.stash;
					} else {
						engine.ChangeCopy(container, item, take.paid.stash);  // the view ended: what was set aside is in the copy again
					}
					counts.stashPaid -= take.paid.stash;
				}
				if (take.paid.pockets > 0) {
					engine.ChangePlayer(PlayerChange{ item, take.paid.pockets });
				}
				counts.usedUp -= take.paid.lacked;
			}
			for (const auto& used : a_build.used) {
				engine.ChangePlayer(PlayerChange{ used.item, used.count });
			}
		}

		// Every frame: each followed build gets its takes, then its judgement (see BuildPlaced).
		void BuildsFrame(Clock::time_point a_now)
		{
			const auto unpaired = [](const Build& a_build) { return !a_build.paired; };
			if (const auto first = std::ranges::find_if(builds, unpaired); first == builds.end()) {
				// No piece waits for them: what is older than the window was no build's (a repair's take).
				std::erase_if(looseTakes, [&](const BuildTake& a_take) {
					if (a_now - a_take.at < BUILD_PAIR_WITHIN) {
						return false;
					}
					if (a_take.notice) {
						Notify(*a_take.notice);
					}
					return true;
				});
				std::erase_if(looseUsed, [&](const Used& a_used) { return a_now - a_used.at >= BUILD_PAIR_WITHIN; });
			} else if (a_now - first->placedAt >= BUILD_PAIR_WITHIN) {
				// Two pieces placed in one moment: whose the takes were can't be told, so neither is judged.
				const bool alone = std::ranges::count_if(builds, unpaired) == 1;
				first->takes = std::exchange(looseTakes, {});
				first->used = std::exchange(looseUsed, {});
				for (auto& build : builds) {
					if (!build.paired) {
						build.paired = true;
						build.judged = alone;
					}
				}
			}
			for (std::size_t i = 0; i < builds.size();) {
				if (!builds[i].paired) {
					++i;
					continue;
				}
				const bool late = a_now - builds[i].placedAt >= BUILD_SETTLE_WITHIN;
				const bool flying = std::ranges::any_of(builds[i].takes, [](const BuildTake& a_take) { return a_take.state == BuildTake::State::kFlying; });
				if (flying && !late && builds[i].judged) {
					++i;
					continue;
				}
				// Out of the list before the game is called for it.
				auto build = std::move(builds[i]);
				builds.erase(builds.begin() + static_cast<std::ptrdiff_t>(i));
				std::int64_t lacked = 0;
				for (const auto& take : build.takes) {
					lacked += take.paid.lacked;
				}
				if (build.judged && !flying && lacked > 0) {
					const auto result = engine.TakeBackBuild(build.piece);
					if (result == TakeBack::kLater && !late) {
						builds.insert(builds.begin() + static_cast<std::ptrdiff_t>(i), std::move(build));
						++i;
						continue;
					}
					if (result == TakeBack::kDone) {
						Refund(build);
						++counts.buildsTakenBack;
						Notify(Notice::kBuildShort);
						continue;
					}
					++counts.buildsKept;
				} else if (flying || !build.judged) {
					++counts.buildsUnsettled;
				}
				ShowHeld(build.takes);
				engine.BuildSettled(build.piece);
			}
		}

		// The followed builds are the old world's or the old connection's: nothing more is done for them here.
		void ForgetBuilds()
		{
			builds.clear();
			looseTakes.clear();
			looseUsed.clear();
		}

		Paid Apply(const Undo& a_undo, std::optional<Notice> a_notice)
		{
			Paid paid;
			if (!a_undo.matched) {
				return paid;
			}
			if (a_undo.player != 0) {
				auto player = a_undo.player;
				if (player < 0 && side.Shows(a_undo.container) && engine.PaysFromStash(a_undo.container)) {
					// A build's components the true copy refused: our own stash at the workbench pays first, as our game's
					// build would have taken them from it. It comes back to the copy as the view ends, less this.
					auto& aside = asides[a_undo.container];
					if (const auto held = aside.find(a_undo.item); held != aside.end()) {
						paid.stash = (std::min)(held->second, -player);
						held->second -= paid.stash;
						if (held->second <= 0) {
							aside.erase(held);
						}
						player += paid.stash;
						counts.stashPaid += paid.stash;
					}
				}
				if (a_undo.player < 0) {
					++counts.takenBack;
					// What our player no longer had (used, moved away in the moment it took the server to answer): for the
					// log. Only a refusal or a shortfall takes back, never what a load or a lost connection left wrong (the
					// true copy is put right for those: Side::Own).
					if (player < 0) {
						const auto done = engine.ChangePlayer(PlayerChange{ a_undo.item, player });
						counts.usedUp += done - player;
						paid.pockets = -done;
						paid.lacked = done - player;
					}
				} else {
					engine.ChangePlayer(PlayerChange{ a_undo.item, player });
					++counts.givenBack;
				}
			}
			if (a_undo.copy != 0 && side.Shows(a_undo.container)) {
				engine.ChangeCopy(a_undo.container, a_undo.item, a_undo.copy);
			}
			if (a_undo.refresh && side.Refresh(a_undo.container)) {
				engine.Send(Protocol::Encode(Protocol::StorageView{ a_undo.container, true }));
			}
			if (a_notice) {
				Notify(*a_notice);
			}
			return paid;
		}

		// A view opens: our copy is to show the true copy (Open, OpenAlso).
		void OpenView(std::uint32_t a_container)
		{
			auto aliased = engine.Aliased(a_container);
			if (!side.Open(a_container, aliased)) {
				return;
			}
			// What our copy held of the session's things is set aside, out of reach: our player's stash, or the host's
			// things it mirrored in an older session (theirs to take only from the true copy). An item a quest alias holds a
			// stack of stays, all of it: it is ours for the view (Side::Open).
			auto& aside = asides[a_container];
			aside.clear();
			for (const auto& [item, count] : SharedIn(a_container)) {
				const auto moved = -engine.ChangeCopy(a_container, item, -count);
				if (moved > 0) {
					aside[item] += moved;
					++counts.setAside;
				}
			}
			engine.Send(Protocol::Encode(Protocol::StorageView{ a_container, true }));
		}

		// Every view ends, the last opened first.
		void EndViews(bool a_tellServer)
		{
			auto viewed = side.Viewed();
			for (auto it = viewed.rbegin(); it != viewed.rend(); ++it) {
				EndView(*it, a_tellServer);
			}
		}

		// A view ends: the session's things leave our copy (unless the server's close took them already), and what was set
		// aside comes back.
		void EndView(std::uint32_t a_container, bool a_tellServer)
		{
			const auto ended = side.Close(a_container);
			if (!ended) {
				return;
			}
			if (!ended->closed) {
				for (const auto& [item, count] : engine.SharedIn(ended->container)) {
					if (!ended->aliased.contains(item)) {
						engine.ChangeCopy(ended->container, item, -count);
					}
				}
			}
			if (const auto it = asides.find(ended->container); it != asides.end()) {
				for (const auto& [item, count] : it->second) {
					engine.ChangeCopy(ended->container, item, count);
				}
				asides.erase(it);
			}
			if (a_tellServer && !ended->closed) {
				engine.Send(Protocol::Encode(Protocol::StorageView{ ended->container, false }));
			}
		}

		void Notify(Notice a_notice)
		{
			const auto now = engine.Now();
			auto&      last = noticed[static_cast<std::size_t>(a_notice)];
			if (last && now - *last < NOTICE_EVERY) {
				return;
			}
			last = now;
			++counts.notices;
			engine.Notify(a_notice);
		}

		Engine&                                                          engine;
		Side                                                             side;
		inline static const std::map<std::uint32_t, std::int64_t>        NOTHING;
		std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>>   asides;  // container -> our copy's own shared things, while it shows the true copy
		std::array<std::optional<Clock::time_point>, static_cast<std::size_t>(Notice::kCount)> noticed{};
		std::vector<Build>                                               builds;      // pieces placed and not yet judged (BuildPlaced)
		std::vector<BuildTake>                                           looseTakes;  // the engine's takes no piece has claimed yet
		std::vector<Used>                                                looseUsed;   // our player's own things used, likewise
		Counts                                                           counts;
	};
}
