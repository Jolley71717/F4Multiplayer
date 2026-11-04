#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>

// Copies of an item another game has, made in ours: with its mods (its instance), its count, condition and name, as a
// dropped item's copy is (Drops) and a lent frame's pieces are (FrameLoans). And the other way: an item of ours as the
// wire carries it. Main thread only.
namespace ItemCopies
{
	// The mod is here (a form of that ID, an attachment mod).
	[[nodiscard]] bool ModHere(std::uint32_t a_mod);

	// Whether our game can make that very item: its base and every mod on it are here.
	[[nodiscard]] bool Placeable(const Protocol::DroppedItem& a_item);

	// The extra data our copy is made with: the item's instance (its mods, as Equipment::Give builds a stand-in's), its
	// count, condition and name.
	RE::BSTSmartPointer<RE::ExtraDataList> ExtraFor(const Protocol::DroppedItem& a_item, RE::TESBoundObject* a_object);

	// A stack of ours (its base, and its extra data, none for a plain one) as the wire carries an item: its template
	// combination and mods, its condition and a name the player gave it, a count of 1. None when it has more mods than
	// the wire carries.
	[[nodiscard]] std::optional<Protocol::DroppedItem> ItemOf(const RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra);
}
