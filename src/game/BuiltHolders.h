#pragma once

#include "Protocol.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// The containers players build in workshop mode, as the session's loot names them (a built holder:
// Protocol::ContainerChange::builder). A built piece's reference is one its builder's game made at runtime, and every
// other game has a copy of it of its own (WorkshopSync), so a change at one names the piece by the key the copies are
// kept under: its builder and the builder's own reference ID. The builder's game puts a friend's change into the piece
// itself, as it would at a plugin's container. Every other game keeps a book of what the session put in each piece and
// took out (Book), in session order, and its copy shows that: a copy is made and removed again and again (a move to
// another cell, a load, the end of a session), and a copy just made has to hold what the piece holds. Kept free of the
// game so it can be tested; LootSync runs it, WorldSync and WorkshopSync read the engine for it.
namespace BuiltHolders
{
	// A built piece as the session names it: whose game built it (a player id, or a builder id: WorkshopStore) and that
	// game's own reference ID for it.
	struct Key
	{
		std::uint32_t builder = 0;
		std::uint32_t ref = 0;

		auto operator<=>(const Key&) const = default;
		bool operator==(const Key&) const = default;
	};

	// What a numbered change's built holder is to our game (a_ownId: our player id in the session, 0 while we have none).
	enum class Whose : std::uint8_t
	{
		kPlugin,     // no built holder: a plugin's reference
		kOurs,       // our own piece: its reference ID is ours
		kCopy,       // someone else's: our copy of it, if we have one, by its key
		kElsewhere,  // a piece of another of our characters' worlds (Protocol::BUILT_ELSEWHERE): nothing here
		kUnknown,    // ours, but we have no player id yet to tell
	};

	[[nodiscard]] constexpr Whose WhoseIs(const Protocol::ContainerChange& a_change, std::uint32_t a_ownId)
	{
		if (!Protocol::IsBuiltHolder(a_change)) {
			return Whose::kPlugin;
		}
		if (a_change.builder == Protocol::BUILT_ELSEWHERE) {
			return Whose::kElsewhere;
		}
		if (a_ownId == 0) {
			return Whose::kUnknown;
		}
		return a_change.builder == a_ownId ? Whose::kOurs : Whose::kCopy;
	}

	// A built holder of ours as the wire names it (the sender's side): our own piece (a_key.builder 0) under our player id
	// and its own ID, a copy under its piece's key. None while we have no player id.
	[[nodiscard]] constexpr std::optional<Protocol::ContainerChange> ToWire(const Key& a_key, std::uint32_t a_ownId, std::uint32_t a_item, std::int32_t a_count)
	{
		if (a_ownId == 0 || a_key.ref == 0) {
			return std::nullopt;
		}
		return Protocol::ContainerChange{ a_key.ref, a_item, a_count, a_key.builder == 0 ? a_ownId : a_key.builder };
	}

	// The key a numbered change's built holder names (the receiver's side), for a copy's book.
	[[nodiscard]] constexpr Key KeyOf(const Protocol::ContainerChange& a_change)
	{
		return { a_change.builder, a_change.container };
	}

	// What the session put into each piece someone else built and took out, item by item, in session order, as our copy of
	// it would hold it had it stood all along: a take of more than is there takes what is (as removeitem does). And what our
	// copy of each, while one is here, was last brought to (shown): it is brought on from that, so what our copy holds that
	// the book can't explain (an item put in before the session shared these, a stack only a mod of ours makes) stays. A
	// copy newly found is taken to show what it holds of the book already (as much as both have): one just made holds
	// nothing and is given it all, one a save kept holds it and is given nothing.
	class Book
	{
	public:
		using Items = std::map<std::uint32_t, std::int64_t>;  // item -> count
		using Deltas = std::vector<std::pair<std::uint32_t, std::int64_t>>;

		static constexpr std::size_t MAX_PIECES = 4096;  // built containers with anything in the book
		static constexpr std::size_t MAX_ITEMS = 512;    // items one holds

		// A change the session numbered at the piece (another player's, or ours as we made it: Own).
		void Apply(const Key& a_key, std::uint32_t a_item, std::int64_t a_count)
		{
			auto it = book.find(a_key);
			if (it == book.end()) {
				if (a_count <= 0 || book.size() >= MAX_PIECES) {
					return;
				}
				it = book.emplace(a_key, Items{}).first;
			}
			auto& items = it->second;
			const auto have = items.find(a_item);
			if (have == items.end() && (a_count <= 0 || items.size() >= MAX_ITEMS)) {
				return;
			}
			auto& count = items[a_item];
			count = (std::max)(std::int64_t{ 0 }, count + a_count);
			if (count == 0) {
				items.erase(a_item);
			}
			if (items.empty()) {
				book.erase(it);
			}
		}

		// The piece was emptied (scrapped: the server's CONTAINER_EMPTIED): a new piece under its key holds nothing of it.
		void Forget(const Key& a_key)
		{
			book.erase(a_key);
			if (const auto it = shown.find(a_key); it != shown.end()) {
				it->second.items.clear();
			}
		}

