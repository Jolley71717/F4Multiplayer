#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The server's book of shared storage (Protocol::SessionRules::sharedStorage): a home's or a settlement's container has
// one true copy, the host's game's, and this book is what the server knows of it, so it can tell whether a friend's take
// or put at their copy may go through.
//
// - Owners. The host's game claims each container its world calls storage by listing it (List): what it holds of what the
//   session shares, as of the change its ledger takes next (asOf). Only the host's game claims: its world is the
//   session's. (A dedicated server has no host's game, so nothing is ever claimed there and storage stays as in 0.9.2.)
//   Owners are kept with the session (SessionFile), so a container claimed once is the host's for good.
// - What it holds. The listing, plus every change numbered for it since that its keeper's world hadn't had yet: the
//   friends' moves from asOf on (the keeper's own, sent before the listing, are in it). Each numbered change moves it
//   on (Numbered).
// - Fresh or not. A listing describes the world its keeper had when it was made. The keeper loading a save, saying hello
//   again or leaving makes every listing of theirs stale (Stale): until their world lists a container again, it is closed
//   to friends, and every move a friend makes there is refused. So a move the server can't check is refused, never let
//   through to dupe or wipe an item.
// - Viewers. A friend whose container menu is open on their copy (Open, Close): only their moves are checked and let
//   through; moves at a copy nobody views (an old copy's contents, which are its player's own) are refused.
//
// A take that went through and then found the keeper's container short (its world reloaded before the server heard)
// comes back from the keeper's game as a shortfall (Short): the taker's game is told to give back what was missing.
//
// Kept free of the server's loop so it can be tested (tests/StorageBookTests.cpp).
namespace StorageBook
{
	enum class Verdict : std::uint8_t
	{
		kNotStorage,  // nobody keeps it: an ordinary container, as before
		kKeeper,      // the keeper's own move: its world is the truth, always numbered
		kAccept,      // a friend's move that the book holds enough for
		kClosed,      // the keeper is away, or hasn't listed it since their world last loaded
		kNotViewing,  // the friend's menu isn't open on it
		kNotHeld,     // a take of more than it holds, or an emptied marker
		kCantHold,    // a put of an item the keeper's game lacks, or of a new item into a full listing
	};

	enum class Listed : std::uint8_t
	{
		kTaken,       // claimed (or listed again) by the host's game
		kNotHost,     // only the host's game claims
		kFull,        // the book has MAX_CONTAINERS and this one is new
	};

	// A friend's view, answered.
	struct Viewed
	{
		enum class Kind : std::uint8_t
		{
			kShown,       // contents: what the book holds, as of the next number the server gives
			kClosed,      // closed for now (Verdict::kClosed)
			kNotStorage,  // nobody keeps it
			kOwn,         // the keeper's own container: nothing to view
		};
		Kind                     kind = Kind::kNotStorage;
		Protocol::StorageListing contents;
	};

	// A viewer to tell their view closed (the keeper went stale or away).
	struct Closed
	{
		std::uint32_t container = 0;
		std::uint32_t viewer = 0;
	};

	class Book
	{
	public:
		static constexpr std::size_t MAX_CONTAINERS = 4096;
		static constexpr std::size_t MAX_VIEWERS = 8;            // per container (a session has a few players)
		static constexpr std::size_t MAX_SHORTS_REMEMBERED = 4096;  // numbered takes already answered for a shortfall

		explicit Book(std::size_t a_maxContainers = MAX_CONTAINERS) :
			maxContainers(a_maxContainers)
		{}

		// ---- the keeper ---------------------------------------------------------------------------------------

		// The host's game says what a_listing.container holds (and claims it). a_log: the session's numbered changes, so the
		// friends' moves its world hadn't had yet (from a_listing.asOf on) count. a_host: the player is the host's own game.
		// a_changed: set when the owner table changed (a new entry, or a new keeper), so the games are told.
		Listed List(std::uint32_t a_player, bool a_host, const Protocol::StorageListing& a_listing, std::span<const Protocol::IndexedContainerChange> a_log,
			bool* a_changed = nullptr)
		{
			if (a_changed) {
				*a_changed = false;
			}
			if (!a_host) {
				return Listed::kNotHost;
			}
			auto it = entries.find(a_listing.container);
			if (it == entries.end()) {
				if (entries.size() >= maxContainers) {
					return Listed::kFull;
				}
				it = entries.emplace(a_listing.container, Entry{}).first;
				if (a_changed) {
					*a_changed = true;
				}
			} else if (it->second.keeper != a_player && a_changed) {
				*a_changed = true;  // another host's game (the session file went to another PC): the host's world is the session's
			}
			auto& entry = it->second;
			entry.keeper = a_player;
			entry.fresh = true;
			entry.held.clear();
			for (const auto& stack : a_listing.stacks) {
				if (stack.item != 0 && stack.count > 0 && entry.held.size() < Protocol::MAX_STORAGE_STACKS) {
					entry.held[stack.item] += stack.count;
				}
			}
			for (std::size_t i = (std::min<std::size_t>)(a_listing.asOf, a_log.size()); i < a_log.size(); ++i) {
				const auto& change = a_log[i];
				if (change.change.container == a_listing.container && change.playerId != a_player && change.change.item != Protocol::CONTAINER_EMPTIED) {
					Move(entry, change.change);
				}
			}
			return Listed::kTaken;
		}

