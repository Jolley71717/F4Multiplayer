#include "Test.h"
#include "game/BuiltWalk.h"

#include <vector>

// What a player built before the session: whose game reports it, which references count as its building,
// each reported once a session, a few a frame, and what can't be reported yet kept for the next frame.
namespace
{
	using BuiltWalk::Piece;

	constexpr std::uint32_t BENCH = 0x000250FE;  // Sanctuary's workbench

	Piece Wall(std::uint32_t a_ref)
	{
		return { .ref = a_ref, .bench = BENCH, .buildable = true };
	}

	struct FakeGame
	{
		std::vector<std::uint32_t> reported;
		bool                       full = false;  // the send queue has no room

		bool Report(const Piece& a_piece)
		{
			if (full) {
				return false;
			}
			reported.push_back(a_piece.ref);
			return true;
		}
	};
}

TEST("built walk: only what the game made at runtime, linked to a workbench, buildable, standing and no copy counts as built")
{
	CHECK(BuiltWalk::Built(Wall(0xFF000F38)));
	auto worldPiece = Wall(0x0001ABCD);  // a piece the settlement came with: in every game already
	CHECK(!BuiltWalk::Built(worldPiece));
	auto loose = Wall(0xFF000F39);  // something dropped, or a spawned NPC: no workbench's
	loose.bench = 0;
	CHECK(!BuiltWalk::Built(loose));
	auto creature = Wall(0xFF000F3A);
	creature.buildable = false;
	CHECK(!BuiltWalk::Built(creature));
	auto copy = Wall(0xFF000F3B);  // a friend's piece, ours only as a copy
	copy.copy = true;
	CHECK(!BuiltWalk::Built(copy));
	auto stored = Wall(0xFF000F3C);  // stored or scrapped, on its way out
	stored.off = true;
	CHECK(!BuiltWalk::Built(stored));
	CHECK(!BuiltWalk::Built(Wall(0)));
	CHECK(!BuiltWalk::Built(Wall(Protocol::PLAYER_REF_ID)));
}

TEST("built walk: each piece is reported once a session, however often its cell attaches, and again after a new session")
{
	BuiltWalk::Walk built;
	FakeGame        game;
	CHECK(built.Offer(Wall(0xFF000F38)));
	CHECK(built.Offer(Wall(0xFF000F44)));
	CHECK(!built.Offer(Wall(0xFF000F38)));  // its cell attached again, or its workbench listed it too
	CHECK(built.ReportSome(game) == 2);
	CHECK((game.reported == std::vector<std::uint32_t>{ 0xFF000F38, 0xFF000F44 }));
	CHECK(!built.Offer(Wall(0xFF000F44)));
	CHECK(built.ReportSome(game) == 0);

	built.Clear();  // a new session, or a save loaded
	CHECK(built.Offer(Wall(0xFF000F38)));
	CHECK(built.ReportSome(game) == 1);
	CHECK(built.Reported() == 3);
}

TEST("built walk: a big settlement goes a few pieces a frame, in the order found, and waits while the send queue is full")
{
	BuiltWalk::Walk built;
	FakeGame        game;
	constexpr std::uint32_t PIECES = 100;
	for (std::uint32_t i = 0; i < PIECES; ++i) {
		built.Offer(Wall(0xFF100000 + i));
	}
	CHECK(built.ReportSome(game) == BuiltWalk::REPORT_PER_FRAME);
	game.full = true;
	CHECK(built.ReportSome(game) == 0);
	CHECK(built.Queued() == PIECES - BuiltWalk::REPORT_PER_FRAME);
	game.full = false;
	while (built.ReportSome(game) > 0) {}
	REQUIRE(game.reported.size() == PIECES);
	for (std::uint32_t i = 0; i < PIECES; ++i) {
		CHECK(game.reported[i] == 0xFF100000 + i);  // none skipped while it waited, none twice
	}
}

TEST("built walk: the host's game reports what it built before the session either way, a guest's only with the host's bShareGuestBuilds")
{
	Protocol::SessionRules rules;  // as an ini without the key hosts
	CHECK(BuiltWalk::Shares(true, rules));
	CHECK(!BuiltWalk::Shares(false, rules));
	rules.shareGuestBuilds = true;
	CHECK(BuiltWalk::Shares(true, rules));
	CHECK(BuiltWalk::Shares(false, rules));
}

TEST("built walk: what is queued stays within what the server would keep")
{
	BuiltWalk::Walk built;
	for (std::uint32_t i = 0; i < BuiltWalk::MAX_QUEUED + 10; ++i) {
		built.Offer(Wall(0xFF000000 + i));
	}
	CHECK(built.Queued() == BuiltWalk::MAX_QUEUED);
}
