#pragma once

#include <cstdint>
#include <string>

// Keeps what only the session made out of the game's saves. A friend's stand-in written into a save
// comes back from it broken (deleted, in no cell, its base form gone), and from then on every save, and
// the engine unloading it, crashes reading the cell it no longer has.
namespace SaveGuard
{
	// Drops the form's record from the save about to be written (from the pre-save message only).
	void Exclude(RE::TESForm* a_form);

	// Before a save: the actors the game made that are in no cell (strays from saves made by earlier
	// versions, or a stand-in caught mid-removal) are kept out of it. Returns how many.
	std::uint32_t BeforeSave();

	// After a load: logs the strays the save brought back, so a broken save can be told apart.
	std::uint32_t AfterLoad();

	// The class name the engine's type information gives the object at a_object, or empty.
	std::string TypeName(const void* a_object);

	std::string Describe();
}
