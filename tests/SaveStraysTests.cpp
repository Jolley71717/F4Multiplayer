#include "Test.h"
#include "game/SaveStrays.h"

#include <algorithm>
#include <map>

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

TEST("save strays: one the engine cannot save under a cell or a worldspace is a stray, whoever placed it, deleted or not")
{
	// The 10-11 crash: a stand-in alive, with its base, in no cell after the host's cells were cleared under it.
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = true, .anchored = false }));
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = false, .anchored = false }));
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = true, .anchored = false }));
	CHECK(SaveStrays::Doomed({ .anchored = false }));
	CHECK(!SaveStrays::Doomed({}));
}

TEST("save strays: with a worldspace to be saved under, the old rules stand")
{
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = false, .ours = true, .anchored = true }));  // ours, only disabled
	CHECK(SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = true, .anchored = true }));
	CHECK(!SaveStrays::Stray({ .inCell = false, .hasBase = true, .deleted = true, .ours = false, .anchored = true }));
	CHECK(!SaveStrays::Gone({ .inCell = false, .hasBase = true, .deleted = false, .anchored = false }));  // doomed is not gone
}

TEST("save strays: a save keeps every stray out; right after a load only ours are dropped, the game's only logged")
{
	using SaveStrays::Judge;
	using SaveStrays::Verdict;
	using SaveStrays::When;
	const SaveStrays::Actor doomedOurs{ .inCell = false, .hasBase = true, .ours = true, .anchored = false };
	const SaveStrays::Actor doomedGames{ .inCell = false, .hasBase = true, .ours = false, .anchored = false };
	const SaveStrays::Actor baseGone{ .inCell = false, .hasBase = false, .deleted = true };
	const SaveStrays::Actor heldBack{ .inCell = false, .hasBase = true, .deleted = false };
	CHECK(Judge(doomedOurs, When::kSave) == Verdict::kExclude);
	CHECK(Judge(doomedGames, When::kSave) == Verdict::kExclude);
	CHECK(Judge(baseGone, When::kSave) == Verdict::kExclude);
	CHECK(Judge(heldBack, When::kSave) == Verdict::kLeave);
	CHECK(Judge(doomedOurs, When::kLoad) == Verdict::kExclude);
	CHECK(Judge(doomedGames, When::kLoad) == Verdict::kTell);  // its cell may be on its way as the load settles
	CHECK(Judge(baseGone, When::kLoad) == Verdict::kTell);
	CHECK(Judge(heldBack, When::kLoad) == Verdict::kLeave);
}

namespace
{
	// A game's created references as SaveGuard sees them through a session: a stand-in, its frame copy and light as
	// RemotePlayers places and tracks them, one of the game's own, and the game handing ids out again after a load.
	// Saved the way SaveGuard's sweep and then the engine's save go.
	struct World
	{
		struct Ref
		{
			std::uint32_t base = 0;
			bool          inCell = true;
			bool          worldspace = true;  // the engine's anchor besides the cell
			bool          deleted = false;
			bool          record = true;
		};
		std::map<std::uint32_t, Ref> refs;
		SaveStrays::Placed           placed;
		SaveStrays::Forgotten        forgotten;
		std::uint32_t                nextId = 0xFF000EB0;

		std::uint32_t Place(std::uint32_t a_base, bool a_ours)
		{
			const auto id = nextId++;
			refs[id] = { .base = a_base };
			if (a_ours) {
				placed.Add(id, a_base);
			}
			return id;
		}

		std::optional<std::uint32_t> BaseNow(std::uint32_t a_id) const
		{
			const auto it = refs.find(a_id);
			return it != refs.end() ? std::optional{ it->second.base } : std::nullopt;
		}

		SaveStrays::Actor Seen(std::uint32_t a_id) const
		{
			const auto& ref = refs.at(a_id);
			return { .inCell = ref.inCell, .hasBase = true, .deleted = ref.deleted, .ours = placed.Ours(a_id, ref.base), .anchored = ref.inCell || ref.worldspace };
		}

