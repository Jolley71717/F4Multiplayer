#include "Test.h"
#include "game/SaveStrays.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

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

TEST("save strays: a load forgets what was placed before it, and remembers there were some")
{
	SaveStrays::Placed placed;
	placed.Add(0xFF000010, 0x0020A578);
	placed.Add(0xFF000011, 0xFF000F00);
	placed.Erase(0xFF000010);  // its form went: the id may be handed out again
	CHECK(!placed.Ours(0xFF000010, 0x0020A578));
	CHECK(placed.Ours(0xFF000011, 0xFF000F00));
	placed.Clear();
	CHECK(placed.Size() == 0);
	CHECK(!placed.Ours(0xFF000011, 0xFF000F00));
	CHECK(placed.Any());
}

TEST("save strays: a copy the player took is let go of at once, so the item of its base they drop next under its id is the game's")
{
	SaveStrays::Book book;
	book.Track(0xFF000F63, 0x0004835D);  // a copy of a Nuka-Cola a friend dropped
	CHECK(book.IsPlaced(0xFF000F63, 0x0004835D));
	book.Release(0xFF000F63);  // taken into the player's inventory: the engine deletes the reference
	// The game hands the id to the Nuka-Cola the player drops next: theirs, to be given, not ours.
	CHECK(!book.IsPlaced(0xFF000F63, 0x0004835D));
	CHECK(book.PlacedCount() == 0);
	CHECK(book.ForgottenCount() == 0);  // nor kept out of saves
	CHECK(book.Session());
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

TEST("save strays: a save keeps every stray of ours out and one whose base is gone; the game's own with no cell or worldspace keeps its record; right after a load only ours are dropped")
{
	using SaveStrays::Judge;
	using SaveStrays::Verdict;
	using SaveStrays::When;
	const SaveStrays::Actor doomedOurs{ .inCell = false, .hasBase = true, .ours = true, .anchored = false };
	const SaveStrays::Actor doomedGames{ .inCell = false, .hasBase = true, .ours = false, .anchored = false };
	const SaveStrays::Actor doomedBaseGone{ .inCell = false, .hasBase = false, .ours = false, .anchored = false };
	const SaveStrays::Actor baseGone{ .inCell = false, .hasBase = false, .deleted = true };
	const SaveStrays::Actor heldBack{ .inCell = false, .hasBase = true, .deleted = false };
	CHECK(Judge(doomedOurs, When::kSave) == Verdict::kExclude);
	CHECK(Judge(doomedGames, When::kSave) == Verdict::kTell);  // a quest's or the player's own: as it is without the mod
	CHECK(Judge(doomedBaseGone, When::kSave) == Verdict::kExclude);  // it could only come back broken
	CHECK(Judge(baseGone, When::kSave) == Verdict::kExclude);
	CHECK(Judge(heldBack, When::kSave) == Verdict::kLeave);
	CHECK(Judge(doomedOurs, When::kLoad) == Verdict::kExclude);
	CHECK(Judge(doomedGames, When::kLoad) == Verdict::kTell);  // its cell may be on its way as the load settles
	CHECK(Judge(baseGone, When::kLoad) == Verdict::kTell);
	CHECK(Judge(heldBack, When::kLoad) == Verdict::kLeave);
}

namespace
{
	constexpr std::uint32_t FACE_COPY = 0xFF0009A8;   // a stand-in's base, made at runtime: no save keeps it
	constexpr std::uint32_t FRAME = 0x0017F6E4;       // a power armor frame every game has: the game's own suits have it too
	constexpr std::uint32_t LIGHT = 0x0001ED2D;       // the friends' Pip-Boy light
	constexpr std::uint32_t SETTLER = 0x0002F3B8;

	// A game's created references as the engine keeps them, and what its save does with them: every reference with a
	// record of changes is written, under its cell, else its worldspace, and one with neither is the crash (10-11).
	// SaveGuard's part is the real SaveStrays::Book, called as SaveGuard calls it, with what a sweep reads.
	struct Engine
	{
		struct Ref
		{
			std::uint32_t base = 0;
			bool          baseExists = true;
			bool          actor = false;
			bool          inCell = true;
			bool          worldspace = true;
			bool          deleted = false;
			bool          record = true;
			bool          standInMark = false;  // carries the stand-ins' faction change
		};
		std::map<std::uint32_t, Ref> refs;
		std::uint32_t                nextId = 0xFF000EB0;
		SaveStrays::Book             book;

		struct Saved
		{
			std::vector<std::uint32_t> written;
			std::vector<std::uint32_t> told;  // logged, left as they were
			bool                       crashed = false;
		};

		std::uint32_t Place(Ref a_ref)
		{
			const auto id = nextId++;
			refs[id] = a_ref;
			return id;
		}

		// RemotePlayers places one and tells SaveGuard (Track).
		std::uint32_t PlaceOurs(std::uint32_t a_base, bool a_actor)
		{
			const auto id = Place({ .base = a_base, .actor = a_actor, .standInMark = a_actor });
			book.Track(id, a_base);
			return id;
		}

		// What SaveGuard::Sweep reads: the created references in no cell.
		std::vector<SaveStrays::Found> Sweep() const
		{
			std::vector<SaveStrays::Found> found;
			for (const auto& [id, ref] : refs) {
				if (!ref.inCell) {
					found.push_back({ .id = id, .base = ref.base, .actor = ref.actor, .seen = { .inCell = false, .hasBase = ref.baseExists, .deleted = ref.deleted, .anchored = ref.worldspace } });
				}
			}
			return found;
		}

		auto StandIn()
		{
			return [this](std::uint32_t a_id, std::uint32_t) { return refs.at(a_id).standInMark; };
		}

		auto BaseNow()
		{
			return [this](std::uint32_t a_id) -> std::optional<std::uint32_t> {
				const auto it = refs.find(a_id);
				return it != refs.end() && it->second.baseExists ? std::optional{ it->second.base } : std::nullopt;
			};
		}

		Saved Save()
		{
			Saved saved;
			for (const auto& [stray, verdict] : book.BeforeSave(BaseNow(), [&](std::uint32_t a_id) { refs[a_id].record = false; }, Sweep(), StandIn())) {
				if (verdict == SaveStrays::Verdict::kExclude) {
					refs[stray.id].record = false;
				} else if (verdict == SaveStrays::Verdict::kTell) {
					saved.told.push_back(stray.id);
				}
			}
			for (const auto& [id, ref] : refs) {
				if (ref.record) {
					saved.written.push_back(id);
					saved.crashed |= !ref.inCell && !ref.worldspace;
				}
			}
			return saved;
		}

		// A load: the save's references are every created one now, and the game counts on from the save's last id.
		void Load(std::map<std::uint32_t, Ref> a_saved, std::uint32_t a_nextId)
		{
			refs = std::move(a_saved);
			nextId = a_nextId;
			for (const auto& [stray, verdict] : book.AfterLoad(Sweep(), StandIn())) {
				if (verdict == SaveStrays::Verdict::kExclude) {
					refs[stray.id].record = false;
				}
			}
		}

		bool Written(std::uint32_t a_id) const
		{
			const auto it = refs.find(a_id);
			return it != refs.end() && it->second.record;
		}
	};

	bool Has(const std::vector<std::uint32_t>& a_ids, std::uint32_t a_id)
	{
		return std::ranges::find(a_ids, a_id) != a_ids.end();
	}
}

TEST("save strays: a session's stand-in, frame copy and light the cells were cleared under stay out of the save, which is written; the game's own keep their records")
{
	Engine engine;
	const auto standIn = engine.PlaceOurs(FACE_COPY, true);
	const auto frame = engine.PlaceOurs(FRAME, false);
	const auto light = engine.PlaceOurs(LIGHT, false);
	const auto settler = engine.Place({ .base = SETTLER, .actor = true });
	const auto heldBack = engine.Place({ .base = SETTLER, .actor = true, .inCell = false });          // a quest holds it, under its worldspace
	const auto droppedSuit = engine.Place({ .base = FRAME, .inCell = false, .deleted = true });       // the game's, deleted: its record keeps its id taken
	// The host follows a friend (coc), and the cells are cleared before ours could be taken out: left with neither a
	// cell nor a worldspace, alive and not deleted.
	for (const auto id : { standIn, frame, light }) {
		engine.refs[id].inCell = false;
		engine.refs[id].worldspace = false;
	}
	auto saved = engine.Save();
	CHECK(!saved.crashed);
	CHECK(!Has(saved.written, standIn));
	CHECK(!Has(saved.written, frame));
	CHECK(!Has(saved.written, light));
	CHECK(Has(saved.written, settler));
	CHECK(Has(saved.written, heldBack));
	CHECK(Has(saved.written, droppedSuit));
	CHECK(saved.told.empty());

	// The game takes the light from under us (its handle stops resolving) while it is back in a cell, and a change
	// makes it a record again: no sweep sees it as a stray, but it was ours, and stays out.
	engine.refs[light] = { .base = LIGHT };
	engine.book.Forget(light);
	CHECK(!Has(engine.Save().written, light));

	// Then its form goes, and the game hands its id to a light of its own, in no cell and deleted, a record kept on
	// purpose: it is the game's, and saved.
	engine.refs.erase(light);
	engine.Save();
	engine.refs[light] = { .base = LIGHT, .inCell = false, .deleted = true };
	saved = engine.Save();
	CHECK(Has(saved.written, light));
	CHECK(!saved.crashed);
}

TEST("save strays: after a load, an id a frame copy or light had before it names the loaded game's own reference, whose record stays, though it has the same base")
{
	Engine engine;
	const auto frame = engine.PlaceOurs(FRAME, false);
	const auto light = engine.PlaceOurs(LIGHT, false);
	const auto standIn = engine.PlaceOurs(FACE_COPY, true);
	engine.book.Forget(light);  // taken from under us, its form still there
	CHECK(engine.book.PlacedCount() == 3);

	// An older save is loaded. In it, the frame copy's id is a power armor suit of the game's, deleted and in no cell,
	// with the very frame base; the light's id is a light of the game's in a cell; the stand-in's id is a stand-in an
	// earlier version saved, on a face copy no save keeps; and a stand-in placed from a settler base carries the
	// stand-ins' faction change.
	std::map<std::uint32_t, Engine::Ref> older;
	older[frame] = { .base = FRAME, .inCell = false, .deleted = true };
	older[light] = { .base = LIGHT };
	older[standIn] = { .base = FACE_COPY, .baseExists = false, .actor = true, .inCell = false, .deleted = true };
	const std::uint32_t marked = 0xFF000E00;
	older[marked] = { .base = SETTLER, .actor = true, .inCell = false, .deleted = true, .standInMark = true };
	engine.Load(older, 0xFF000EC0);
	CHECK(engine.book.PlacedCount() == 0);
	CHECK(engine.book.ForgottenCount() == 0);
	CHECK(engine.Written(frame));    // the game's suit: not ours, deleted on purpose
	CHECK(!engine.Written(marked));  // a stand-in all the same: dropped as the load brings it back

	const auto saved = engine.Save();
	CHECK(Has(saved.written, frame));
	CHECK(Has(saved.written, light));     // no longer forgotten: the id is the game's light now
	CHECK(!Has(saved.written, standIn));  // its base is gone: it could only come back broken
	CHECK(!saved.crashed);
	CHECK(engine.book.Session());  // the load does not change that this run had a session
}

TEST("save strays: the game's own reference with neither a cell nor a worldspace keeps its record in every save of a session: alone, beside a friend's stand-in the cells were cleared under, after a reload, and after the friend leaves")
{
	Engine engine;
	// A quest's or a mod's reference, held in no cell: the game has it so with or without the mod. Whether the
	// engine's save gets through with it is the game's business (the crash is known only for ours), so it is only
	// logged, and its record is written as without the mod.
	const auto games = engine.Place({ .base = 0x0020A578, .inCell = false, .worldspace = false });
	const auto onlyGames = std::vector<std::uint32_t>{ games };
	auto saved = engine.Save();
	CHECK(saved.told == onlyGames);
	CHECK(Has(saved.written, games));

	// A friend joins: their stand-in, frame copy and light are placed. The host follows them (coc), and the cells are
	// cleared under ours before they could be taken out.
	const std::vector<std::uint32_t> ours{ engine.PlaceOurs(FACE_COPY, true), engine.PlaceOurs(FRAME, false), engine.PlaceOurs(LIGHT, false) };
	for (const auto id : ours) {
		engine.refs[id].inCell = false;
		engine.refs[id].worldspace = false;
	}
	saved = engine.Save();
	CHECK(engine.book.Session());
	for (const auto id : ours) {
		CHECK(!Has(saved.written, id));
	}
	CHECK(Has(saved.written, games));  // a session is no reason to drop the game's own
	CHECK(saved.told == onlyGames);

	// The host reloads that save: ours are not in it, and the game's comes back as it was saved.
	std::map<std::uint32_t, Engine::Ref> fromSave;
	for (const auto id : saved.written) {
		fromSave[id] = engine.refs.at(id);
	}
	engine.Load(fromSave, engine.nextId);
	CHECK(engine.Written(games));  // after a load it is logged, never dropped
	CHECK(engine.book.PlacedCount() == 0);

	// The friend's stand-in is placed again, then they leave: the game takes it from under us, and its form goes.
	const auto again = engine.PlaceOurs(FACE_COPY, true);
	engine.book.Forget(again);
	engine.refs.erase(again);
	saved = engine.Save();
	CHECK(Has(saved.written, games));
	CHECK(!Has(saved.written, again));
	CHECK(saved.told == onlyGames);
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

TEST("save strays: the engine is told no only for the stand-in at that address, under that id, with that base")
{
	SaveStrays::Unpromoted unpromoted;
	int        standIn = 0;
	int        other = 0;
	const auto at = static_cast<const void*>(&standIn);
	unpromoted.Add(at, 0xFF000EB8, FACE_COPY);
	unpromoted.Add(nullptr, 0xFF000EB9, FACE_COPY);  // nothing there
	unpromoted.Add(&other, 0, FACE_COPY);            // no id
	CHECK(unpromoted.Size() == 1);
	CHECK(unpromoted.Refuses(at, 0xFF000EB8, FACE_COPY));
	// The game hands addresses and ids out again: an actor of its own under the old address and id has another base.
	CHECK(!unpromoted.Refuses(at, 0xFF000EB8, SETTLER));
	CHECK(!unpromoted.Refuses(at, 0xFF000EC0, FACE_COPY));
	CHECK(!unpromoted.Refuses(&other, 0xFF000EB8, FACE_COPY));
	CHECK(unpromoted.Refused() == 1);

	// A new stand-in under the same id, or at the same address, takes the old one's place.
	unpromoted.Add(&other, 0xFF000EB8, FACE_COPY);
	CHECK(unpromoted.Size() == 1);
	CHECK(!unpromoted.Refuses(at, 0xFF000EB8, FACE_COPY));
	CHECK(unpromoted.Refuses(&other, 0xFF000EB8, FACE_COPY));
	unpromoted.Add(&other, 0xFF000EC0, SETTLER);
	CHECK(unpromoted.Size() == 1);
	CHECK(unpromoted.Refuses(&other, 0xFF000EC0, SETTLER));

	// Let go of by the id and the base it was placed from: an entry a newer stand-in made under the id stays.
	unpromoted.Remove(0xFF000EC0, FACE_COPY);
	CHECK(unpromoted.Size() == 1);
	unpromoted.Remove(0xFF000EC0, SETTLER);
	CHECK(unpromoted.Size() == 0);
	CHECK(!unpromoted.Refuses(&other, 0xFF000EC0, SETTLER));

	// The oldest go past MAX, and a load forgets them all; what was refused stays counted.
	std::vector<int> many(SaveStrays::Unpromoted::MAX + 5);
	for (std::uint32_t i = 0; i < many.size(); ++i) {
		unpromoted.Add(&many[i], 0xFF001000 + i, FACE_COPY);
	}
	CHECK(unpromoted.Size() == SaveStrays::Unpromoted::MAX);
	CHECK(!unpromoted.Refuses(&many[4], 0xFF001004, FACE_COPY));
	CHECK(unpromoted.Refuses(&many[5], 0xFF001005, FACE_COPY));
	unpromoted.Clear();
	CHECK(unpromoted.Size() == 0);
	CHECK(unpromoted.Refused() == 4);
}

TEST("save strays: the engine may ask from any thread while stand-ins come and go")
{
	SaveStrays::Unpromoted unpromoted;
	std::vector<int>       actors(64);
	std::atomic<bool>      stop{ false };
	std::atomic<int>       wrong{ 0 };
	unpromoted.Add(&actors[0], 0xFF000EB8, FACE_COPY);
	std::vector<std::thread> askers;
	for (int t = 0; t < 4; ++t) {
		askers.emplace_back([&] {
			while (!stop) {
				wrong += !unpromoted.Refuses(&actors[0], 0xFF000EB8, FACE_COPY);  // never taken away
				wrong += unpromoted.Refuses(&actors[1], 0xFF000EB8, SETTLER);    // never ours
			}
		});
	}
	for (std::uint32_t round = 0; round < 2000; ++round) {
		const auto i = 2 + round % 60;
		unpromoted.Add(&actors[i], 0xFF002000 + i, FACE_COPY);
		unpromoted.Remove(0xFF002000 + i, FACE_COPY);
	}
	stop = true;
	for (auto& asker : askers) {
		asker.join();
	}
	CHECK(wrong == 0);
	CHECK(unpromoted.Size() == 1);
}

namespace
{
	// The engine's dynamic persistence as it is read from its code (1.11.240), around the Engine above. Something of the
	// game's that holds a reference promotes it (BGSDynamicPersistenceManager::PromoteReference, with itself as the
	// owner), which keeps it in memory past its cell. Letting go of an actor waits for the end of the frame
	// (DemoteQueuedActors); the last owner gone, one whose cell is not loaded is unloaded (BGSSaveLoadGame::UnloadForm):
	// a record of changes is written out under its cell or else its worldspace, and with neither the game crashes. Both
	// ask the reference first whether it may be promoted, and do nothing more on a no: SaveGuard's hook, answered by
	// the real SaveStrays::Unpromoted. Cells cleared under the player (a door, a fast travel, walking off) unload what
	// is in them, but leave a promoted reference in memory in no cell.
	struct Persistence
	{
		Engine                                               engine;
		SaveStrays::Unpromoted                               unpromoted;
		std::map<std::uint32_t, int>                         address;  // where each reference is in memory
		std::vector<int>                                     memory = std::vector<int>(16);
		std::map<std::uint32_t, std::vector<std::uint32_t>>  owners;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> queued;  // actor, owner
		bool                                                 crashed = false;
		std::vector<std::uint32_t>                           unloaded;

		const void* At(std::uint32_t a_id) { return &memory[address.at(a_id)]; }

		// The engine asking (TESObjectREFR::GetAllowPromoteToPersistent, hooked).
		bool Allowed(std::uint32_t a_id) { return !unpromoted.Refuses(At(a_id), a_id, engine.refs.at(a_id).base); }

		std::uint32_t Place(Engine::Ref a_ref, int a_address)
		{
			const auto id = engine.Place(a_ref);
			address[id] = a_address;
			return id;
		}

		// RemotePlayers::Spawn: Track, then KeepUnpromoted.
		std::uint32_t PlaceStandIn(int a_address, bool a_keepUnpromoted)
		{
			const auto id = engine.PlaceOurs(FACE_COPY, true);
			address[id] = a_address;
			if (a_keepUnpromoted) {
				unpromoted.Add(At(id), id, FACE_COPY);
			}
			return id;
		}

		void Promote(std::uint32_t a_id, std::uint32_t a_owner)
		{
			if (Allowed(a_id)) {
				owners[a_id].push_back(a_owner);
			}
		}

		void Demote(std::uint32_t a_id, std::uint32_t a_owner)
		{
			if (engine.refs.contains(a_id) && Allowed(a_id)) {  // a holder whose handle no longer resolves lets go of nothing
				queued.emplace_back(a_id, a_owner);  // an actor's waits for the end of the frame
			}
		}

		void Unload(std::uint32_t a_id)
		{
			const auto& ref = engine.refs.at(a_id);
			crashed |= ref.record && !ref.inCell && !ref.worldspace;
			unloaded.push_back(a_id);
			engine.refs.erase(a_id);
			owners.erase(a_id);
		}

		// The player leaves the cells: what is not promoted goes with them (its record kept under its cell), what is
		// promoted stays in memory with neither a cell nor a worldspace.
		void ClearCells(const std::vector<std::uint32_t>& a_ids)
		{
			for (const auto id : a_ids) {
				if (owners.contains(id) && !owners[id].empty()) {
					engine.refs[id].inCell = false;
					engine.refs[id].worldspace = false;
				} else {
					Unload(id);
				}
			}
		}

		// The end of the frame (Main::OnIdle): DemoteQueuedActors.
		void EndOfFrame()
		{
			for (const auto& [id, owner] : std::exchange(queued, {})) {
				if (!engine.refs.contains(id) || !Allowed(id)) {
					continue;
				}
				std::erase(owners[id], owner);
				if (owners[id].empty() && !engine.refs[id].inCell) {
					Unload(id);
				}
			}
		}

		// A frame of ours: RemotePlayers drops the record of one of ours in no cell (SaveGuard::ExcludeOurs), and lets go
		// of the ones the game took (SaveGuard::Forget, then Frame: KeepForgotten, LetGoOfGone).
		void OurFrame(const std::vector<std::uint32_t>& a_ours)
		{
			for (const auto id : a_ours) {
				if (!engine.refs.contains(id)) {
					engine.book.Forget(id);
				} else if (!engine.refs[id].inCell) {
					engine.refs[id].record = false;
				}
			}
			engine.book.KeepForgotten(engine.BaseNow(), [&](std::uint32_t a_id) { engine.refs[a_id].record = false; });
			for (const auto& [id, base] : engine.book.TakeGone()) {
				unpromoted.Remove(id, base);
			}
		}
	};

	constexpr std::uint32_t QUEST = 0x000229E4;  // a quest of the settlement's, filling an alias with who stands there
}

TEST("save strays: leaving Red Rocket, a quest letting go of the friend's stand-in it held crashed the game in the same frame; told no, the stand-in goes with its cell")
{
	// Before: the quest promotes the stand-in, the cells are cleared as the host leaves, and the quest, stopped with the
	// location left, lets go of it in that same update. The end of the frame unloads it in no cell with its record: the
	// crash, before any frame of ours could drop the record.
	{
		Persistence game;
		const auto  standIn = game.PlaceStandIn(0, false);
		game.Promote(standIn, QUEST);
		game.ClearCells({ standIn });
		game.Demote(standIn, QUEST);
		game.EndOfFrame();
		CHECK(game.crashed);
	}
	// Now: the engine is told no, so the stand-in is never promoted, goes with its cell (its record kept under it, as
	// for any reference the game placed), and the quest letting go of it does nothing. The settlement's own settler
	// the quest holds is promoted and let go of as before.
	Persistence game;
	const auto  standIn = game.PlaceStandIn(0, true);
	const auto  settler = game.Place({ .base = SETTLER, .actor = true }, 1);
	game.Promote(standIn, QUEST);
	game.Promote(settler, QUEST);
	CHECK(game.owners[standIn].empty());
	CHECK(game.owners[settler].size() == 1);
	game.ClearCells({ standIn });
	CHECK(game.unloaded == std::vector<std::uint32_t>{ standIn });
	game.Demote(standIn, QUEST);
	game.Demote(settler, QUEST);
	game.EndOfFrame();
	CHECK(!game.crashed);
	CHECK(game.owners[settler].empty());
	CHECK(game.engine.refs.contains(settler));
	CHECK(game.unpromoted.Refused() == 1);  // the promotion; the demotion found it gone already

	// Our frame finds the stand-in gone, and once its form is, it is no longer ours to refuse: an actor of the game's
	// placed after it under the same address and id (the game hands both out again) is promoted and let go of.
	game.OurFrame({ standIn });
	CHECK(game.unpromoted.Size() == 0);
	game.engine.nextId = standIn;
	const auto newcomer = game.Place({ .base = FACE_COPY, .actor = true }, 0);
	CHECK(newcomer == standIn);
	game.Promote(newcomer, QUEST);
	CHECK(game.owners[newcomer].size() == 1);
	game.Demote(newcomer, QUEST);
	game.EndOfFrame();
	CHECK(game.owners[newcomer].empty());
	CHECK(!game.crashed);
}

TEST("save strays: a stand-in promoted before it was told no is safe only with its record dropped before it is let go of, and once told no it stays out of every save")
{
	// A frame of ours between the cells clearing and the holder letting go drops the record, and the unload finds
	// nothing to write.
	Persistence game;
	const auto  standIn = game.PlaceStandIn(0, false);
	game.Promote(standIn, QUEST);
	game.ClearCells({ standIn });
	game.OurFrame({ standIn });
	game.Demote(standIn, QUEST);
	game.EndOfFrame();
	CHECK(!game.crashed);

	// One an earlier version saved promoted (SaveGuard::AfterLoad drops its record, then tells the engine no): never
	// let go of, it stays in memory in no cell, and a record a later change makes is kept out of the save.
	Persistence kept;
	const auto  loaded = kept.PlaceStandIn(0, false);
	kept.Promote(loaded, QUEST);
	kept.ClearCells({ loaded });
	kept.engine.refs[loaded].record = false;
	kept.unpromoted.Add(kept.At(loaded), loaded, FACE_COPY);
	kept.Demote(loaded, QUEST);
	kept.EndOfFrame();
	CHECK(!kept.crashed);
	CHECK(kept.engine.refs.contains(loaded));
	kept.engine.refs[loaded].record = true;
	const auto saved = kept.engine.Save();
	CHECK(!saved.crashed);
	CHECK(!Has(saved.written, loaded));
}
