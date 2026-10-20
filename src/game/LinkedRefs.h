#pragma once

#include <cstdint>
#include <optional>

// Where a linked-ref entry (ExtraLinkedRef) points: a terminal's door or safe, a container's trigger, what a switch
// drives. WorldSync watches what the player uses and what it is linked to. The engine fills an entry with the
// target's form ID as it reads the reference from its file and puts the pointer in later; until then the pointer
// field holds the ID, and reading it as a pointer crashed both games at the Red Rocket terminal (0.7.1, 2026-10-10).
// A pointer the entry does hold is believed only if the form it names by its own ID is that very form: one left
// behind by a target that unloaded or went names another form or none, and its memory may not be readable at all
// (the reader is guarded for that). Kept free of the game so it can be tested.
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
}
