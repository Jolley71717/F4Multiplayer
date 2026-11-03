// Which settlement a crafting station reads (game/CraftingWorkshop.h): the engine's own answer (the station's link to its
// workbench) always stands; our copy of a friend's station, which is linked to nothing here, reads the workbench its
// report named, once that is a workbench in our game; anything else reads the player's things alone, as vanilla does.

#include "Test.h"

#include "game/CraftingWorkshop.h"

#include <cstdint>

namespace
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;  // a settlement's workbench, the same form in every game
	constexpr std::uint32_t RED_ROCKET = 0x0001F2A1;
	constexpr std::uint32_t CHEM_COPY = 0xFF000A01;  // our copy of a friend's chemistry station
	constexpr std::uint32_t ARMOR_COPY = 0xFF000A02;
	constexpr std::uint32_t OUR_STATION = 0xFF000B01;
}

TEST("crafting workshop: a station linked to its workbench reads that settlement, whatever else is known of it")
{
	CHECK(CraftingWorkshop::Pick(SANCTUARY, 0, false) == SANCTUARY);
	// The engine's link wins over a copy's workbench: a link the game made is the game's (the power audit takes it off).
	CHECK(CraftingWorkshop::Pick(SANCTUARY, RED_ROCKET, true) == SANCTUARY);
}

TEST("crafting workshop: our copy of a friend's station reads the workbench its report named, a workbench here")
{
	CHECK(CraftingWorkshop::Pick(0, SANCTUARY, true) == SANCTUARY);
}

TEST("crafting workshop: a station with no settlement and no copy's workbench reads the player's things alone, as vanilla")
{
	CHECK(CraftingWorkshop::Pick(0, 0, true) == 0);
	CHECK(CraftingWorkshop::Pick(0, 0, false) == 0);
	// A copy whose workbench is no workbench in our game (a friend's mod settlement we lack) reads nothing of a settlement.
	CHECK(CraftingWorkshop::Pick(0, SANCTUARY, false) == 0);
}

TEST("crafting workshop: the copies' workbenches are noted as each copy is kept, and forgotten with it")
{
	CraftingWorkshop::Benches benches;
	CHECK(benches.Of(CHEM_COPY) == 0);
	benches.Note(CHEM_COPY, SANCTUARY);
	benches.Note(ARMOR_COPY, RED_ROCKET);
	CHECK(benches.Of(CHEM_COPY) == SANCTUARY);
	CHECK(benches.Of(ARMOR_COPY) == RED_ROCKET);
	CHECK(benches.Of(OUR_STATION) == 0);  // our own station is no copy: the engine's link is its answer
	CHECK(benches.Size() == 2);

	// Kept again for a newer report (taken over after a load): the newest workbench.
	benches.Note(CHEM_COPY, RED_ROCKET);
	CHECK(benches.Of(CHEM_COPY) == RED_ROCKET);
	CHECK(benches.Size() == 2);

	// A report naming no workbench leaves none behind.
	benches.Note(ARMOR_COPY, 0);
	CHECK(benches.Of(ARMOR_COPY) == 0);
	CHECK(benches.Size() == 1);

	benches.Forget(CHEM_COPY);
	CHECK(benches.Of(CHEM_COPY) == 0);
	benches.Forget(CHEM_COPY);  // twice is nothing
	CHECK(benches.Size() == 0);

	// A reference with no form ID is never noted.
	benches.Note(0, SANCTUARY);
	CHECK(benches.Size() == 0);
}

TEST("crafting workshop: leaving the session, a load or another world forgets every copy's workbench")
{
	CraftingWorkshop::Benches benches;
	benches.Note(CHEM_COPY, SANCTUARY);
	benches.Note(ARMOR_COPY, SANCTUARY);
	benches.Clear();
	CHECK(benches.Size() == 0);
	CHECK(CraftingWorkshop::Pick(0, benches.Of(CHEM_COPY), true) == 0);  // a station whose copy is gone reads as vanilla
}

TEST("crafting workshop: the patched call is the gather's question to Workshop::GetWorkshop in 1.11.240")
{
	// 0x140B28540 + 0x10D is `call 0x140385A80` (WorkshopSync::InstallCrafting checks the bytes before patching).
	CHECK(CraftingWorkshop::GET_WORKSHOP_CALL == 0x10D);
}
