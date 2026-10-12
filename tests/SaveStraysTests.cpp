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

TEST("save strays: gone means in no cell, and deleted or without its base")
{
	CHECK(!SaveStrays::Gone({ .inCell = true, .hasBase = false, .deleted = true }));
	CHECK(SaveStrays::Gone({ .inCell = false, .hasBase = true, .deleted = true }));
	CHECK(SaveStrays::Gone({ .inCell = false, .hasBase = false, .deleted = false }));
	CHECK(!SaveStrays::Gone({ .inCell = false, .hasBase = true, .deleted = false }));  // only held back
}

namespace
{
	// As RemotePlayers::IsStandInBase: here a settler and an NPC a friend picked as their look.
	bool StandInBase(std::uint32_t a_base)
	{
		return a_base == 0x0020A578 || a_base == 0x0001A4D7;
	}
}

TEST("save strays: placed from a base stand-ins are placed from, or a face copy, is a stand-in")
{
	CHECK(SaveStrays::FromStandInBase({ .base = 0x0020A578 }, StandInBase));
	CHECK(SaveStrays::FromStandInBase({ .base = 0x0001A4D7 }, StandInBase));  // a look the face could not go on
	CHECK(SaveStrays::FromStandInBase({ .base = 0xFF0015B7 }, StandInBase));  // a face copy
	CHECK(!SaveStrays::FromStandInBase({ .base = 0x0001D41F }, StandInBase));  // an NPC of the game's
	CHECK(!SaveStrays::FromStandInBase({}, StandInBase));
	// So one an earlier version saved deleted is kept out of saves; the game's is left alone.
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = SaveStrays::FromStandInBase({ .base = 0x0001A4D7 }, StandInBase) }));
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = SaveStrays::FromStandInBase({ .base = 0x0001D41F }, StandInBase) }));
}

TEST("save strays: a leveled actor counts by what it was placed from, not its own base")
{
	// A look with a leveled template: its base is the game's copy, its leveled data names the look.
	CHECK(SaveStrays::FromStandInBase({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0x0001A4D7 }, StandInBase));
	CHECK(SaveStrays::FromStandInBase({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0xFF0015B7 }, StandInBase));  // a face copy of one
	// The game's: from a leveled list or one of its NPCs, or from nothing it names.
	CHECK(!SaveStrays::FromStandInBase({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0x0002F3B8 }, StandInBase));
	CHECK(!SaveStrays::FromStandInBase({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0 }, StandInBase));
}

TEST("save strays: the stand-ins' faction change is the player's faction at rank 1, as Spawn sets it")
{
	CHECK(SaveStrays::StandInFaction(0x0001C21C, 1));
	CHECK(!SaveStrays::StandInFaction(0x0001C21C, 0));   // a script's AddToFaction
	CHECK(!SaveStrays::StandInFaction(0x0001C21C, -1));  // taken out again
	CHECK(!SaveStrays::StandInFaction(0x00023C01, 1));   // another faction
	static_assert(SaveStrays::STAND_IN_FACTION == 0x0001C21C && SaveStrays::STAND_IN_RANK == 1);  // what every version set
}

TEST("save strays: one carrying the stand-ins' faction change is a stand-in before its friend's look is known")
{
	// Placed straight on a look no friend has asked for this run (0x0001A4D8): only the faction tells.
	CHECK(!SaveStrays::FromStandInBase({ .base = 0x0001A4D8 }, StandInBase));
	CHECK(SaveStrays::StandIn({ .base = 0x0001A4D8, .standInFaction = true }, StandInBase));
	CHECK(!SaveStrays::StandIn({ .base = 0x0001A4D8 }, StandInBase));
	// Either tells on its own.
	CHECK(SaveStrays::StandIn({ .base = 0x0001A4D7 }, StandInBase));
	CHECK(SaveStrays::StandIn({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0x0002F3B8, .standInFaction = true }, StandInBase));
	// So it is kept out of saves when deleted in no cell, and left alone otherwise.
	const bool ours = SaveStrays::StandIn({ .base = 0x0001A4D8, .standInFaction = true }, StandInBase);
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = ours }));
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = ours }));
	CHECK(!SaveStrays::Stray({ .inCell = true, .hasBase = true, .deleted = true, .ours = ours }));
}

TEST("save strays: created ids are the FF ones")
{
	CHECK(SaveStrays::IsCreatedId(0xFF000001));
	CHECK(!SaveStrays::IsCreatedId(0x0020A578));
	CHECK(!SaveStrays::IsCreatedId(0xFE001000));  // a light plugin's
	CHECK(SaveStrays::PlacedFrom({ .base = 0xFF002000, .leveled = true, .leveledFrom = 0x0001A4D7 }) == 0x0001A4D7);
	CHECK(SaveStrays::PlacedFrom({ .base = 0x0001A4D7 }) == 0x0001A4D7);
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

TEST("save strays: saves are searched only after a stand-in, or a load that brought gone actors")
{
	CHECK(!SaveStrays::SearchBeforeSave(false, false));
	CHECK(SaveStrays::SearchBeforeSave(true, false));
	CHECK(SaveStrays::SearchBeforeSave(false, true));
}
