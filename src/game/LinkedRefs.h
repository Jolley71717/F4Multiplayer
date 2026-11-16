#pragma once

#include <cstdint>
#include <optional>

// Where a linked-ref entry (ExtraLinkedRef) points: a terminal's door or safe, a container's trigger, what a switch
// drives, the workbench a piece belongs to, the piece another stands on. The field CommonLibF4 calls the entry's
// pointer holds the target's form ID instead (zero-extended to 64 bits) when the engine keys the entry by ID (1.11.240,
// read from the disassembly: the entry's getter 2191128, the setter 2191122, the save's writer 2191124 and reader
// 2191125): every entry of a list marked transient (the byte after the array, set by 2190308), and an entry under one
// of the two keywords the engine holds at 4797181 (the workshop's stacked-piece link) and 4797170, whatever the list. An entry the engine is still
// reading from a file holds an ID too. Reading the field as a pointer crashed both games at the Red Rocket terminal
// (0.7.1, 2026-10-10) and the host's in a wsinfo scan of the statics around Sanctuary (2026-11-16, a piece's link
// holding 000AE0F2). The value tells the two apart for every entry the engine writes, so it is all that is asked.
// A pointer the entry does hold is believed only if the form it names by its own ID is that very form: one left behind
// by a target that unloaded or went names another form or none, and its memory may not be readable at all (the reader
// is guarded for that). Kept free of the game so it can be tested; LinkedTargets.h is the game's side.
namespace LinkedRefs
{
	// Whether the field holds a form ID rather than an address: nothing of the game's lies in the first 4 GB.
	constexpr bool HoldsId(std::uintptr_t a_raw)
	{
		return a_raw != 0 && a_raw <= 0xFFFFFFFFull;
	}

	// The reference the field names, or nullptr. a_readId reads the ID a pointer's form says it has (nullopt where its
	// memory can't be read); a_lookup finds a reference by ID (nullptr: none).
	template <class Ref, class ReadId, class Lookup>
	Ref* Resolve(std::uintptr_t a_raw, ReadId&& a_readId, Lookup&& a_lookup)
	{
		if (a_raw == 0) {
			return nullptr;
		}
		if (HoldsId(a_raw)) {
			return a_lookup(static_cast<std::uint32_t>(a_raw));
		}
		const auto                         pointer = reinterpret_cast<Ref*>(a_raw);
		const std::optional<std::uint32_t> id = a_readId(pointer);
		if (!id || *id == 0) {
			return nullptr;
		}
		Ref* const found = a_lookup(*id);
		return found == pointer ? found : nullptr;
	}

	// The form ID the field names, or 0: an ID held there as it is (its reference need not be in memory: a log still
	// says what the entry names), a pointer's only if it resolves (Resolve), so a stale one says 0, never another form's.
	template <class Ref, class ReadId, class Lookup>
	std::uint32_t TargetId(std::uintptr_t a_raw, ReadId&& a_readId, Lookup&& a_lookup)
	{
		if (HoldsId(a_raw)) {
			return static_cast<std::uint32_t>(a_raw);
		}
		Ref* const found = Resolve<Ref>(a_raw, a_readId, a_lookup);
		if (!found) {
			return 0;
		}
		const std::optional<std::uint32_t> id = a_readId(found);
		return id ? *id : 0;
	}
}
