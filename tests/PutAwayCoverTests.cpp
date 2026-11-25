#include "Test.h"
#include "game/PutAway.h"

#include <vector>

// Only a piece the host's version covers is put away: a copy of the same base, at the same workbench, within a few units.
namespace
{
	using PutAway::Spot;

	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t BED = 0x0005DE9B;
	constexpr std::uint32_t GENERATOR = 0x00062F0D;

	Spot Mine(std::uint32_t a_base = BED, float a_x = 100.0f)
	{
		return { a_base, BENCH, a_x, 200.0f, 50.0f };
	}
}

TEST("PutAway: no host copy near, nothing covers the piece") {
	CHECK(!PutAway::Covered(Mine(), {}));
}

TEST("PutAway: a copy of the same base at the same spot covers it") {
	CHECK(PutAway::Covered(Mine(), { Mine() }));
}

TEST("PutAway: a copy within the tolerance covers it, one past it does not") {
	CHECK(PutAway::Covered(Mine(), { Mine(BED, 100.0f + PutAway::COVER_TOLERANCE) }));
	CHECK(!PutAway::Covered(Mine(), { Mine(BED, 100.0f + PutAway::COVER_TOLERANCE + 0.5f) }));
}

TEST("PutAway: a copy of another base does not cover it, whatever kind the piece is") {
	CHECK(!PutAway::Covered(Mine(BED), { Mine(GENERATOR) }));
	CHECK(PutAway::Covered(Mine(GENERATOR), { Mine(BED), Mine(GENERATOR) }));
}

TEST("PutAway: a copy at another workbench does not cover it, nor one with none") {
	Spot other = Mine();
	other.bench = 0x00012345;
	CHECK(!PutAway::Covered(Mine(), { other }));
	other.bench = 0;
	CHECK(!PutAway::Covered(Mine(), { other }));
}

TEST("PutAway: a copy off by height does not cover it") {
	Spot up = Mine();
	up.z += 30.0f;
	CHECK(!PutAway::Covered(Mine(), { up }));
}

TEST("PutAway: a piece is covered when a copy appears, and uncovered when it goes") {
	std::vector<Spot> copies;
	CHECK(!PutAway::Covered(Mine(), copies));
	copies.push_back(Mine());
	CHECK(PutAway::Covered(Mine(), copies));
	copies.clear();
	CHECK(!PutAway::Covered(Mine(), copies));
}

TEST("PutAway: a round that no longer lists a piece shows it again") {
	PutAway::Away away;
	away.Begin();
	CHECK(away.Want(0xFF000001, BENCH, 0x1000));
	CHECK(away.End(std::vector<std::uint32_t>{ BENCH }, std::vector<std::uint32_t>{ BENCH }).empty());
	away.Begin();  // the copy went: the piece is not wanted this round
	const auto shown = away.End(std::vector<std::uint32_t>{ BENCH }, std::vector<std::uint32_t>{ BENCH });
	CHECK(shown.size() == 1);
	CHECK(!away.Has(0xFF000001));
}
