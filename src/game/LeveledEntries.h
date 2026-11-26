#pragma once

#include <cstdint>

// The walk over a leveled list's entries (TESLeveledList: baseListCount / scriptListCount, each a uint8 count). The index is
// wider than the count, so a list of 128 entries or more is walked to its end: an int8 index read such a count as negative
// (before CommonLibF4 typed it uint8) or never reached it. Game-free: tests/LeveledEntriesTests.cpp.
namespace LeveledEntries
{
	// Calls a_visit on each of a_count elements of a_entries (nothing without an array).
	template <class T, class Visit>
	void Each(T* a_entries, std::uint32_t a_count, Visit&& a_visit)
	{
		if (!a_entries) {
			return;
		}
		for (std::uint32_t i = 0; i < a_count; ++i) {
			a_visit(a_entries[i]);
		}
	}
}
