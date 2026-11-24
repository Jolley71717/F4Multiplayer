#include "Test.h"
#include "game/PutAway.h"

#include <vector>

// A friend's own pieces at a settlement the host owns, put away for the session (src/game/PutAway.h): which pieces count,
// what the engine is told and when again, and what is shown again.
namespace
{
	using PutAway::Away;
	using PutAway::Piece;

	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x00020A30;

	Piece Wall(std::uint32_t a_ref, std::uint32_t a_bench = SANCTUARY)
	{
		return { .ref = a_ref, .bench = a_bench, .buildable = true };
	}
}

TEST("put away: only a friend's game in a session under the rule, with the switch on, puts anything away")
{
	CHECK(PutAway::Active(true, true));
	CHECK(!PutAway::Active(false, true));  // the local switch
	CHECK(!PutAway::Active(true, false));  // the host's game, no session, the rule off or the host gone
}

TEST("put away: a piece built before the session at a host's workbench counts; anything else stands")
{
	CHECK(PutAway::Eligible(Wall(0xFF000F38), true));
	CHECK(!PutAway::Eligible(Wall(0xFF000F38), false));  // the workbench is not the host's
	auto copy = Wall(0xFF000F39);
	copy.copy = true;  // the host's piece, our copy of it
	CHECK(!PutAway::Eligible(copy, true));
	auto scrapped = Wall(0xFF000F3A);
	scrapped.off = true;
	CHECK(!PutAway::Eligible(scrapped, true));
	auto world = Wall(0x0001ABCD);  // the settlement's own, in every game
	CHECK(!PutAway::Eligible(world, true));
	auto unbuildable = Wall(0xFF000F3B);
	unbuildable.buildable = false;
	CHECK(!PutAway::Eligible(unbuildable, true));
	auto unlinked = Wall(0xFF000F3C, 0);
	CHECK(!PutAway::Eligible(unlinked, true));
	auto made = Wall(0xFF000F3D);
	made.madeNow = true;  // built in the session (the host was away): it stands
	CHECK(!PutAway::Eligible(made, true));
}

TEST("put away: the engine is told once for each 3D, and again when the piece's 3D is made anew")
{
	Away away;
	away.Begin();
	CHECK(away.Want(0xFF000001, SANCTUARY, 0x1000));
	CHECK(!away.Want(0xFF000002, SANCTUARY, 0));  // its cell is not loaded: kept, nothing to tell
	away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY });
	CHECK(away.Has(0xFF000001));
	CHECK(away.Has(0xFF000002));
	away.Begin();
	CHECK(!away.Want(0xFF000001, SANCTUARY, 0x1000));  // the same 3D: no new call
	CHECK(away.Want(0xFF000002, SANCTUARY, 0x2000));   // loaded since
	away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY });
	away.Begin();
	CHECK(away.Want(0xFF000001, SANCTUARY, 0x3000));  // the engine made its 3D anew
	CHECK(!away.Want(0xFF000002, SANCTUARY, 0x2000));
	CHECK(away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY }).empty());
	CHECK(away.Size() == 2);
}

TEST("put away: a piece the workbench no longer lists is shown again; one whose workbench was not read stays")
{
	Away away;
	away.Begin();
	away.Want(0xFF000001, SANCTUARY, 0x1000);
	away.Want(0xFF000002, RED_ROCKET, 0x2000);
	away.End(std::vector<std::uint32_t>{ SANCTUARY, RED_ROCKET }, { RED_ROCKET, SANCTUARY });
	away.Begin();
	away.Want(0xFF000002, RED_ROCKET, 0x2000);  // Red Rocket's list only (Sanctuary's was read and no longer names the first)
	auto shown = away.End(std::vector<std::uint32_t>{ SANCTUARY, RED_ROCKET }, { RED_ROCKET, SANCTUARY });
	REQUIRE(shown.size() == 1);
	CHECK(shown[0].ref == 0xFF000001);
	CHECK(shown[0].root == 0x1000);  // the 3D the engine was told about, for the collision to be switched on again
	CHECK(!away.Has(0xFF000001));
	away.Begin();  // Red Rocket's workbench is not loaded this round: its pieces stay as they are
	shown = away.End(std::vector<std::uint32_t>{ SANCTUARY }, { RED_ROCKET, SANCTUARY });
	CHECK(shown.empty());
	CHECK(away.Has(0xFF000002));
}

TEST("put away: a workbench that stops being the host's shows its pieces again")
{
	Away away;
	away.Begin();
	away.Want(0xFF000001, SANCTUARY, 0x1000);
	away.Want(0xFF000002, RED_ROCKET, 0x2000);
	away.End(std::vector<std::uint32_t>{ SANCTUARY, RED_ROCKET }, { RED_ROCKET, SANCTUARY });
	away.Begin();  // the host lost Sanctuary
	away.Want(0xFF000002, RED_ROCKET, 0x2000);
	const auto shown = away.End(std::vector<std::uint32_t>{ RED_ROCKET }, { RED_ROCKET });
	REQUIRE(shown.size() == 1);
	CHECK(shown[0].ref == 0xFF000001);
	CHECK(shown[0].bench == SANCTUARY);
	// ...and gets it back: put away again, the engine told again
	away.Begin();
	CHECK(away.Want(0xFF000001, SANCTUARY, 0x1000));
}

TEST("put away: leaving the session shows everything, each once")
{
	Away away;
	away.Begin();
	away.Want(0xFF000003, SANCTUARY, 0x3000);
	away.Want(0xFF000001, SANCTUARY, 0x1000);
	away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY });
	const auto all = away.All();
	REQUIRE(all.size() == 2);
	CHECK(all[0].ref == 0xFF000001);
	CHECK(all[1].ref == 0xFF000003);
	CHECK(away.Size() == 0);
	CHECK(away.All().empty());
	CHECK(!away.Has(0xFF000001));
}

TEST("put away: the count at each workbench is said once, and again when it changes")
{
	Away away;
	away.Begin();
	away.Want(0xFF000001, SANCTUARY, 0x1000);
	away.Want(0xFF000002, SANCTUARY, 0x2000);
	away.Want(0xFF000003, SANCTUARY, 0);  // not loaded: not counted
	away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY });
	auto said = away.Changed();
	REQUIRE(said.size() == 1);
	CHECK(said[0].first == SANCTUARY);
	CHECK(said[0].second == 2);
	CHECK(away.At(SANCTUARY) == 2);
	CHECK(away.Changed().empty());
	away.Begin();
	away.Want(0xFF000001, SANCTUARY, 0x1000);
	away.Want(0xFF000002, SANCTUARY, 0x2000);
	away.Want(0xFF000003, SANCTUARY, 0x3000);
	away.End(std::vector<std::uint32_t>{ SANCTUARY }, { SANCTUARY });
	said = away.Changed();
	REQUIRE(said.size() == 1);
	CHECK(said[0].second == 3);
	away.All();
	CHECK(away.Changed().empty());
}
