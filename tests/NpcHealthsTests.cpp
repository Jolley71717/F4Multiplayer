#include "Test.h"

#include "NpcHealths.h"

#include <cmath>
#include <limits>
#include <map>

// The server's book of the health of NPCs hurt in the session, for a game that joins or loads later (common/NpcHealths.h).
namespace
{
	std::map<std::uint32_t, float> All(const NpcHealths::Book& a_book)
	{
		std::map<std::uint32_t, float> out;
		a_book.ForEach([&](std::uint32_t a_ref, float a_fraction) { out[a_ref] = a_fraction; });
		return out;
	}
}

TEST("npc healths: only an NPC hurt here is kept; healed back to full it stays, told as whole")
{
	NpcHealths::Book book;
	book.Heard(1, 1.0f);  // a first reading at full health: every NPC a game comes to run
	CHECK(book.Size() == 0);
	book.Heard(2, 0.6f);
	book.Heard(2, 0.4f);
	CHECK((All(book) == std::map<std::uint32_t, float>{ { 2, 0.4f } }));
	// Its joiner's older save may have it hurt: full health is told too, once it was hurt.
	book.Heard(2, 1.0f);
	CHECK((All(book) == std::map<std::uint32_t, float>{ { 2, 1.0f } }));
}

TEST("npc healths: a report outside [0, 1] is clamped, one that is no number is dropped, and a forgotten NPC is gone")
{
	NpcHealths::Book book;
	book.Heard(1, -0.5f);
	book.Heard(2, 0.5f);
	book.Heard(2, std::numeric_limits<float>::quiet_NaN());
	book.Heard(3, std::numeric_limits<float>::infinity());
	CHECK((All(book) == std::map<std::uint32_t, float>{ { 1, 0.0f }, { 2, 0.5f } }));
	book.Heard(2, 3.0f);
	CHECK(All(book)[2] == 1.0f);
	book.Forget(1);
	CHECK(!book.Contains(1));
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("npc healths: at most MAX_KEPT; past it a healed one goes first, then the one heard of longest ago")
{
	NpcHealths::Book book;
	for (std::uint32_t i = 0; i < NpcHealths::MAX_KEPT; ++i) {
		book.Heard(100 + i, 0.5f);
	}
	book.Heard(100 + 7, 1.0f);   // healed
	book.Heard(100 + 0, 0.25f);  // heard again: no longer the oldest
	book.Heard(1, 0.1f);
	CHECK(book.Size() == NpcHealths::MAX_KEPT);
	CHECK(book.Contains(1));
	CHECK(!book.Contains(100 + 7));
	book.Heard(2, 0.1f);
	CHECK(book.Size() == NpcHealths::MAX_KEPT);
	CHECK(book.Contains(100 + 0));
	CHECK(!book.Contains(100 + 1));  // the oldest still hurt
	CHECK(book.Contains(2));
}
