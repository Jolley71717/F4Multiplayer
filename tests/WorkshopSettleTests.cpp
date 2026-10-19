#include "Test.h"
#include "game/WorkshopSettle.h"

#include <map>
#include <vector>

// A piece placed or moved is looked at again over the next seconds, and reported again where it ends up.
namespace
{
	using WorkshopSettle::Clock;
	using WorkshopSettle::AFTER;
	using namespace std::chrono_literals;

	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t BED = 0xFF000F38;

	WorkshopMove::Transform At(float a_x, float a_rotZ = 0.0f)
	{
		return { { a_x, 89000.0f, 7800.0f }, { 0.0f, 0.0f, a_rotZ }, 1.0f };
	}

	struct FakeGame
	{
		std::map<std::uint32_t, WorkshopMove::Transform> standing;
		std::vector<std::uint32_t>                       reported;
		bool                                             full = false;

		std::optional<WorkshopMove::Transform> TransformOf(std::uint32_t a_ref)
		{
			const auto it = standing.find(a_ref);
			return it == standing.end() ? std::nullopt : std::optional{ it->second };
		}

		bool Report(std::uint32_t a_ref, std::uint32_t)
		{
			if (full) {
				return false;
			}
			reported.push_back(a_ref);
			return true;
		}
	};
}

TEST("settle: a piece the game set somewhere else after it was placed is reported again where it stands")
{
	WorkshopSettle::Watch watch;
	FakeGame              game;
	const auto            t0 = Clock::now();
	game.standing[BED] = At(-80342.0f);
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	CHECK(watch.Check(game, t0 + 100ms) == 0);  // not due yet
	game.standing[BED] = At(-80202.0f);          // the game put it down where the player saw it
	CHECK(watch.Check(game, t0 + AFTER[0]) == 1);
	CHECK((game.reported == std::vector<std::uint32_t>{ BED }));
	CHECK(watch.Check(game, t0 + AFTER[0] + AFTER[0]) == 0);  // reported where it stands: nothing to send
	CHECK(watch.Watched() == 1);  // still watched, as long again
}

TEST("settle: a turn alone is reported again too")
{
	WorkshopSettle::Watch watch;
	FakeGame              game;
	const auto            t0 = Clock::now();
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	game.standing[BED] = At(-80342.0f, 1.5f);
	CHECK(watch.Check(game, t0 + AFTER[0]) == 1);
}

TEST("settle: a piece that stays put is looked at a few times, then let go")
{
	WorkshopSettle::Watch watch;
	FakeGame              game;
	const auto            t0 = Clock::now();
	game.standing[BED] = At(-80342.0f);
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	for (const auto after : AFTER) {
		CHECK(watch.Watched() == 1);
		CHECK(watch.Check(game, t0 + after) == 0);
	}
	CHECK(watch.Watched() == 0);
	CHECK(game.reported.empty());
}

TEST("settle: a piece scrapped or stored meanwhile is let go, and one moved again is watched from its new report")
{
	WorkshopSettle::Watch watch;
	FakeGame              game;
	const auto            t0 = Clock::now();
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	CHECK(watch.Check(game, t0 + AFTER[0]) == 0);  // not standing any more
	CHECK(watch.Watched() == 0);

	game.standing[BED] = At(-80100.0f);
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	watch.Reported(BED, BENCH, At(-80100.0f), t0 + 1s);  // the player moved it: one entry, from the move
	CHECK(watch.Watched() == 1);
	CHECK(watch.Check(game, t0 + 1s + AFTER[0]) == 0);
	CHECK(game.reported.empty());
}

TEST("settle: a report the send queue had no room for is tried at the next look, and few are watched at once")
{
	WorkshopSettle::Watch watch;
	FakeGame              game;
	const auto            t0 = Clock::now();
	watch.Reported(BED, BENCH, At(-80342.0f), t0);
	game.standing[BED] = At(-80202.0f);
	game.full = true;
	CHECK(watch.Check(game, t0 + AFTER[0]) == 0);
	game.full = false;
	CHECK(watch.Check(game, t0 + AFTER[1]) == 1);

	for (std::uint32_t i = 0; i < WorkshopSettle::MAX_WATCHED + 5; ++i) {
		watch.Reported(0xFF100000 + i, BENCH, At(0.0f), t0);
	}
	CHECK(watch.Watched() == WorkshopSettle::MAX_WATCHED);
}