		// Our player's own move at our copy a_local (already made there, so a_have is what it holds of the item now).
		void Own(const Key& a_key, std::uint32_t a_local, std::uint32_t a_item, std::int64_t a_count, std::int64_t a_have)
		{
			Apply(a_key, a_item, a_count);
			auto it = shown.find(a_key);
			if (it == shown.end() || it->second.local != a_local) {
				return;  // found anew at its next Show (as much as both have)
			}
			auto& count = it->second.items[a_item];
			count = (std::max)(std::int64_t{ 0 }, (std::min)(count + a_count, a_have));
			if (count == 0) {
				it->second.items.erase(a_item);
			}
		}

		// What to put into (> 0) or take out of (< 0) our copy a_local of the piece for it to show the book, a_have being what
		// it holds of the items the session shares there; it is taken to show the book once they go in.
		Deltas Show(const Key& a_key, std::uint32_t a_local, const Items& a_have)
		{
			const auto& want = Of(a_key);
			auto        it = shown.find(a_key);
			if (it == shown.end() || it->second.local != a_local) {
				// Found anew: it shows what it holds of the book already.
				for (auto other = shown.begin(); other != shown.end();) {
					other = other->second.local == a_local ? shown.erase(other) : std::next(other);  // an ID our game gave again
				}
				Shown fresh{ a_local, {} };
				for (const auto& [item, count] : a_have) {
					const auto wanted = want.find(item);
					if (const auto both = wanted != want.end() ? (std::min)(count, wanted->second) : 0; both > 0) {
						fresh.items[item] = both;
					}
				}
				it = shown.insert_or_assign(a_key, std::move(fresh)).first;
			}
			Deltas out;
			auto&  was = it->second.items;
			for (const auto& [item, count] : want) {
				const auto had = was.find(item);
				if (const auto delta = count - (had != was.end() ? had->second : 0); delta != 0) {
					out.emplace_back(item, delta);
				}
			}
			for (const auto& [item, count] : was) {
				if (!want.contains(item) && count > 0) {
					out.emplace_back(item, -count);
				}
			}
			was = want;
			return out;
		}

		// Our copy a_local goes (WorkshopSync removes it): what it shows of the book, to take out of it first, so that only
		// what is our player's own goes to their pockets (WorkshopApply::Rescues). The book's share is in the piece itself,
		// in its builder's game: handed over too, it would be had twice.
		Items Going(std::uint32_t a_local)
		{
			for (auto it = shown.begin(); it != shown.end(); ++it) {
				if (it->second.local == a_local) {
					auto items = std::move(it->second.items);
					shown.erase(it);
					return items;
				}
			}
			return {};
		}

		[[nodiscard]] const Items& Of(const Key& a_key) const
		{
			static const Items none;
			const auto         it = book.find(a_key);
			return it != book.end() ? it->second : none;
		}

		[[nodiscard]] std::size_t Pieces() const { return book.size(); }
		[[nodiscard]] std::size_t Copies() const { return shown.size(); }

		void Clear()
		{
			book.clear();
			shown.clear();
		}

		// For the save's record (WorldSync's): the book (not what copies show: found anew after a load). Each piece's key,
		// its count of items, and each item and count.
		[[nodiscard]] std::vector<std::uint32_t> Record() const
		{
			std::vector<std::uint32_t> out;
			out.push_back(static_cast<std::uint32_t>(book.size()));
			for (const auto& [key, items] : book) {
				out.push_back(key.builder);
				out.push_back(key.ref);
				out.push_back(static_cast<std::uint32_t>(items.size()));
				for (const auto& [item, count] : items) {
					out.push_back(item);
					out.push_back(static_cast<std::uint32_t>((std::min<std::int64_t>)(count, Protocol::MAX_ITEM_COUNT)));
				}
			}
			return out;
		}

		// A record Record made; false (and nothing kept) for one that doesn't read.
		bool Load(const std::vector<std::uint32_t>& a_record)
		{
			Clear();
			std::size_t at = 0;
			const auto  next = [&](std::uint32_t& a_value) {
				if (at >= a_record.size()) {
					return false;
				}
				a_value = a_record[at++];
				return true;
			};
			std::uint32_t pieces = 0;
			if (!next(pieces) || pieces > MAX_PIECES) {
				return false;
			}
			for (std::uint32_t p = 0; p < pieces; ++p) {
				Key           key;
				std::uint32_t items = 0;
				if (!next(key.builder) || !next(key.ref) || !next(items) || items > MAX_ITEMS) {
					Clear();
					return false;
				}
				for (std::uint32_t i = 0; i < items; ++i) {
					std::uint32_t item = 0, count = 0;
					if (!next(item) || !next(count) || count > static_cast<std::uint32_t>(Protocol::MAX_ITEM_COUNT)) {
						Clear();
						return false;
					}
					Apply(key, item, count);
				}
			}
			if (at != a_record.size()) {
				Clear();
				return false;
			}
			return true;
		}

	private:
		struct Shown
		{
			std::uint32_t local = 0;  // our copy's reference ID
			Items         items;
		};

		std::map<Key, Items> book;
		std::map<Key, Shown> shown;  // our copies here, what each was last brought to
	};
}
