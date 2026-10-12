#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Keeps what only the session made out of the game's saves. A friend's stand-in written into a save
// comes back from it broken (deleted, in no cell, its base form gone), and from then on every save, and
// the engine unloading it, crashes reading the cell it no longer has.
namespace SaveGuard
{
	// Drops the form's record from the game's list of changes, so the next save does not write it. Called
	// before a save, and before a load (DespawnAll runs at both; before a load it does no harm, the load
	// replaces the list). Whether there was a record and it is gone now.
	bool Exclude(RE::TESForm* a_form);

	// A stand-in was placed: one of these left in no cell is ours to keep out of saves (see BeforeSave).
	void Track(RE::Actor* a_actor);

	// Before a save: the created actors in no cell that are gone (deleted stand-ins, or actors whose base
	// no longer exists, such as a stand-in an earlier version saved) are kept out of it. Searched for only
	// once this run placed a stand-in or the last load brought strays back. Returns how many were kept out.
	std::uint32_t BeforeSave();

	// After a load: logs the strays the save brought back, so a broken save can be told apart.
	std::uint32_t AfterLoad();

	// How many strays there are now (counts nothing, changes nothing).
	std::uint32_t CountStrays();

	// The class name the engine's type information gives the object at a_object, or empty.
	std::string TypeName(const void* a_object);

	// The pointer held by the game's global at a_offset from the module, or nothing if it can't be read.
	std::optional<const void*> ReadGlobal(std::uintptr_t a_offset);

	std::string Describe();
}
