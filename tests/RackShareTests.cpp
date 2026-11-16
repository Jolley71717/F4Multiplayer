// Weapons on weapon racks shown to friends (src/game/RackShare.h): the owner's list, what is reported again, and which
// of a friend's weapons our game shows.

#include "Test.h"

#include "game/RackShare.h"

namespace
{
	using RackShare::RackWeapon;
	using RackShare::Spot;

	constexpr std::uint32_t SANCTUARY_WORLD = 0x0000003C;  // the Commonwealth
	constexpr std::uint32_t HOME_CELL = 0x0001F0C5;        // an interior

	RackWeapon Entry(std::uint32_t a_key, float a_x, float a_y, float a_z = 0.0f, std::uint32_t a_world = SANCTUARY_WORLD, std::uint32_t a_cell = 0)
	{
		RackWeapon entry;
		entry.key = a_key;
		entry.item.base = 0x0004F46A;  // a 10mm pistol
		entry.item.count = 1;
		entry.item.templateIndex = 0xFFFF;
		entry.item.mods = { { 0x0001A2B3, 0, 0 } };
		entry.item.worldspace = a_cell != 0 ? 0 : a_world;
		entry.item.cell = a_cell;
		entry.item.x = a_x;
		entry.item.y = a_y;
		entry.item.z = a_z;
		entry.angleZ = 1.5f;
		return entry;
	}

	Spot Outside(float a_x, float a_y)
	{
		return { 0, SANCTUARY_WORLD, a_x, a_y, 0.0f };
	}
}

TEST("rack share: a friend's rack weapon is shown near its place, in the same space only")
{
	const auto entry = Entry(0xFF000801, 1000.0f, 0.0f);
	CHECK(RackShare::Near(RackShare::SpotOf(entry), Outside(0.0f, 0.0f), RackShare::PLACE_RANGE));
	CHECK(!RackShare::Near(RackShare::SpotOf(entry), Outside(9000.0f, 0.0f), RackShare::REMOVE_RANGE));
	CHECK(!RackShare::Near(RackShare::SpotOf(entry), Spot{ HOME_CELL, 0, 1000.0f, 0.0f, 0.0f }, RackShare::PLACE_RANGE));  // we are inside
	CHECK(!RackShare::Near(RackShare::SpotOf(entry), Spot{ 0, 0x00123456, 1000.0f, 0.0f, 0.0f }, RackShare::PLACE_RANGE));  // another worldspace
	const auto inside = Entry(0xFF000802, 10.0f, 10.0f, 0.0f, 0, HOME_CELL);
	CHECK(RackShare::Near(RackShare::SpotOf(inside), Spot{ HOME_CELL, 0, 0.0f, 0.0f, 0.0f }, RackShare::PLACE_RANGE));
	CHECK(!RackShare::Near(RackShare::SpotOf(inside), Spot{ HOME_CELL + 1, 0, 0.0f, 0.0f, 0.0f }, RackShare::PLACE_RANGE));
	CHECK(!RackShare::Near(RackShare::SpotOf(inside), Outside(10.0f, 10.0f), RackShare::PLACE_RANGE));
	// A place with neither a cell nor a worldspace is nowhere.
	CHECK(!RackShare::SameSpace(Spot{}, Spot{}));
}

TEST("rack share: our own weapon in the same slot is shown, not theirs")
{
	const auto theirs = Entry(0xFF000801, 100.0f, 100.0f, 50.0f);
	const std::vector<RackWeapon> ours{ Entry(0xFF000999, 105.0f, 100.0f, 50.0f) };
	CHECK(RackShare::OursThere(theirs, ours));
	const std::vector<RackWeapon> nextSlot{ Entry(0xFF000999, 100.0f, 100.0f, 80.0f) };  // the slot above
	CHECK(!RackShare::OursThere(theirs, nextSlot));
	CHECK(!RackShare::OursThere(theirs, {}));
}

