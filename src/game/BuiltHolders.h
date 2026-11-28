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
	// it would hold it had it stood all along: a take of more than is there takes what is (as removeitem does). And what
	// each copy of ours was last brought to (shown), by the copy's own reference ID: it is brought on from that, so what our
	// copy holds that the book can't explain (an item put in before the session shared these, a stack only a mod of ours
	// makes) stays. A copy newly found holds nothing of the book, and is given it all: one just made holds nothing, and one
	// a save from before built containers were shared kept (a version 1 record: no book) holds only our player's own
	// things. Only while the book is unsure is one taken to show what it holds of the book already (as much as both have).
	//
	// What a copy shows is the share of the piece's loot that is in the piece itself, in its builder's world: when the copy
	// goes, that comes out of it first (Going), and only the rest is our player's. So it is kept per copy, not per piece (a
	// copy waiting to go after its piece was scrapped and another took its ID still shows the old one's), it outlives the
	// piece's emptying and a session's change, and it goes into the save's record with the book: a copy a save kept is
	// known by what it showed then. When that can't be known (a record that didn't read, more copies than are kept) the
	// book is unsure, and a copy it knows nothing of may hold the session's loot: none of it is our player's to have.
	class Book
	{
	public:
		using Items = std::map<std::uint32_t, std::int64_t>;  // item -> count
		using Deltas = std::vector<std::pair<std::uint32_t, std::int64_t>>;

		static constexpr std::size_t MAX_PIECES = 4096;  // built containers with anything in the book
		static constexpr std::size_t MAX_COPIES = 4096;  // copies of ours whose share is kept
		static constexpr std::size_t MAX_ITEMS = 512;    // items one holds

		// What a copy that goes holds of the session's loot (Going).
		struct Share
		{
			Items items;         // the session's: out of the copy before anything in it is our player's
			bool  known = true;  // false: the book can't say (unsure, and nothing kept of this copy): none of it is ours
		};

		// A change the session numbered at the piece (another player's, or ours as we made it: Own). Returns what the book's
		// count of the item moved by (a take of more than it has moves it by what it had).
		std::int64_t Apply(const Key& a_key, std::uint32_t a_item, std::int64_t a_count)
		{
			auto it = book.find(a_key);
			if (it == book.end()) {
				if (a_count <= 0 || book.size() >= MAX_PIECES) {
					return 0;
				}
				it = book.emplace(a_key, Items{}).first;
			}
			auto&      items = it->second;
			const auto have = items.find(a_item);
			if (have == items.end() && (a_count <= 0 || items.size() >= MAX_ITEMS)) {
				return 0;
			}
			auto&      count = items[a_item];
			const auto before = count;
			count = (std::max)(std::int64_t{ 0 }, count + a_count);
			const auto moved = count - before;
			if (count == 0) {
				items.erase(a_item);
			}
			if (items.empty()) {
				book.erase(it);
			}
			return moved;
		}

		// What our player's own move did (Own), for its undoing should the session not number it (LootSync's BuiltRefused).
		struct Moved
		{
			std::int64_t book = 0;   // the piece's count of the item in the book
			std::int64_t shown = 0;  // what our copy shows of it: the session's share of the move
		};

		// The piece was emptied (scrapped: the server's CONTAINER_EMPTIED): a new piece under its key holds nothing of it. What
		// our copies show stays theirs: brought to the book (Show) they give it up, and one that goes first gives it up then.
		void Forget(const Key& a_key) { book.erase(a_key); }

		// Our player's own move at our copy a_local (already made there, so a_have is what it holds of the item now).
		Moved Own(const Key& a_key, std::uint32_t a_local, std::uint32_t a_item, std::int64_t a_count, std::int64_t a_have)
		{
			Moved out{ Apply(a_key, a_item, a_count), 0 };
			auto  it = shown.find(a_local);
			if (it == shown.end() || it->second.key != a_key) {
				// Never shown: it holds nothing of the book but this move. Its next Show gives it the rest.
				if (!Track(a_local, a_key)) {
					return out;
				}
				it = shown.find(a_local);
			}
			auto&      count = it->second.items[a_item];
			const auto before = count;
			count = (std::max)(std::int64_t{ 0 }, (std::min)(count + a_count, a_have));
			out.shown = count - before;
			if (count == 0) {
				it->second.items.erase(a_item);
			}
			return out;
		}

		// What to put into (> 0) or take out of (< 0) our copy a_local of the piece for it to show the book, a_have being what
		// it holds of the items the session shares there; it is taken to show the book once they go in. Nothing while the
		// copy can't be kept (MAX_COPIES): it shows what it holds.
		Deltas Show(const Key& a_key, std::uint32_t a_local, const Items& a_have)
		{
			const auto& want = Of(a_key);
			auto        it = shown.find(a_local);
			if (it == shown.end() || it->second.key != a_key) {
				// Found anew: none of the book is in it (Book), unless the book is unsure, which can't tell a copy that holds
				// some of it already from one that doesn't: it is taken to show what it holds of the book then.
				Items fresh;
				for (const auto& [item, count] : a_have) {
					const auto wanted = want.find(item);
					if (const auto both = unsure && wanted != want.end() ? (std::min)(count, wanted->second) : 0; both > 0) {
						fresh[item] = both;
					}
				}
				if (!Track(a_local, a_key)) {
					return {};
				}
				it = shown.find(a_local);
				it->second.items = std::move(fresh);
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

		// Our copy a_local goes (WorkshopSync removes it, whichever way: a scrap, the session's end, a save's leftover, a cell
		// loading out of a session): what it shows of the book, to take out of it first, so that only what is our player's
		// own goes to their pockets (WorkshopApply::Rescues). The book's share is in the piece itself, in its builder's game
		// (or, the piece scrapped, in its builder's workbench): handed over too, it would be had twice.
		Share Going(std::uint32_t a_local)
		{
			const auto it = shown.find(a_local);
			if (it == shown.end()) {
				return { {}, !unsure };
			}
			Share share{ std::move(it->second.items), true };
			shown.erase(it);
			return share;
		}

		[[nodiscard]] const Items& Of(const Key& a_key) const
		{
			static const Items none;
			const auto         it = book.find(a_key);
			return it != book.end() ? it->second : none;
		}

		[[nodiscard]] std::size_t Pieces() const { return book.size(); }
		[[nodiscard]] std::size_t Copies() const { return shown.size(); }
		[[nodiscard]] bool        Unsure() const { return unsure; }
		[[nodiscard]] bool        Empty() const { return book.empty() && shown.empty() && !unsure; }

		// A different session: none of its changes are in the pieces. What our copies show stays (still the share of the
		// pieces' loot in them, to come out as they go).
		void NewSession() { book.clear(); }

		// The world's share can't be known (the save's record didn't read): a copy the book knows nothing of gives our player
		// nothing as it goes.
		void MarkUnsure() { unsure = true; }

		void Clear()
		{
			book.clear();
			shown.clear();
			unsure = false;
		}

		// For the save's record (WorldSync's): whether unsure; the number of pieces, then each piece's key, its count of items,
		// and each item and count; the number of copies, then each copy's ID, its piece's key, and its items the same way.
		[[nodiscard]] std::vector<std::uint32_t> Record() const
		{
			std::vector<std::uint32_t> out;
			const auto                 items = [&](const Items& a_items) {
                out.push_back(static_cast<std::uint32_t>(a_items.size()));
                for (const auto& [item, count] : a_items) {
                    out.push_back(item);
                    out.push_back(static_cast<std::uint32_t>((std::min<std::int64_t>)(count, Protocol::MAX_ITEM_COUNT)));
                }
			};
			out.push_back(unsure ? 1 : 0);
			out.push_back(static_cast<std::uint32_t>(book.size()));
			for (const auto& [key, held] : book) {
				out.push_back(key.builder);
				out.push_back(key.ref);
				items(held);
			}
			out.push_back(static_cast<std::uint32_t>(shown.size()));
			for (const auto& [local, copy] : shown) {
				out.push_back(local);
				out.push_back(copy.key.builder);
				out.push_back(copy.key.ref);
				items(copy.items);
			}
			return out;
		}

		// A record Record made; false for one that doesn't read, which leaves the book empty and unsure.
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
			const auto items = [&](Items& a_items) {
				std::uint32_t n = 0;
				if (!next(n) || n > MAX_ITEMS) {
					return false;
				}
				for (std::uint32_t i = 0; i < n; ++i) {
					std::uint32_t item = 0, count = 0;
					if (!next(item) || !next(count) || count > static_cast<std::uint32_t>(Protocol::MAX_ITEM_COUNT)) {
						return false;
					}
					if (count > 0) {
						a_items[item] = count;
					}
				}
				return true;
			};
			const auto fail = [&] {
				Clear();
				unsure = true;
				return false;
			};
			std::uint32_t flag = 0, pieces = 0, copies = 0;
			if (!next(flag) || flag > 1 || !next(pieces) || pieces > MAX_PIECES) {
				return fail();
			}
			for (std::uint32_t p = 0; p < pieces; ++p) {
				Key   key;
				Items held;
				if (!next(key.builder) || !next(key.ref) || !items(held)) {
					return fail();
				}
				if (!held.empty()) {
					book[key] = std::move(held);
				}
			}
			if (!next(copies) || copies > MAX_COPIES) {
				return fail();
			}
			for (std::uint32_t c = 0; c < copies; ++c) {
				std::uint32_t local = 0;
				Copy          copy;
				if (!next(local) || !next(copy.key.builder) || !next(copy.key.ref) || !items(copy.items)) {
					return fail();
				}
				shown[local] = std::move(copy);
			}
			if (at != a_record.size()) {
				return fail();
			}
			unsure = flag != 0;
			return true;
		}

	private:
		struct Copy
		{
			Key   key;    // the piece it is a copy of
			Items items;  // what it was last brought to
		};

		// Keeps a_local as a copy of a_key, none of the book in it yet (an ID our game gave again is that copy now); false when
		// no more can be kept, and the book is unsure from then on.
		bool Track(std::uint32_t a_local, const Key& a_key)
		{
			if (!shown.contains(a_local) && shown.size() >= MAX_COPIES) {
				unsure = true;
				return false;
			}
			shown[a_local] = Copy{ a_key, {} };
			return true;
		}

		std::map<Key, Items>          book;
		std::map<std::uint32_t, Copy> shown;  // our copies, by their own ID: what each was last brought to
		bool                          unsure = false;
	};
}
