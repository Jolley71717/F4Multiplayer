#include "Test.h"
#include "game/BuriedPieces.h"

#include <map>
#include <vector>

// What WorkshopSync buried this session, found back alive after its cell loaded again, is buried again.
namespace
{
	using BuriedPieces::Clock;
	using BuriedPieces::Facts;
	using BuriedPieces::Verdict;
	using namespace std::chrono_literals;

	constexpr std::uint32_t COPY = 0xFF000E8A;  // the friend's generator copy the rig saw come back
	constexpr std::uint32_t GENERATOR = 0x00030386;
	constexpr std::array<float, 3> SPOT{ -80000.0f, 90000.0f, 7800.0f };

	struct Game
	{
		std::map<std::uint32_t, Facts> facts;

		Facts operator()(const BuriedPieces::Entry& a_entry) const
		{
			const auto it = facts.find(a_entry.id);
			return it != facts.end() ? it->second : Facts{};
		}
	};

	Facts Buried()
	{
		return { .present = true, .base = GENERATOR, .deleted = true, .has3D = true };
	}

	Facts Back(bool a_has3D = true)
	{
		return { .present = true, .base = GENERATOR, .deleted = false, .has3D = a_has3D };
	}
}

TEST("buried pieces: a buried copy still deleted is kept watched, nothing done")
{
	BuriedPieces::Book book;
	CHECK(book.Add(COPY, GENERATOR, SPOT));
	Game game;
	game.facts[COPY] = Buried();
	CHECK(book.Check(game, Clock::now()).empty());
	CHECK(book.Contains(COPY));
}

TEST("buried pieces: one back alive from its record (the cell loaded again) is buried again")
{
	BuriedPieces::Book book;
	book.Add(COPY, GENERATOR, SPOT);
	Game game;
	game.facts[COPY] = Back();
	const auto found = book.Check(game, Clock::now());
	REQUIRE(found.size() == 1);
	CHECK(found[0].first.id == COPY);
	CHECK(found[0].second == Verdict::kBuryAgain);
	CHECK(found[0].first.burials == 1);
	CHECK(book.Contains(COPY));
}

TEST("buried pieces: one back without its 3D waits for it (or for the loader's grace), then is buried again")
{
	BuriedPieces::Book book;
	book.Add(COPY, GENERATOR, SPOT);
	Game       game;
	const auto start = Clock::now();
	game.facts[COPY] = Back(false);
	auto found = book.Check(game, start);
	REQUIRE(found.size() == 1);
	CHECK(found[0].second == Verdict::kWait);
	found = book.Check(game, start + 5s);
	REQUIRE(found.size() == 1);
	CHECK(found[0].second == Verdict::kWait);
	found = book.Check(game, start + DeferredDelete::MAX_WAIT);
	REQUIRE(found.size() == 1);
	CHECK(found[0].second == Verdict::kBuryAgain);
}

TEST("buried pieces: gone with its record still there is kept (its cell could bring it back); gone with none, forgotten")
{
	BuriedPieces::Book book;
	book.Add(COPY, GENERATOR, SPOT);
	Game game;
	game.facts[COPY] = Facts{ .present = false, .hasRecord = true };
	CHECK(book.Check(game, Clock::now()).empty());
	CHECK(book.Contains(COPY));
	game.facts[COPY] = Facts{ .present = false, .hasRecord = false };
	CHECK(book.Check(game, Clock::now()).empty());
	CHECK(!book.Contains(COPY));  // the id is free: the engine may hand it to anything next
}

TEST("buried pieces: the id handed out again (another base, a copy or a build of ours, another spot) is never touched")
{
	Game game;
	for (const auto& now : { Facts{ .present = true, .base = 0x0001D971 }, Facts{ .present = true, .base = GENERATOR, .ours = true },
			 Facts{ .present = true, .base = GENERATOR, .fromSpot = 500.0f, .has3D = true } }) {
		BuriedPieces::Book book;
		book.Add(COPY, GENERATOR, SPOT);
		game.facts[COPY] = now;
		CHECK(book.Check(game, Clock::now()).empty());
		CHECK(!book.Contains(COPY));
	}
}

