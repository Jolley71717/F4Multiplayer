#include "Test.h"
#include "game/WorkshopApply.h"

// What a reference of ours is known by, and what goes with it when it goes (src/game/WorkshopApply.h): the engine's own
// delete leaves a record already written out to the cell's buffer, which brings the piece back when the cell loads again.

using WorkshopApply::RecordDrop;

TEST("workshop apply: a reference made at runtime goes with its record dropped; a plugin's keeps its record")
{
	CHECK(WorkshopApply::DropsRecord(0xFF0010A9, false));
	CHECK(WorkshopApply::DropsRecord(0xFF000800, false));
	CHECK(!WorkshopApply::DropsRecord(0x00118F54, false));  // a pre-placed crop a friend scrapped: switched off here, a change the save keeps
	CHECK(!WorkshopApply::DropsRecord(0x0A001234, false));
	CHECK(!WorkshopApply::DropsRecord(0xFE001234, false));  // a light plugin's
	CHECK(!WorkshopApply::DropsRecord(0, false));
}

TEST("workshop apply: a reference buried while persistent keeps the engine's own deleted-flag record")
{
	// The engine's delete of a persistent created reference leaves a record with the deleted flag: the load frees it, the
	// save writes it deleted. Dropped, it would have none, and the next change would make a fresh one without the flag.
	CHECK(!WorkshopApply::DropsRecord(0xFF0010A9, true));
	CHECK(!WorkshopApply::DropsRecord(0x00118F54, true));
	CHECK(WorkshopApply::DropOf(0xFF0010A9, true, true, true) == RecordDrop::kPersistent);
	CHECK(WorkshopApply::DropOf(0xFF0010A9, true, false, false) == RecordDrop::kPersistent);
	CHECK(WorkshopApply::DropOf(0x00118F54, true, true, true) == RecordDrop::kNotOurs);  // a plugin's, persistent or not
}

TEST("workshop apply: what dropping the record did")
{
	CHECK(WorkshopApply::DropOf(0x00118F54, false, true, true) == RecordDrop::kNotOurs);
	CHECK(WorkshopApply::DropOf(0xFF0010A9, false, false, false) == RecordDrop::kNone);
	CHECK(WorkshopApply::DropOf(0xFF0010A9, false, true, false) == RecordDrop::kDropped);
	CHECK(WorkshopApply::DropOf(0xFF0010A9, false, true, true) == RecordDrop::kKept);   // ClearChanges left one: the piece may come back
	CHECK(WorkshopApply::DropOf(0xFF0010A9, false, false, true) == RecordDrop::kKept);  // a record made between the two looks: the same
}

TEST("workshop apply: a buried actor's runtime base loses its record with the actor's; a plugin base, or a piece's, keeps it")
{
	// A machinegun turret copy: an actor over a base the engine made for it alone (SaveGuard::HandToLoad does the same for a
	// stand-in's face copy: an actor kept in memory through a load over a base the load freed crashed the load after).
	CHECK(WorkshopApply::DropsBaseRecord(true, 0xFF001A00));
	CHECK(!WorkshopApply::DropsBaseRecord(true, 0x0019A7F2));   // a laser turret: the game's own base
	CHECK(!WorkshopApply::DropsBaseRecord(true, 0x000B3036));   // the original of a templated one, never the runtime base
	CHECK(!WorkshopApply::DropsBaseRecord(false, 0xFF001A00));  // not an actor: a piece never has a runtime base of its own
	CHECK(!WorkshopApply::DropsBaseRecord(true, 0));
}

TEST("workshop apply: a piece is reported under the game's base, a templated turret under its original")
{
	CHECK(WorkshopApply::ReportedBase(0x00030386, 0) == 0x00030386);           // a generator: its own base
	CHECK(WorkshopApply::ReportedBase(0x0019A7F2, 0) == 0x0019A7F2);           // a laser turret keeps its base
	CHECK(WorkshopApply::ReportedBase(0x0019A7F2, 0x000B3036) == 0x0019A7F2);  // its own base wins over any original
	CHECK(WorkshopApply::ReportedBase(0xFF001A00, 0x000B3036) == 0x000B3036);  // a machinegun turret: the engine's runtime base, its original named
	CHECK(WorkshopApply::ReportedBase(0xFF001A00, 0x001167A8) == 0x001167A8);
}

TEST("workshop apply: a runtime base with no original, or a runtime original, is nothing to report")
{
	CHECK(WorkshopApply::ReportedBase(0xFF001A00, 0) == 0);
	CHECK(WorkshopApply::ReportedBase(0xFF001A00, 0xFF001A01) == 0);
	CHECK(WorkshopApply::ReportedBase(0, 0) == 0);
}

TEST("workshop apply: a copy is the same copy under its original base, whatever base the engine gave it here")
{
	// The copy of a templated turret is known by the original (ReportedBase of what the engine made here), so Live finds it
	// again under its id after its cell was unloaded and loaded.
	const auto known = WorkshopApply::ReportedBase(0xFF002B00, 0x000B3036);
	CHECK(WorkshopApply::SameCopy(0xFF001000, known, 0x000B3036, false));
	CHECK(!WorkshopApply::SameCopy(0xFF001000, known, 0x000B3036, true));
}
