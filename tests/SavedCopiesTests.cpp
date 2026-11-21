// The copies of friends' pieces a save holds (SavedCopies): named in each save's co-save record, and removed after its load
// as their cells load, out of a session. Each test names what a player would see go wrong if it failed: the host's desk, bed
// or house left standing for good in the friend's own game, unusable and unscrappable, or a piece of the player's own removed.

#include "Test.h"

#include "game/SavedCopies.h"

#include <map>

using SavedCopies::Book;
using SavedCopies::Entry;
using SavedCopies::Found;

namespace
{
	constexpr std::uint32_t DESK = 0xFF000A01;
	constexpr std::uint32_t BED = 0xFF000A02;
	constexpr std::uint32_t HOUSE = 0xFF000A03;
	constexpr std::uint32_t DESK_BASE = 0x0B012345;
	constexpr std::uint32_t BED_BASE = 0x00118F57;
	constexpr std::uint32_t HOUSE_BASE = 0x0B054321;

	using Clock = std::chrono::steady_clock;
	const Clock::time_point T0{};

	// The game as the book asks it: what each id names now, whether its 3D is in, and what was removed.
	struct FakeGame
	{
		std::map<std::uint32_t, Found> found;
		std::map<std::uint32_t, bool>  drawn;
		std::vector<std::uint32_t>     removed;

		Found Find(const Entry& a_entry)
		{
			const auto it = found.find(a_entry.id);
			return it == found.end() ? Found::kAbsent : it->second;
		}
		bool Ready(const Entry& a_entry, Clock::duration a_seen) { return drawn[a_entry.id] || a_seen >= std::chrono::seconds(15); }
		void Remove(const Entry& a_entry)
		{
			removed.push_back(a_entry.id);
			found[a_entry.id] = Found::kGone;
		}
	};
}

TEST("saved copies: the record names each copy once, with its base, and reads back the same")
{
	const std::vector<Entry> held{ { BED, BED_BASE }, { DESK, DESK_BASE }, { BED, BED_BASE }, { HOUSE, HOUSE_BASE } };
	const auto bytes = SavedCopies::Encode(held);
	const auto back = SavedCopies::Decode(bytes);
	CHECK(back.has_value());
	CHECK(back->size() == 3);  // the bed once
	CHECK((*back)[0] == (Entry{ DESK, DESK_BASE }));
	CHECK((*back)[1] == (Entry{ BED, BED_BASE }));
	CHECK((*back)[2] == (Entry{ HOUSE, HOUSE_BASE }));
}

TEST("saved copies: a save with no copy writes no record, and a plugin's reference or a copy with no base is never named")
{
	CHECK(SavedCopies::Encode(std::vector<Entry>{}).empty());
	// A plugin's own reference (a settlement's bed) is no copy of ours: naming it would have a load remove the settlement's bed.
	CHECK(SavedCopies::Encode(std::vector<Entry>{ { 0x00012345, BED_BASE }, { DESK, 0 } }).empty());
	const auto bytes = SavedCopies::Encode(std::vector<Entry>{ { 0x00012345, BED_BASE }, { DESK, DESK_BASE } });
	const auto back = SavedCopies::Decode(bytes);
	CHECK(back && back->size() == 1 && (*back)[0].id == DESK);
}

TEST("saved copies: a record that can't be read is ignored whole (another version, a wrong length, a plugin's id inside)")
{
	auto bytes = SavedCopies::Encode(std::vector<Entry>{ { DESK, DESK_BASE }, { BED, BED_BASE } });
	CHECK(SavedCopies::Decode(bytes).has_value());
	auto shorter = bytes;
	shorter.pop_back();
	CHECK(!SavedCopies::Decode(shorter));
	auto other = bytes;
	other[0] = 2;  // version 2
	CHECK(!SavedCopies::Decode(other));
	auto plugin = bytes;
	plugin[6 + 3] = 0x00;  // the first id's top byte: a plugin's reference now
	CHECK(!SavedCopies::Decode(plugin));
	CHECK(!SavedCopies::Decode(std::vector<std::uint8_t>{ 1, 0 }));
}