TEST("buried pieces: each id forgotten is told (for SaveGuard to let go of it), once; one kept or buried again is not")
{
	constexpr std::uint32_t KEPT = 0xFF000E8B;
	constexpr std::uint32_t AGAIN = 0xFF000E8C;
	constexpr std::uint32_t PLAYERS = 0xFF000E8D;  // the engine gave the freed id to the player's own wire: a build of theirs
	constexpr std::uint32_t FREED = 0xFF000E8E;
	BuriedPieces::Book book;
	for (const auto id : { KEPT, AGAIN, PLAYERS, FREED }) {
		book.Add(id, GENERATOR, SPOT);
	}
	Game game;
	game.facts[KEPT] = Buried();
	game.facts[AGAIN] = Back();
	game.facts[PLAYERS] = Facts{ .present = true, .base = GENERATOR, .ours = true, .has3D = true };
	game.facts[FREED] = Facts{ .present = false, .hasRecord = false };
	std::vector<std::uint32_t> forgot;
	const auto                 found = book.Check(game, Clock::now(), [&](const BuriedPieces::Entry& a_entry) { forgot.push_back(a_entry.id); });
	CHECK((forgot == std::vector<std::uint32_t>{ PLAYERS, FREED }));
	REQUIRE(found.size() == 1);
	CHECK(found[0].first.id == AGAIN);
	forgot.clear();
	book.Check(game, Clock::now(), [&](const BuriedPieces::Entry& a_entry) { forgot.push_back(a_entry.id); });
	CHECK(forgot.empty());  // gone from the book: told once
}

TEST("buried pieces: one that keeps coming back is buried again MAX_BURIALS times, then given up (said once)")
{
	BuriedPieces::Book book;
	book.Add(COPY, GENERATOR, SPOT);
	Game game;
	game.facts[COPY] = Back();
	for (std::uint8_t i = 0; i < BuriedPieces::MAX_BURIALS; ++i) {
		const auto found = book.Check(game, Clock::now());
		REQUIRE(found.size() == 1);
		CHECK(found[0].second == Verdict::kBuryAgain);
		book.Add(COPY, GENERATOR, SPOT);  // what Bury does again: the count is kept
	}
	const auto found = book.Check(game, Clock::now());
	REQUIRE(found.size() == 1);
	CHECK(found[0].second == Verdict::kGiveUp);
	CHECK(!book.Contains(COPY));
	CHECK(book.Check(game, Clock::now()).empty());
}

TEST("buried pieces: buried again is no new burial to say; the book holds at most MAX, the oldest going first")
{
	BuriedPieces::Book book;
	CHECK(book.Add(COPY, GENERATOR, SPOT));
	CHECK(!book.Add(COPY, GENERATOR, SPOT));
	CHECK(!book.Add(0, GENERATOR, SPOT));
	for (std::uint32_t i = 1; i <= BuriedPieces::MAX; ++i) {
		book.Add(COPY + i, GENERATOR, SPOT);
	}
	CHECK(book.Size() == BuriedPieces::MAX);
	CHECK(!book.Contains(COPY));
	CHECK(book.Contains(COPY + BuriedPieces::MAX));
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("buried pieces: a judge's order: deleted first, then whose it is")
{
	BuriedPieces::Entry entry{ COPY, GENERATOR, SPOT };
	CHECK(BuriedPieces::Judge(entry, Facts{ .present = true, .base = GENERATOR, .deleted = true, .ours = true }, 0s) == Verdict::kKeep);
	CHECK(BuriedPieces::Judge(entry, Facts{ .present = true, .base = GENERATOR, .fromSpot = BuriedPieces::SAME_SPOT, .has3D = true }, 0s) == Verdict::kBuryAgain);
}
