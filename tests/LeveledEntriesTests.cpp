#include "Test.h"
#include "game/LeveledEntries.h"

#include <cstdint>
#include <vector>

// The walk over a leveled list's entries: a count is a uint8 (0-255), so a list of 128 or more entries is walked whole.
namespace
{
	std::uint32_t Visits(std::uint8_t a_count)
	{
		std::vector<int> entries(a_count, 7);
		std::uint32_t    visited = 0;
		LeveledEntries::Each(entries.data(), a_count, [&](int) { ++visited; });
		return visited;
	}
}

TEST("leveled entries: a list of 200 entries is walked 200 times, not skipped as negative")
{
	CHECK(Visits(200) == 200);
}

TEST("leveled entries: the counts either side of 128 and the largest one are walked whole")
{
	CHECK(Visits(0) == 0);
	CHECK(Visits(1) == 1);
	CHECK(Visits(127) == 127);
	CHECK(Visits(128) == 128);
	CHECK(Visits(255) == 255);
}

TEST("leveled entries: no array is no visit, and each entry is seen once in order")
{
	std::uint32_t visited = 0;
	LeveledEntries::Each(static_cast<int*>(nullptr), 200, [&](int) { ++visited; });
	CHECK(visited == 0);

	std::vector<int> entries(255);
	for (std::size_t i = 0; i < entries.size(); ++i) {
		entries[i] = static_cast<int>(i);
	}
	int next = 0;
	bool inOrder = true;
	LeveledEntries::Each(entries.data(), 255, [&](int a_entry) { inOrder = inOrder && a_entry == next++; });
	CHECK(inOrder);
	CHECK(next == 255);
}

TEST("leveled entries: a script-added list's pointers are passed as they are, null ones included")
{
	int  a = 1;
	int* lists[3] = { &a, nullptr, &a };
	int  seen = 0, nulls = 0;
	LeveledEntries::Each(lists, 3, [&](int* a_entry) { a_entry ? ++seen : ++nulls; });
	CHECK(seen == 2);
	CHECK(nulls == 1);
}