TEST("saved copies: loaded alone, the host's desk copy goes once its cell is loaded and its 3D is in; one whose cell isn't loaded waits")
{
	Book book;
	book.Load({ { DESK, DESK_BASE }, { HOUSE, HOUSE_BASE } });
	CHECK(book.Size() == 2);
	FakeGame game;
	game.found[DESK] = Found::kOurs;  // at the settlement the friend loaded at; the house is elsewhere (its cell not loaded)
	// Its 3D still loading: not yet (deleting a reference whose 3D is being loaded crashes the game).
	CHECK(book.Look(game, false, T0) == 0);
	CHECK(game.removed.empty());
	game.drawn[DESK] = true;
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(1)) == 1);
	CHECK(game.removed == std::vector<std::uint32_t>{ DESK });
	CHECK(book.Size() == 1);  // the house waits for its cell
	CHECK((book.Unsettled() == std::vector<Entry>{ { HOUSE, HOUSE_BASE } }));
	// The friend travels there: it goes too.
	game.found[HOUSE] = Found::kOurs;
	game.drawn[HOUSE] = true;
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(60)) == 1);
	CHECK(book.Empty());
	CHECK(book.Removed() == 2);
}

TEST("saved copies: a copy whose 3D never comes goes after the wait all the same, counted from when it was first found")
{
	Book book;
	book.Load({ { BED, BED_BASE } });
	FakeGame game;
	game.found[BED] = Found::kOurs;
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(100)) == 0);  // first found now
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(110)) == 0);
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(115)) == 1);
}

TEST("saved copies: in a session none is removed (the session's report finds it at its spot), and one the session keeps is settled")
{
	Book book;
	book.Load({ { DESK, DESK_BASE }, { BED, BED_BASE } });
	FakeGame game;
	game.found[DESK] = Found::kOurs;
	game.drawn[DESK] = true;
	game.found[BED] = Found::kKept;  // adopted by the host's report: the session's end removes it
	CHECK(book.Look(game, true, T0) == 0);
	CHECK(game.removed.empty());
	CHECK((book.Unsettled() == std::vector<Entry>{ { DESK, DESK_BASE } }));  // carried into the next save's record
	CHECK(book.KeptBySession() == 1);
	// The session ends without its report finding the desk (the host scrapped it meanwhile): out of the session it goes.
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(5)) == 1);
	CHECK(game.removed == std::vector<std::uint32_t>{ DESK });
}

TEST("saved copies: an id that names something else now is never removed: another base, a piece of the player's own, no block keyword")
{
	Book book;
	book.Load({ { DESK, DESK_BASE }, { BED, BED_BASE }, { HOUSE, HOUSE_BASE } });
	FakeGame game;
	game.found[DESK] = Found::kOther;  // the engine handed the id out again: the player's own desk
	game.found[BED] = Found::kGone;    // removed before the save was written: nothing under it
	CHECK(book.Look(game, false, T0) == 0);
	CHECK(game.removed.empty());
	CHECK(book.Size() == 1);  // only the house, whose cell isn't loaded
	CHECK(book.NotOurs() == 1);
	CHECK(book.AlreadyGone() == 1);
}

TEST("saved copies: a copy found, then its cell unloaded before its 3D came, waits afresh when it loads again")
{
	Book book;
	book.Load({ { BED, BED_BASE } });
	FakeGame game;
	game.found[BED] = Found::kOurs;
	CHECK(book.Look(game, false, T0) == 0);
	game.found.erase(BED);  // the player walked away
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(10)) == 0);
	game.found[BED] = Found::kOurs;  // and back: not "seen 20 s", its 3D may be loading again
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(20)) == 0);
	CHECK(book.Look(game, false, T0 + std::chrono::seconds(36)) == 1);
}

TEST("saved copies: a new load replaces what an earlier one named, and holds each id once, up to MAX")
{
	Book book;
	book.Load({ { DESK, DESK_BASE } });
	book.Load({ { BED, BED_BASE }, { BED, BED_BASE }, { 0x00012345, BED_BASE } });
	CHECK((book.Unsettled() == std::vector<Entry>{ { BED, BED_BASE } }));
	std::vector<Entry> many;
	for (std::uint32_t i = 1; i <= SavedCopies::MAX + 10; ++i) {
		many.push_back({ 0xFF000000 | i, BED_BASE });
	}
	book.Load(many);
	CHECK(book.Size() == SavedCopies::MAX);
	CHECK(SavedCopies::Decode(SavedCopies::Encode(many))->size() == SavedCopies::MAX);
	book.Load({});
	CHECK(book.Empty());
}
