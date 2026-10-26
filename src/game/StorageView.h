#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
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
//   at the open, the shared things its copy held (the player's own stash from before, or a roll of loot) go into the
//   player's inventory, and the server's contents come in their place; at the close they go again. Their takes and puts
//   are reported as any loot is, and the server checks each against what it holds. One it refuses is undone here: a take
//   given back, a put returned. While the menu is closed nothing of the session's goes into that copy, and the player's
//   own moves there stay theirs (their weapons, armor, keys and notes: what isn't shared never leaves their copy).
// - With the rule on, storage nobody keeps yet (the host's game hasn't loaded it since) is each player's own, as a
//   workbench is: nothing there is reported or applied, so no friend's copy fills with things the session has elsewhere.
//
// Kept free of the game so it can be tested; Storage.cpp reads and changes the engine for it, and WorldSync's loot
// asks it who keeps a container (LootSync::World).
namespace StorageView
{
	enum class Keeper : std::uint8_t
	{
		kNone,    // no rule, or nobody keeps it and it isn't storage here (or we host): ordinary loot
		kUs,      // our game keeps the true copy (the host's)
		kFriend,  // another game keeps it: our copy shows it while our menu is open
		kLocal,   // storage here, kept by nobody yet: each player's own
	};

	// A view's change is not yet numbered when its contents haven't come.
	inline constexpr std::uint32_t NOT_YET = 0xFFFFFFFF;

	// One stack to add to (count > 0) or take out of (count < 0) an inventory.
	struct Adjust
	{
		std::uint32_t item = 0;
		std::int32_t  count = 0;

		bool operator==(const Adjust&) const = default;
	};

	// What undoing a refused move does (Side::Refused).
	struct Undo
	{
		bool          matched = false;    // one of our moves (anything else is left alone)
		bool          closeView = false;  // the server closed our view: the menu closes
		bool          refresh = false;    // we view that container: ask for its contents again (others' changes may have gone into our copy meanwhile)
		std::uint32_t container = 0;
		std::uint32_t item = 0;
		std::int32_t  player = 0;  // to our player's inventory: > 0 given back, < 0 taken back
		std::int32_t  copy = 0;    // to our copy of the container (only while we still view it)
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

	class Side
	{
	public:
		static constexpr std::size_t MAX_OWNERS = 4096;    // as the server's book (StorageBook::Book::MAX_CONTAINERS)
		static constexpr std::size_t MAX_IN_FLIGHT = 256;  // our moves at a friend's storage not yet numbered or refused
		static constexpr std::size_t MAX_NUMBERED = 256;   // our numbered takes there a shortfall may still come for

		struct Counts
		{
			std::uint32_t views = 0;          // menus opened on a friend's storage
			std::uint32_t closedByHost = 0;   // views the server closed (its keeper away, or its world reloaded)
			std::uint32_t refusedTakes = 0;   // our takes refused, given back
			std::uint32_t refusedPuts = 0;    // our puts refused, returned
			std::uint32_t shortTakes = 0;     // numbered takes the keeper's container held less of: given back
			std::uint32_t unmatched = 0;      // refusals for no move we remember (made before a load): left alone
			std::uint32_t inFlightLost = 0;   // moves forgotten at MAX_IN_FLIGHT (never answered)
			std::uint32_t listed = 0;         // our storage listings sent (the host's game)
			std::uint32_t shorts = 0;         // friends' takes our container held less of (the host's game)
		};

		// ---- the session ----------------------------------------------------------------------------------

		// Welcomed under the session's rule; a_host: our game is the host's (its world is the session's). The owner
		// table comes after the welcome.
		void OnWelcome(bool a_rule, bool a_host)
		{
			rule = a_rule;
			host = a_host;
			owners.clear();
			listed.clear();
			view.reset();
			// Moves out at a disconnect are answered by nobody now; what our world holds of them is what it holds.
			flights.clear();
			numbered.clear();
		}

		void OnDisconnected()
		{
			rule = false;
			owners.clear();
			listed.clear();
			view.reset();
			flights.clear();
			numbered.clear();
		}