		// The keeper's world is not the one its listings described any more (it loaded a save, said hello again, or left).
		// Returns the viewers to tell their view closed; they are viewers no more.
		std::vector<Closed> Stale(std::uint32_t a_keeper)
		{
			std::vector<Closed> closed;
			for (auto& [container, entry] : entries) {
				if (entry.keeper != a_keeper) {
					continue;
				}
				entry.fresh = false;
				entry.held.clear();
				for (const auto viewer : entry.viewers) {
					closed.push_back({ container, viewer });
				}
				entry.viewers.clear();
			}
			return closed;
		}

		// A player left: they view nothing, and what they keep is stale (see Stale).
		std::vector<Closed> Left(std::uint32_t a_player)
		{
			for (auto& [container, entry] : entries) {
				std::erase(entry.viewers, a_player);
			}
			return Stale(a_player);
		}

		// ---- moves ----------------------------------------------------------------------------------------------

		// Whether a player's move at a container may be numbered. a_canHold: whether the keeper's game has the item (a
		// friend's put of a mod's item it lacks would vanish there).
		Verdict Check(std::uint32_t a_player, const Protocol::ContainerChange& a_change, const std::function<bool(std::uint32_t a_keeper, std::uint32_t a_item)>& a_canHold = {}) const
		{
			const auto it = entries.find(a_change.container);
			if (it == entries.end()) {
				return Verdict::kNotStorage;
			}
			const auto& entry = it->second;
			if (entry.keeper == a_player) {
				return Verdict::kKeeper;
			}
			if (!entry.fresh) {
				return Verdict::kClosed;
			}
			if (!std::ranges::contains(entry.viewers, a_player)) {
				return Verdict::kNotViewing;
			}
			if (a_change.item == Protocol::CONTAINER_EMPTIED) {
				return Verdict::kNotHeld;
			}
			if (a_change.count < 0) {
				const auto held = entry.held.find(a_change.item);
				return held != entry.held.end() && held->second >= -static_cast<std::int64_t>(a_change.count) ? Verdict::kAccept : Verdict::kNotHeld;
			}
			if (a_canHold && !a_canHold(entry.keeper, a_change.item)) {
				return Verdict::kCantHold;
			}
			if (!entry.held.contains(a_change.item) && entry.held.size() >= Protocol::MAX_STORAGE_STACKS) {
				return Verdict::kCantHold;
			}
			return Verdict::kAccept;
		}

		// A change the server numbered: what the container holds moves on. False when it took more than the book held (the
		// keeper's own take of what a friend's take, numbered first, already had: the keeper's game finds the shortfall).
		bool Numbered(const Protocol::IndexedContainerChange& a_change)
		{
			const auto it = entries.find(a_change.change.container);
			if (it == entries.end() || !it->second.fresh || a_change.change.item == Protocol::CONTAINER_EMPTIED) {
				return true;
			}
			return Move(it->second, a_change.change);
		}

		// The keeper's game applied numbered take a_index (a friend's) and its container held a_missing fewer than taken.
		// The refusal for the friend who took it, once per change; nothing for a change that isn't a friend's take at a
		// container a_keeper keeps, or more than was taken.
		std::optional<std::pair<std::uint32_t, Protocol::StorageRefusal>> Short(std::uint32_t a_keeper, const Protocol::StorageShort& a_short,
			std::span<const Protocol::IndexedContainerChange> a_log)
		{
			if (a_short.index >= a_log.size() || shorts.contains(a_short.index)) {
				return std::nullopt;
			}
			const auto& numbered = a_log[a_short.index];
			const auto  it = entries.find(numbered.change.container);
			if (it == entries.end() || it->second.keeper != a_keeper || numbered.playerId == a_keeper || numbered.change.count >= 0 ||
				numbered.change.item == Protocol::CONTAINER_EMPTIED || a_short.missing <= 0 || a_short.missing > -static_cast<std::int64_t>(numbered.change.count)) {
				return std::nullopt;
			}
			if (shorts.size() >= MAX_SHORTS_REMEMBERED) {
				shorts.erase(shorts.begin());
			}
			shorts.insert(a_short.index);
			return std::pair{ numbered.playerId, Protocol::StorageRefusal{ numbered.change.container, numbered.change.item, -a_short.missing, a_short.index, Protocol::StorageReason::kShort } };
		}

		// ---- viewers --------------------------------------------------------------------------------------------