		// The sweep, then what the engine writes: every reference left with a record. Returns the ids written.
		std::vector<std::uint32_t> Save()
		{
			forgotten.Keep([&](std::uint32_t a_id) { return BaseNow(a_id); }, [&](std::uint32_t a_id) { refs[a_id].record = false; });
			for (auto& [id, ref] : refs) {
				if (SaveStrays::Judge(Seen(id), SaveStrays::When::kSave) == SaveStrays::Verdict::kExclude) {
					ref.record = false;
				}
			}
			std::vector<std::uint32_t> written;
			for (const auto& [id, ref] : refs) {
				if (ref.record) {
					written.push_back(id);
				}
			}
			return written;
		}

		// A doomed reference written is the crash.
		bool WritesDoomed(const std::vector<std::uint32_t>& a_written) const
		{
			return std::ranges::any_of(a_written, [&](std::uint32_t a_id) { return SaveStrays::Doomed(Seen(a_id)); });
		}

		static bool Has(const std::vector<std::uint32_t>& a_ids, std::uint32_t a_id) { return std::ranges::find(a_ids, a_id) != a_ids.end(); }
	};
}

TEST("save strays: a session's stand-in, frame copy and light never reach a save, and ids a load hands out again stay the game's")
{
	World world;
	const auto standIn = world.Place(0xFF0009A8, true);   // on a face copy
	const auto frame = world.Place(0x0017F6E4, true);     // the frame copy it got into
	const auto light = world.Place(0x0001ED2D, true);     // its Pip-Boy light
	const auto settler = world.Place(0x0002F3B8, false);  // one of the game's, in a cell
	// The host follows a friend (coc), and the cells are cleared before the stand-in could be taken out: ours are
	// left with neither a cell nor a worldspace, alive and not deleted.
	for (const auto id : { standIn, frame, light }) {
		world.refs[id].inCell = false;
		world.refs[id].worldspace = false;
	}
	auto written = world.Save();
	CHECK(!world.WritesDoomed(written));
	CHECK(World::Has(written, settler));
	CHECK(!World::Has(written, standIn));
	CHECK(!World::Has(written, frame));

	// The game took the light from under us (its handle stopped resolving) while it was still in a cell, and a change
	// made it a record again. Forgotten, it stays out of the save, though no sweep sees it as a stray.
	world.refs[light] = { .base = 0x0001ED2D };
	world.forgotten.Add(light, 0x0001ED2D);
	written = world.Save();
	CHECK(!World::Has(written, light));
	CHECK(World::Has(written, settler));

	// A load: what was placed before it is gone, and the game hands the same ids out to references of its own.
	world.refs.clear();
	world.forgotten.Clear();
	world.placed.KeepOnly({});
	world.nextId = 0xFF000EB0;
	const auto reused = world.Place(0x0001D41F, false);
	CHECK(reused == standIn);
	world.refs[reused].inCell = false;  // held back by a quest, saved under its worldspace
	written = world.Save();
	CHECK(World::Has(written, reused));  // the game's own: its record is kept
	CHECK(!world.WritesDoomed(written));
}

TEST("save strays: a forgotten reference is kept out only while its id still names the one we placed")
{
	SaveStrays::Forgotten forgotten;
	forgotten.Add(0xFF000010, 0x0020A578);
	forgotten.Add(0xFF000010, 0x0020A578);  // told twice
	forgotten.Add(0, 0x0020A578);           // no id
	CHECK(forgotten.Size() == 1);
	std::vector<std::uint32_t> kept;
	const auto keep = [&](std::optional<std::uint32_t> a_base) {
		kept.clear();
		forgotten.Keep([&](std::uint32_t) { return a_base; }, [&](std::uint32_t a_id) { kept.push_back(a_id); });
	};
	keep(0x0020A578);
	CHECK(kept == std::vector<std::uint32_t>{ 0xFF000010 });
	keep(0x0001D41F);  // the id went to another reference: dropped, never excluded
	CHECK(kept.empty());
	CHECK(forgotten.Size() == 0);
	forgotten.Add(0xFF000011, 0x0020A578);
	keep(std::nullopt);  // the form is gone
	CHECK(forgotten.Size() == 0);
	// The oldest go past MAX.
	for (std::uint32_t i = 0; i < SaveStrays::Forgotten::MAX + 5; ++i) {
		forgotten.Add(0xFF001000 + i, 0x0020A578);
	}
	CHECK(forgotten.Size() == SaveStrays::Forgotten::MAX);
	keep(0x0020A578);
	CHECK(kept.size() == SaveStrays::Forgotten::MAX);
	CHECK(kept.front() == 0xFF001005);
}
