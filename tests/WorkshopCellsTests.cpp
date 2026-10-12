#include "Test.h"
#include "game/WorkshopCells.h"

#include <algorithm>
#include <string>

// What waits for its cell to load (the copies of what others built, their scraps of the world), and
// which of it a cell that attaches brings up.
namespace
{
	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t VAULT = 0x0001F2A1;  // an interior cell

	using Waiting = WorkshopCells::Waiting<int, std::string>;

	bool Has(const std::vector<int>& a_keys, int a_key)
	{
		return std::ranges::find(a_keys, a_key) != a_keys.end();
	}
}

TEST("workshop cells: a position falls in the cell whose grid square holds it")
{
	CHECK(WorkshopCells::Grid(0.0f) == 0);
	CHECK(WorkshopCells::Grid(4095.9f) == 0);
	CHECK(WorkshopCells::Grid(4096.0f) == 1);
	CHECK(WorkshopCells::Grid(-0.1f) == -1);  // below zero rounds down, not toward zero
	CHECK(WorkshopCells::Grid(-4096.0f) == -1);
	CHECK(WorkshopCells::Grid(-4096.1f) == -2);
	const auto where = WorkshopCells::Exterior(COMMONWEALTH, -20000.0f, 90000.0f);
	CHECK(where.space == COMMONWEALTH);
	CHECK(where.x == -5);
	CHECK(where.y == 21);
}

TEST("workshop cells: a cell brings up what waits in it and in the eight around it, not farther")
{
	Waiting waiting;
	waiting.Put({ COMMONWEALTH, 3, 4 }, 1, "here");
	waiting.Put({ COMMONWEALTH, 2, 5 }, 2, "next to it, corner");
	waiting.Put({ COMMONWEALTH, 5, 4 }, 3, "two cells away");
	waiting.Put({ VAULT, 3, 4 }, 4, "another space");
	const auto near = waiting.Near({ COMMONWEALTH, 3, 4 });
	CHECK(near.size() == 2);
	CHECK(Has(near, 1));
	CHECK(Has(near, 2));
	// From the next cell over: the first one, and the one two cells from it.
	const auto over = waiting.Near({ COMMONWEALTH, 4, 4 });
	CHECK(over.size() == 2);
	CHECK(Has(over, 1));
	CHECK(Has(over, 3));
	CHECK(waiting.Near({ COMMONWEALTH, 10, 10 }).empty());
}

TEST("workshop cells: an interior brings up only what waits in it")
{
	Waiting waiting;
	waiting.Put(WorkshopCells::Interior(VAULT), 1, "inside");
	waiting.Put(WorkshopCells::Interior(VAULT + 1), 2, "another interior");
	waiting.Put({ COMMONWEALTH, 0, 0 }, 3, "outside");
	const auto near = waiting.Near(WorkshopCells::Interior(VAULT));
	REQUIRE(near.size() == 1);
	CHECK(near[0] == 1);
}

TEST("workshop cells: what waits nowhere (its workbench isn't in this game) is never brought up")
{
	Waiting waiting;
	waiting.Put({}, 1, "a mod's settlement we don't have");
	CHECK(waiting.Size() == 1);
	CHECK(waiting.Near({}).empty());
	CHECK(waiting.Near({ COMMONWEALTH, 0, 0 }).empty());
}

TEST("workshop cells: putting under the same key again replaces it, in its new cell")
{
	Waiting waiting;
	waiting.Put({ COMMONWEALTH, 0, 0 }, 1, "placed");
	waiting.Put({ COMMONWEALTH, 9, 9 }, 1, "moved");
	CHECK(waiting.Size() == 1);
	CHECK(waiting.Near({ COMMONWEALTH, 0, 0 }).empty());
	REQUIRE(waiting.Near({ COMMONWEALTH, 9, 9 }).size() == 1);
	REQUIRE(waiting.Find(1));
	CHECK(*waiting.Find(1) == "moved");
}

TEST("workshop cells: what's erased is gone from its cell too")
{
	Waiting waiting;
	waiting.Put({ COMMONWEALTH, 0, 0 }, 1, "a");
	waiting.Put({ COMMONWEALTH, 0, 0 }, 2, "b");
	CHECK(waiting.Erase(1));
	CHECK(!waiting.Erase(1));
	CHECK(!waiting.Find(1));
	const auto near = waiting.Near({ COMMONWEALTH, 0, 0 });
	REQUIRE(near.size() == 1);
	CHECK(near[0] == 2);
	waiting.Clear();
	CHECK(waiting.Empty());
	CHECK(waiting.Near({ COMMONWEALTH, 0, 0 }).empty());
}