		// A friend opened their copy: what it holds, as of a_next (the number the server gives next), when it is open.
		Viewed Open(std::uint32_t a_player, std::uint32_t a_container, std::uint32_t a_next)
		{
			const auto it = entries.find(a_container);
			if (it == entries.end()) {
				return {};
			}
			auto& entry = it->second;
			if (entry.keeper == a_player) {
				return { Viewed::Kind::kOwn };
			}
			if (!entry.fresh) {
				return { Viewed::Kind::kClosed };
			}
			if (!std::ranges::contains(entry.viewers, a_player)) {
				if (entry.viewers.size() >= MAX_VIEWERS) {
					entry.viewers.erase(entry.viewers.begin());
				}
				entry.viewers.push_back(a_player);
			}
			return { Viewed::Kind::kShown, Contents(a_container, entry, a_next) };
		}

		void Close(std::uint32_t a_player, std::uint32_t a_container)
		{
			if (const auto it = entries.find(a_container); it != entries.end()) {
				std::erase(it->second.viewers, a_player);
			}
		}

		// What a container holds, as of a_next, for its viewers (after the keeper listed it again).
		[[nodiscard]] Protocol::StorageListing ContentsOf(std::uint32_t a_container, std::uint32_t a_next) const
		{
			const auto it = entries.find(a_container);
			return it != entries.end() ? Contents(a_container, it->second, a_next) : Protocol::StorageListing{ a_container, a_next, {} };
		}

		[[nodiscard]] std::vector<std::uint32_t> Viewers(std::uint32_t a_container) const
		{
			const auto it = entries.find(a_container);
			return it != entries.end() ? it->second.viewers : std::vector<std::uint32_t>{};
		}

		// ---- what is known ----------------------------------------------------------------------------------------

		[[nodiscard]] std::optional<std::uint32_t> KeeperOf(std::uint32_t a_container) const
		{
			const auto it = entries.find(a_container);
			return it != entries.end() ? std::optional{ it->second.keeper } : std::nullopt;
		}

		[[nodiscard]] bool Fresh(std::uint32_t a_container) const
		{
			const auto it = entries.find(a_container);
			return it != entries.end() && it->second.fresh;
		}

		// How many of a_item the book holds for a container (0 when stale or unknown).
		[[nodiscard]] std::int64_t Held(std::uint32_t a_container, std::uint32_t a_item) const
		{
			const auto it = entries.find(a_container);
			if (it == entries.end()) {
				return 0;
			}
			const auto held = it->second.held.find(a_item);
			return held != it->second.held.end() ? held->second : 0;
		}

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

		// The owner table, for the games and the session file (in container order).
		[[nodiscard]] std::vector<Protocol::StorageOwner> Owners() const
		{
			std::vector<Protocol::StorageOwner> out;
			out.reserve(entries.size());
			for (const auto& [container, entry] : entries) {
				out.push_back({ container, entry.keeper });
			}
			return out;
		}

		// An owner from the session file: stale until its keeper's world lists it again.
		void Restore(const Protocol::StorageOwner& a_owner)
		{
			if (!Protocol::IsShareableRef(a_owner.container) || a_owner.ownerId == 0 || (entries.size() >= maxContainers && !entries.contains(a_owner.container))) {
				return;
			}
			entries[a_owner.container] = Entry{ .keeper = a_owner.ownerId };
		}

		void Clear()
		{
			entries.clear();
			shorts.clear();
		}

	private:
		struct Entry
		{
			std::uint32_t                           keeper = 0;
			bool                                    fresh = false;
			std::map<std::uint32_t, std::int64_t>   held;     // item -> count, while fresh
			std::vector<std::uint32_t>              viewers;  // friends whose menu is open on it
		};

		// False when a take found less than it took (what is there is taken).
		static bool Move(Entry& a_entry, const Protocol::ContainerChange& a_change)
		{
			if (a_change.count > 0) {
				if (a_entry.held.contains(a_change.item) || a_entry.held.size() < Protocol::MAX_STORAGE_STACKS) {
					a_entry.held[a_change.item] += a_change.count;
				}
				return true;
			}
			const auto it = a_entry.held.find(a_change.item);
			const auto taken = -static_cast<std::int64_t>(a_change.count);
			if (it == a_entry.held.end()) {
				return taken == 0;
			}
			const bool enough = it->second >= taken;
			it->second -= (std::min)(it->second, taken);
			if (it->second == 0) {
				a_entry.held.erase(it);
			}
			return enough;
		}

		static Protocol::StorageListing Contents(std::uint32_t a_container, const Entry& a_entry, std::uint32_t a_next)
		{
			Protocol::StorageListing out{ a_container, a_next, {} };
			for (const auto& [item, count] : a_entry.held) {
				if (count > 0 && out.stacks.size() < Protocol::MAX_STORAGE_STACKS) {
					out.stacks.push_back({ item, static_cast<std::int32_t>((std::min<std::int64_t>)(count, Protocol::MAX_ITEM_COUNT)) });
				}
			}
			return out;
		}

		std::size_t                             maxContainers;
		std::map<std::uint32_t, Entry>          entries;
		std::unordered_set<std::uint32_t>       shorts;  // numbered changes a shortfall was answered for
	};
}
