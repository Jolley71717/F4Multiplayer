#include "Test.h"
#include "game/SaveStrays.h"

// Which created actors are kept out of a save (SaveGuard).
TEST("save strays: an actor in a cell is never one")
{
	CHECK(!SaveStrays::Stray({ .inCell = true, .hasBase = false, .deleted = true, .ours = true }));
	CHECK(!SaveStrays::Stray({ .inCell = true, .hasBase = true, .deleted = true, .ours = true }));
}

TEST("save strays: one in no cell whose base is gone is, whoever made it")
{
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = false, .deleted = true, .ours = false }));
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = false, .deleted = false, .ours = false }));
}

TEST("save strays: a deleted stand-in in no cell is")
{
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = true }));
}

TEST("save strays: an actor the game deleted, or holds in no cell, is left alone")
{
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = false }));
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = false }));
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = true }));  // ours, only disabled
}

TEST("save strays: an id is ours only with the base it was placed from")
{
	SaveStrays::Placed placed;
	CHECK(!placed.Any());
	placed.Add(0xFF001234, 0x0020A578);
	CHECK(placed.Any());
	CHECK(placed.Ours(0xFF001234, 0x0020A578));
	CHECK(!placed.Ours(0xFF001234, 0x0001D41F));  // the game gave the id to an actor of its own
	CHECK(!placed.Ours(0xFF001235, 0x0020A578));
}

TEST("save strays: a load keeps only the stand-ins it brought back, and remembers there were some")
{
	SaveStrays::Placed placed;
	placed.Add(0xFF000010, 0x0020A578);
	placed.Add(0xFF000011, 0xFF000F00);
	placed.KeepOnly({ 0xFF000011, 0xFF000099 });
	CHECK(placed.Size() == 1);
	CHECK(!placed.Ours(0xFF000010, 0x0020A578));
	CHECK(placed.Ours(0xFF000011, 0xFF000F00));
	placed.KeepOnly({});
	CHECK(placed.Size() == 0);
	CHECK(placed.Any());
}

TEST("save strays: saves are searched only after a stand-in, or a load that brought strays")
{
	CHECK(!SaveStrays::SearchBeforeSave(false, false));
	CHECK(SaveStrays::SearchBeforeSave(true, false));
	CHECK(SaveStrays::SearchBeforeSave(false, true));
}