TEST("rack share: a list is reported again only when what it shows changed")
{
	const std::vector<RackWeapon> last{ Entry(0xFF000801, 0.0f, 0.0f), Entry(0xFF000802, 30.0f, 0.0f) };
	std::vector<RackWeapon>       now{ last[1], last[0] };  // another order
	CHECK(RackShare::SameList(last, now));
	now[0].item.x += 1.0f;  // a wobble
	now[0].angleZ += 0.01f;
	CHECK(RackShare::SameList(last, now));
	now[0].item.x += 10.0f;  // the rack moved
	CHECK(!RackShare::SameList(last, now));
	now = { last[1], last[0] };
	now[1].angleX = 1.0f;  // turned
	CHECK(!RackShare::SameList(last, now));
	now = { last[1], last[0] };
	now[1].item.mods.clear();  // another weapon in that slot under the same key can't be, but its mods are compared
	CHECK(!RackShare::SameList(last, now));
	now = { last[0] };
	CHECK(!RackShare::SameList(last, now));
	CHECK(RackShare::SameList({}, {}));
}

TEST("rack share: the owner's list keeps what lies out of sight, and drops what left its rack in sight")
{
	const auto near = Entry(0xFF000801, 100.0f, 0.0f);
	const auto far = Entry(0xFF000802, 20000.0f, 0.0f);
	const auto inside = Entry(0xFF000803, 0.0f, 0.0f, 0.0f, 0, HOME_CELL);
	const std::vector<RackWeapon> last{ near, far, inside };
	// The walk found nothing by us: the near one was taken off its rack; the others are out of sight and kept.
	auto merged = RackShare::Merge(last, {}, Outside(0.0f, 0.0f));
	CHECK(merged.size() == 2);
	CHECK(std::ranges::find(merged, far.key, &RackWeapon::key) != merged.end());
	CHECK(std::ranges::find(merged, inside.key, &RackWeapon::key) != merged.end());
	// Found again: once, under its key, as the walk saw it.
	auto moved = near;
	moved.item.z = 7.0f;
	merged = RackShare::Merge(last, { moved }, Outside(0.0f, 0.0f));
	CHECK(merged.size() == 3);
	CHECK(merged[0].item.z == 7.0f);
	// A kept entry whose slot a weapon found now under another key fills (the rack dropped it anew after a load): the
	// found one only.
	auto again = far;
	again.key = 0xFF000900;
	merged = RackShare::Merge(last, { again }, Outside(20000.0f, 0.0f));
	CHECK(merged.size() == 3);  // far in sight and not found under its key: gone; near and inside out of sight: kept
	CHECK(std::ranges::find(merged, far.key, &RackWeapon::key) == merged.end());
	CHECK(std::ranges::find(merged, near.key, &RackWeapon::key) != merged.end());
	merged = RackShare::Merge(last, { again }, Outside(30000.0f, 0.0f));
	CHECK(std::ranges::find(merged, far.key, &RackWeapon::key) == merged.end());  // same slot: not twice
	CHECK(merged.size() == 3);
}

TEST("rack share: the owner's list is never longer than the wire carries")
{
	std::vector<RackWeapon> found;
	for (std::uint32_t i = 0; i < Protocol::MAX_RACK_WEAPONS + 5; ++i) {
		found.push_back(Entry(0xFF001000 + i, static_cast<float>(i) * 40.0f, 0.0f));
	}
	const std::vector<RackWeapon> last{ Entry(0xFF000801, 50000.0f, 0.0f) };
	const auto merged = RackShare::Merge(last, found, Outside(0.0f, 0.0f));
	CHECK(merged.size() == Protocol::MAX_RACK_WEAPONS);
	CHECK(merged.front().key == 0xFF001000);  // the found ones first
	CHECK(RackShare::CopyKey(2, 0xFF000801) == 0x00000002FF000801ull);
}
