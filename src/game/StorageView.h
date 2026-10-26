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
//   holds. One it refuses is undone here: a take given back, a put returned. One with no answer for ANSWER_WITHIN (the
//   server dropped it, the connection went) is undone the same way, and done again should its number come after all.
//   While the menu is closed nothing of the session's goes into that copy, and the player's own moves there stay theirs.
// - A load: our moves at a friend's storage that the server numbered and our loaded world lacks (made after the save) come
//   back in the load's catch-up, and are done again on our player (Side::Own): a put takes the item from them again, a
//   take gives it to them again. So the true copy, the server's book and our player agree, and a quickload neither dupes
//   what was put nor loses what was taken. The moves a save was made with still unanswered are kept with it (the co-save),
//   so their answer is known for what it is after a load.
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
	// A move with no answer after this is undone here (the server dropped it, or never had it). Should its number still
	// come, it is done again.
	inline constexpr Clock::duration ANSWER_WITHIN = std::chrono::seconds{ 30 };
	// The same notice at most this often (a Take All refused is one notice, not a thousand).
	inline constexpr Clock::duration NOTICE_EVERY = std::chrono::seconds{ 2 };

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

	// The bookkeeping: who keeps what, our view, and our moves at friends' storage until they are answered.
	class Side
	{
	public:
		static constexpr std::size_t MAX_OWNERS = 4096;    // as the server's book (StorageBook::Book::MAX_CONTAINERS)
		static constexpr std::size_t MAX_NUMBERED = MAX_IN_FLIGHT;  // our numbered takes there a shortfall may still come for
		static constexpr std::size_t MAX_ORPHANS = 4096;   // our moves undone for want of an answer that may still come

		struct Counts
		{
			std::uint32_t views = 0;          // menus opened on a friend's storage
			std::uint32_t closedByHost = 0;   // views the server closed (its keeper away, or its world reloaded)
			std::uint32_t refusedTakes = 0;   // our takes refused, given back
			std::uint32_t refusedPuts = 0;    // our puts refused, returned
			std::uint32_t shortTakes = 0;     // numbered takes the keeper's container held less of: given back
			std::uint32_t unmatched = 0;      // refusals for no move we remember (made before a load): left alone
			std::uint32_t lateRefusals = 0;   // refusals for a move already undone for want of an answer
			std::uint32_t unanswered = 0;     // our moves undone with no answer (ANSWER_WITHIN, or the connection went)
			std::uint32_t unsent = 0;         // our moves undone because they couldn't be sent (an item the session lacks)
			std::uint32_t overflow = 0;       // our moves undone unsent at MAX_IN_FLIGHT
			std::uint32_t redone = 0;         // our numbered moves our world lacked (a load, or undone unanswered): done again
			std::uint32_t listed = 0;         // our storage listings sent (the host's game)
			std::uint32_t shorts = 0;         // friends' takes our container held less of (the host's game)
		};

		// The view that ended (Close).
		struct Ended
		{
			std::uint32_t container = 0;
			bool          closed = false;  // the server had closed it already: our copy holds nothing of the session's
		};

		// ---- the session ----------------------------------------------------------------------------------

		// Welcomed into a_session under its rule; a_host: our game is the host's (its world is the session's). The owner
		// table comes after the welcome. Our moves still unanswered (from a save loaded before it, or undone as the
		// connection went) stay when it is the same session: its catch-up answers them.
		void OnWelcome(bool a_rule, bool a_host, std::uint64_t a_session, Clock::time_point a_now)
		{
			rule = a_rule;
			host = a_host;
			owners.clear();
			listed.clear();
			view.reset();
			if (a_session != movesSession) {
				flights.clear();
				orphans.clear();
				numbered.clear();
				movesSession = a_session;
			}
			for (auto& flight : flights) {
				flight.due = a_now + ANSWER_WITHIN;
			}
		}

		// The connection went: our moves still unanswered are undone (returned). Should a reconnect's catch-up number one
		// after all, it is done again (Own).
		std::vector<Undo> OnDisconnected()
		{
			rule = false;
			owners.clear();
			listed.clear();
			view.reset();
			std::vector<Undo> undone;
			while (!flights.empty()) {
				undone.push_back(Abandon(flights.front().change));
				flights.pop_front();
			}
			return undone;
		}

		// Our world is a save's from here (a load) in a_session: a_unanswered, the moves the save was made with still
		// unanswered (its co-save record; none when it is from another session), are in it. Nothing else we did before is.
		void OnLoad(std::uint64_t a_session, std::span<const Protocol::ContainerChange> a_unanswered, Clock::time_point a_now)
		{
			view.reset();
			listed.clear();
			flights.clear();
			orphans.clear();
			numbered.clear();
			movesSession = a_session;
			for (const auto& change : a_unanswered) {
				if (flights.size() < MAX_IN_FLIGHT) {
					flights.push_back({ change, a_now + ANSWER_WITHIN });
				}
			}
		}

		void Owners(const Protocol::StorageOwners& a_owners)
		{
			for (const auto& owner : a_owners.entries) {
				if (owners.size() < MAX_OWNERS || owners.contains(owner.container)) {
					owners[owner.container] = owner.ownerId;
				}
			}
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
				return a_ours(it->second) ? Keeper::kUs : Keeper::kFriend;
			}
			return Keeper::kNone;
		}

		[[nodiscard]] std::optional<std::uint32_t> OwnerOf(std::uint32_t a_container) const
		{
			const auto it = owners.find(a_container);
			return it != owners.end() ? std::optional{ it->second } : std::nullopt;
		}

		[[nodiscard]] std::size_t OwnerCount() const { return owners.size(); }

		// ---- a friend's storage, viewed ----------------------------------------------------------------------

		// Our menu opened on our copy of a friend's container: true when the server is to be told (not when we view it
		// already). A view of another container ends first (Close).
		bool Open(std::uint32_t a_container)
		{
			if (view && view->container == a_container) {
				return false;
			}
			view = View{ a_container, NOT_YET, false };
			++counts.views;
			return true;
		}

		// Our menu closed: the view that ended.
		std::optional<Ended> Close()
		{
			if (!view) {
				return std::nullopt;
			}
			const Ended ended{ view->container, view->closed };
			view.reset();
			return ended;
		}

		// Our copy of the container we view is to be shown afresh (after a refusal): true when the server is asked again
		// (not while its contents are still to come: they answer this too). Its changes wait for the contents, as at the open.
		bool Refresh(std::uint32_t a_container)
		{
			if (!Shows(a_container) || view->from == NOT_YET) {
				return false;
			}
			view->from = NOT_YET;
			return true;
		}

		// The container our menu is open on as a friend's storage (shown, or closed by the server).
		[[nodiscard]] std::optional<std::uint32_t> Viewing() const { return view ? std::optional{ view->container } : std::nullopt; }

		// Our copy of a_container shows the true copy (its contents asked for, or come), not closed by the server.
		[[nodiscard]] bool Shows(std::uint32_t a_container) const { return view && view->container == a_container && !view->closed; }

		// While our copy of a_container shows the true copy: the first numbered change its contents lack (NOT_YET until they
		// come). Changes from there on go into our copy; those before are in the contents.
		[[nodiscard]] std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container) const
		{
			return Shows(a_container) ? std::optional{ view->from } : std::nullopt;
		}

		// What the server says the container we view holds: what our copy should show of the shared things, with our moves
		// on their way to it. Nothing for a container we don't show (an answer to a view since closed).
		std::optional<std::map<std::uint32_t, std::int64_t>> Contents(const Protocol::StorageListing& a_contents)
		{
			if (!Shows(a_contents.container)) {
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
			std::erase_if(want, [](const auto& a_entry) { return a_entry.second <= 0; });
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
			flights.push_back({ a_change, a_now + ANSWER_WITHIN });
			return true;
		}

		// One of our moves at a friend's storage came back numbered, in session order (live, or in a catch-up). a_lacking:
		// the ledger says our world doesn't have it. What to do again on our player: nothing for one on its way from this
		// world (a flight); the move again for one we undid for want of an answer, or one our world lacks (a load from
		// before it).
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
				return std::nullopt;
			}
			if (const auto it = std::ranges::find(orphans, change); it != orphans.end()) {
				orphans.erase(it);
				return Redo(change);
			}
			return a_lacking ? std::optional{ Redo(change) } : std::nullopt;
		}

		// The server refused one of our moves, or closed our view (item 0). What to undo; nothing for a move we don't
		// remember making in this world.
		Undo Refused(const Protocol::StorageRefusal& a_refusal)
		{
			Undo undo{ .container = a_refusal.container, .item = a_refusal.item };
			if (a_refusal.item == 0) {
				undo.matched = undo.closeView = Shows(a_refusal.container);
				if (undo.closeView) {
					view->closed = true;
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
			if (const auto it = std::ranges::find(orphans, move); it != orphans.end()) {
				orphans.erase(it);  // undone already
				++counts.lateRefusals;
				return undo;
			}
			++counts.unmatched;
			return undo;
		}

		// Our moves with no answer past ANSWER_WITHIN: undone, each. An answer may still come (Own does it again, Refused
		// leaves it).
		std::vector<Undo> Expired(Clock::time_point a_now)
		{
			std::vector<Undo> undone;
			while (!flights.empty() && flights.front().due <= a_now) {
				undone.push_back(Abandon(flights.front().change));
				flights.pop_front();
			}
			return undone;
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
		[[nodiscard]] std::size_t Orphans() const { return orphans.size(); }

		// For the co-save: our moves still unanswered, and the session they were made in.
		[[nodiscard]] std::vector<Protocol::ContainerChange> Unanswered() const
		{
			std::vector<Protocol::ContainerChange> out;
			out.reserve(flights.size());
			for (const auto& flight : flights) {
				out.push_back(flight.change);
			}
			return out;
		}

		[[nodiscard]] std::uint64_t MovesSession() const { return movesSession; }

		// ---- our own storage (the host's game) -----------------------------------------------------------------

		// Whether a container is to be listed now: not yet since the session began or our world last loaded (or a
		// friend's take found it short).
		[[nodiscard]] bool Listed(std::uint32_t a_container) const { return listed.contains(a_container); }

		void MarkListed(std::uint32_t a_container)
		{
			if (listed.size() < MAX_OWNERS) {
				listed.insert(a_container);
				++counts.listed;
			}
		}

		// Listed again soon (a shortfall, our own menu closed on it).
		void Unlist(std::uint32_t a_container) { listed.erase(a_container); }

		// Every container is listed again (our world asked the server for the session's changes again: the server holds
		// our listings stale until then).
		void UnlistAll() { listed.clear(); }

		void NoteShort() { ++counts.shorts; }

		[[nodiscard]] const Counts& Stats() const { return counts; }

	private:
		struct View
		{
			std::uint32_t container = 0;
			std::uint32_t from = NOT_YET;
			bool          closed = false;  // the server closed it: the menu is still open on our own copy
		};

		struct Flight
		{
			Protocol::ContainerChange change;
			Clock::time_point         due{};  // undone if no answer has come by then
		};

		// Our move undone: a take comes back out of our player (and into our copy, while it shows the true copy); a put goes
		// back to them.
		Undo Back(const Protocol::ContainerChange& a_change) const
		{
			const bool shown = Shows(a_change.container);
			return { .matched = true, .refresh = shown, .container = a_change.container, .item = a_change.item, .player = a_change.count, .copy = shown ? -static_cast<std::int64_t>(a_change.count) : 0 };
		}

		// Our numbered move done again on our player: a put taken from them, a take given to them (our copy is shown afresh).
		Undo Redo(const Protocol::ContainerChange& a_change)
		{
			++counts.redone;
			return { .matched = true, .refresh = Shows(a_change.container), .container = a_change.container, .item = a_change.item, .player = -static_cast<std::int64_t>(a_change.count) };
		}

		// A flight given up: undone, and remembered in case its answer comes after all.
		Undo Abandon(const Protocol::ContainerChange& a_change)
		{
			if (orphans.size() >= MAX_ORPHANS) {
				orphans.pop_front();
			}
			orphans.push_back(a_change);
			++counts.unanswered;
			return Back(a_change);
		}

		bool                                             rule = false;
		bool                                             host = false;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;  // container -> the player whose game keeps it
		std::optional<View>                              view;
		std::uint64_t                                    movesSession = 0;  // the session our flights and orphans are in
		std::deque<Flight>                               flights;   // ours, oldest first, not yet numbered or refused
		std::deque<Protocol::ContainerChange>            orphans;   // ours, undone unanswered, oldest first
		std::map<std::uint32_t, Protocol::ContainerChange> numbered;  // our numbered takes, by index
		std::unordered_set<std::uint32_t>                listed;    // ours listed since our world last loaded
		Counts                                           counts;
	};

	// What a friend's game tells its player.
	enum class Notice : std::uint8_t
	{
		kClosed,       // the true copy can't be shown now (its keeper away, or their world reloaded): the menu closes
		kTakeRefused,  // a take undone: given back
		kPutRefused,   // a put undone: returned
		kShort,        // a take the keeper's container held less of: what it lacked is given back
		kCount,
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
		// Adds to (> 0) or takes out of (< 0) our copy of a container; what was changed (a take takes no more than there is).
		virtual std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count) = 0;
		// The same for our player's inventory.
		virtual std::int64_t ChangePlayer(const PlayerChange& a_change) = 0;
		// The player's moves since the last frame are reported now (WorldSync::FlushLoot): they come back through
		// Client::Reporting.
		virtual void FlushLoot() = 0;
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
			std::uint32_t takenBack = 0;  // undone takes and redone puts: out of our player
			std::int64_t  usedUp = 0;     // of those, items our player no longer had
			std::uint32_t givenBack = 0;  // undone puts and redone takes: to our player
			std::uint32_t notices = 0;    // notices shown (NOTICE_EVERY at most, each)
		};

		explicit Client(Engine& a_engine) :
			engine(a_engine)
		{}

		Side&                             State() { return side; }
		[[nodiscard]] const Side&         State() const { return side; }
		[[nodiscard]] const Counts&       Stats() const { return counts; }
		// What our copy's own shared things are, set aside while it shows the true copy.
		[[nodiscard]] const std::map<std::uint32_t, std::int64_t>& Aside() const { return aside; }

		// ---- the session ----------------------------------------------------------------------------------

		void OnWelcome(bool a_rule, bool a_host, std::uint64_t a_session)
		{
			EndView(false);
			side.OnWelcome(a_rule, a_host, a_session, engine.Now());
		}

		// The connection went: the moves made in the menu go with the rest, every unanswered one is undone, and our copy is
		// our own again.
		void OnDisconnected()
		{
			if (side.Viewing()) {
				engine.FlushLoot();
			}
			EndView(false);
			for (const auto& undo : side.OnDisconnected()) {
				Apply(undo, std::nullopt);
			}
		}

		// Before a save or a load (after WorldSync::FlushLoot): no save holds the session's things in our copy, nor misses
		// what was set aside.
		void BeforeSaveOrLoad() { EndView(true); }

		// A save loaded, in a_session (see Side::OnLoad).
		void OnLoad(std::uint64_t a_session, std::span<const Protocol::ContainerChange> a_unanswered)
		{
			aside.clear();  // the save's copy holds it
			side.OnLoad(a_session, a_unanswered, engine.Now());
		}

		// Every frame: moves unanswered too long are undone.
		void Frame()
		{
			for (const auto& undo : side.Expired(engine.Now())) {
				Apply(undo, undo.player < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
			}
		}

		// ---- the container menu ----------------------------------------------------------------------------

		// Our menu shows a_container; a_friends: another game keeps it.
		void Open(std::uint32_t a_container, bool a_friends)
		{
			if (!side.Rule() || !a_friends) {
				return;
			}
			// Our moves before it (at another container, or this one before it was a friend's) go first, as theirs.
			engine.FlushLoot();
			if (side.Viewing() && *side.Viewing() != a_container) {
				EndView(true);
			}
			if (!side.Open(a_container)) {
				return;
			}
			// What our copy held of the session's things is set aside, out of reach: our player's stash, or the host's
			// things it mirrored in an older session (theirs to take only from the true copy).
			aside.clear();
			for (const auto& [item, count] : engine.SharedIn(a_container)) {
				const auto moved = -engine.ChangeCopy(a_container, item, -count);
				if (moved > 0) {
					aside[item] += moved;
					++counts.setAside;
				}
			}
			engine.Send(Protocol::Encode(Protocol::StorageView{ a_container, true }));
		}

		// Our menu closed. Nothing to do unless it showed a friend's storage.
		void MenuClosed()
		{
			if (!side.Viewing()) {
				return;
			}
			engine.FlushLoot();  // the moves made in the menu go first: they were made while it showed the session's things
			EndView(true);
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
			for (const auto& adjust : Reconcile(*want, engine.SharedIn(a_contents.container))) {
				engine.ChangeCopy(a_contents.container, adjust.item, adjust.count);
				++counts.adjusts;
			}
		}

		void OnRefused(const Protocol::StorageRefusal& a_refusal)
		{
			if (a_refusal.item == 0) {
				if (!side.Shows(a_refusal.container)) {
					return;
				}
				// The server closed our view. The moves made in the menu go first (they are refused, and undone); then the
				// session's things leave our copy, and the menu closes. What was set aside comes back as it does.
				engine.FlushLoot();
				if (side.Refused(a_refusal).closeView) {
					for (const auto& [item, count] : engine.SharedIn(a_refusal.container)) {
						engine.ChangeCopy(a_refusal.container, item, -count);
					}
					engine.HideMenu();
					Notify(Notice::kClosed);
				}
				return;
			}
			const auto undo = side.Refused(a_refusal);
			Apply(undo, a_refusal.reason == Protocol::StorageReason::kShort ? Notice::kShort :
			            undo.player < 0                                     ? Notice::kTakeRefused :
			                                                                  Notice::kPutRefused);
		}

		// ---- WorldSync's loot ----------------------------------------------------------------------------------

		// Our move at a friend's storage, about to be reported: false when it isn't (too many unanswered), and it is undone.
		bool Reporting(const Protocol::ContainerChange& a_change)
		{
			if (side.Reported(a_change, engine.Now())) {
				return true;
			}
			const bool shown = side.Shows(a_change.container);
			Apply({ .matched = true, .container = a_change.container, .item = a_change.item, .player = a_change.count, .copy = shown ? -static_cast<std::int64_t>(a_change.count) : 0 },
				a_change.count < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
			return false;
		}

		// One of our moves at a friend's storage, numbered (see Side::Own).
		void OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking)
		{
			if (const auto redo = side.Own(a_change, a_lacking)) {
				Apply(*redo, std::nullopt);
			}
		}

		// Our move couldn't be sent (Session: the session's numbering has no ID for its item).
		void Unsent(const Protocol::ContainerChange& a_change)
		{
			if (const auto undo = side.Unsent(a_change)) {
				Apply(*undo, undo->player < 0 ? Notice::kTakeRefused : Notice::kPutRefused);
			}
		}

	private:
		void Apply(const Undo& a_undo, std::optional<Notice> a_notice)
		{
			if (!a_undo.matched) {
				return;
			}
			if (a_undo.player != 0) {
				const auto done = engine.ChangePlayer(PlayerChange{ a_undo.item, a_undo.player });
				if (a_undo.player < 0) {
					++counts.takenBack;
					counts.usedUp += done - a_undo.player;  // what our player no longer had (used, dropped)
				} else {
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
		}

		// The view ends: the session's things leave our copy (unless the server's close took them already), and what was
		// set aside comes back.
		void EndView(bool a_tellServer)
		{
			const auto ended = side.Close();
			if (!ended) {
				return;
			}
			if (!ended->closed) {
				for (const auto& [item, count] : engine.SharedIn(ended->container)) {
					engine.ChangeCopy(ended->container, item, -count);
				}
			}
			for (const auto& [item, count] : aside) {
				engine.ChangeCopy(ended->container, item, count);
			}
			aside.clear();
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
		std::map<std::uint32_t, std::int64_t>                            aside;  // our copy's own shared things, while it shows the true copy
		std::array<std::optional<Clock::time_point>, static_cast<std::size_t>(Notice::kCount)> noticed{};
		Counts                                                           counts;
	};
}