		// Our world is a save's from here (a load): nothing it does now was ours before, and every listing goes again.
		void OnLoad()
		{
			view.reset();
			flights.clear();
			numbered.clear();
			listed.clear();
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

		// Who keeps a container. a_storageHere: our game calls it storage (LootScope::IsStorage); a_ours: a player ID of ours.
		[[nodiscard]] Keeper KeeperOf(std::uint32_t a_container, bool a_storageHere, const std::function<bool(std::uint32_t)>& a_ours) const
		{
			if (!rule) {
				return Keeper::kNone;
			}
			if (const auto it = owners.find(a_container); it != owners.end()) {
				return a_ours(it->second) ? Keeper::kUs : Keeper::kFriend;
			}
			return a_storageHere && !host ? Keeper::kLocal : Keeper::kNone;
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
			view = View{ a_container, NOT_YET };
			++counts.views;
			return true;
		}

		// Our menu closed: the container we viewed, to tell the server and to take the session's things out of our copy.
		std::optional<std::uint32_t> Close()
		{
			if (!view) {
				return std::nullopt;
			}
			const auto container = view->container;
			view.reset();
			return container;
		}

		// Our copy of the container we view is to be shown afresh (after a refusal): true when the server is asked again. Its
		// changes wait for the contents, as at the open.
		bool Refresh(std::uint32_t a_container)
		{
			if (!view || view->container != a_container) {
				return false;
			}
			view->from = NOT_YET;
			return true;
		}

		[[nodiscard]] std::optional<std::uint32_t> Viewing() const { return view ? std::optional{ view->container } : std::nullopt; }

		// While we view a_container: the first numbered change its contents lack (NOT_YET until they come). Changes from
		// there on go into our copy; those before are in the contents.
		[[nodiscard]] std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container) const
		{
			return view && view->container == a_container ? std::optional{ view->from } : std::nullopt;
		}

		// What the server says the container we view holds: what our copy should show of the shared things, with our moves
		// on their way to it. Nothing for a container we don't view (an answer to a view since closed).
		std::optional<std::map<std::uint32_t, std::int64_t>> Contents(const Protocol::StorageListing& a_contents)
		{
			if (!view || view->container != a_contents.container) {
				return std::nullopt;
			}
			view->from = a_contents.asOf;
			std::map<std::uint32_t, std::int64_t> want;
			for (const auto& stack : a_contents.stacks) {
				want[stack.item] += stack.count;
			}
			for (const auto& flight : flights) {
				if (flight.container == a_contents.container) {
					want[flight.item] += flight.count;
				}
			}
			std::erase_if(want, [](const auto& a_entry) { return a_entry.second <= 0; });
			return want;
		}

		// ---- our moves at a friend's storage -------------------------------------------------------------------

		void Reported(const Protocol::ContainerChange& a_change)
		{
			if (flights.size() >= MAX_IN_FLIGHT) {
				flights.pop_front();
				++counts.inFlightLost;
			}
			flights.push_back(a_change);
		}

		// One of our changes came back numbered (live, or in a catch-up): it went through.
		void OwnNumbered(const Protocol::IndexedContainerChange& a_change)
		{
			const auto it = std::ranges::find(flights, a_change.change);
			if (it == flights.end()) {
				return;
			}
			flights.erase(it);
			if (a_change.change.count < 0) {
				if (numbered.size() >= MAX_NUMBERED) {
					numbered.erase(numbered.begin());
				}
				numbered[a_change.index] = a_change.change;
			}
		}

		// The server refused one of our moves, or closed our view (item 0). What to undo; nothing for a move we don't
		// remember making in this world.
		Undo Refused(const Protocol::StorageRefusal& a_refusal)
		{
			Undo undo{ .container = a_refusal.container, .item = a_refusal.item };
			if (a_refusal.item == 0) {
				undo.matched = undo.closeView = view && view->container == a_refusal.container;
				if (undo.closeView) {
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
				undo.refresh = view && view->container == a_refusal.container;
				++counts.shortTakes;
				return undo;
			}
			const Protocol::ContainerChange move{ a_refusal.container, a_refusal.item, a_refusal.count };
			const auto                      it = std::ranges::find(flights, move);
			if (it == flights.end() || a_refusal.item == Protocol::CONTAINER_EMPTIED) {
				++counts.unmatched;
				return undo;
			}
			flights.erase(it);
			undo.matched = true;
			undo.player = a_refusal.count;  // a take (< 0) comes back out of our player; a put (> 0) goes back to them
			undo.copy = view && view->container == a_refusal.container ? -a_refusal.count : 0;
			undo.refresh = view && view->container == a_refusal.container;
			++(a_refusal.count < 0 ? counts.refusedTakes : counts.refusedPuts);
			return undo;
		}

		[[nodiscard]] std::size_t InFlight() const { return flights.size(); }

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
		};

		bool                                             rule = false;
		bool                                             host = false;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;  // container -> the player whose game keeps it
		std::optional<View>                              view;
		std::deque<Protocol::ContainerChange>            flights;   // ours, oldest first, not yet numbered or refused
		std::map<std::uint32_t, Protocol::ContainerChange> numbered;  // our numbered takes, by index
		std::unordered_set<std::uint32_t>                listed;    // ours listed since our world last loaded
		Counts                                           counts;
	};
}
