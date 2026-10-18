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

	// Whether the engine's save code finds a cell or a worldspace to save the reference under (see SaveStrays::Doomed).
	bool Anchored(RE::TESObjectREFR* a_ref);

	// Exclude, for a reference of ours as it is marked for deletion, or found on its way out with no cell: its record
	// is dropped now, not at the next save, as the engine unloading a cell writes out the records of what was in it,
	// and one with neither a cell nor a worldspace crashes it. Counted (excludedAtDelete).
	bool ExcludeOurs(RE::TESObjectREFR* a_ref);

	// A stand-in, frame copy or light was placed: one of these left in no cell is ours to keep out of saves (see
	// BeforeSave).
	void Track(RE::TESObjectREFR* a_ref);

	// Whether the reference is one of those Track was told of this run (a stand-in, a frame copy, a light, a copy of a
	// friend's runtime NPC or frame): the session's, not the game's own.
	[[nodiscard]] bool Placed(const RE::TESObjectREFR* a_ref);

	// A stand-in was placed: the engine is not let make it persistent (or let go of it so) from now on, so it is never kept
	// in memory past its cell and unloaded later with no cell (see SaveStrays::Unpromoted). Until its form is gone or a load.
	void KeepUnpromoted(RE::Actor* a_actor);

	// A reference of ours (Track) whose handle stopped resolving: the game took it from under us, and its form may
	// still be there. Kept out of saves, and its record dropped once a second (Frame), for as long as it is.
	void Forget(std::uint32_t a_id);

	// Once a frame.
	void Frame();

	// Before a save: the created references that are strays (SaveStrays::Stray) are kept out of it (SaveStrays::Judge):
	// ours with neither a cell nor a worldspace, and deleted ones in no cell that are ours or whose base no longer exists.
	// One of the game's with neither, its base still there, is only logged (its record is the game's, as without the
	// mod). Every save is searched. Returns how many were kept out.
	std::uint32_t BeforeSave();

	// After a load: the strays the save brought back are logged, and the ones that are ours dropped from it.
	std::uint32_t AfterLoad();

	// A new game: what was placed before it is gone, and its ids name the new game's references (as after a load).
	void NewGame();

	// The strays there are now, one after another, with what a save and a load would do with each (changes nothing).
	std::string ListStrays();

	// The class name the engine's type information gives the object at a_object, or empty.
	std::string TypeName(const void* a_object);

	// The pointer held by the game's global at a_offset from the module, or nothing if it can't be read.
	std::optional<const void*> ReadGlobal(std::uintptr_t a_offset);

	std::string Describe();
}
