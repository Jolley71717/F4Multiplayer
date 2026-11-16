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
	CHECK(!RackShare::Near(RackShare::SpotOf(entry), Outside(10000.0f, 0.0f), RackShare::REMOVE_RANGE));
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

TEST("rack share: a friend's rack weapon is shown only where our game has a rack for it to hang on")
{
	const auto gun = Entry(0xFF000101, 100.0f, 200.0f, 50.0f);
	// No rack here (the friend's Sanctuary from before the session, which our world never had): not shown.
	CHECK(!RackShare::RackThere(gun, {}));
	const std::vector<Spot> far{ { 0, SANCTUARY_WORLD, 100.0f + RackShare::RACK_NEAR + 1.0f, 200.0f, 50.0f } };
	CHECK(!RackShare::RackThere(gun, far));
	// A rack at the place (the friend's own as the shared workshop shows it, or one of ours): shown.
	const std::vector<Spot> rack{ { 0, SANCTUARY_WORLD, 140.0f, 200.0f, 10.0f } };
	CHECK(RackShare::RackThere(gun, rack));
	// The same coordinates in another space are no rack of this one.
	const std::vector<Spot> inside{ { HOME_CELL, 0, 100.0f, 200.0f, 50.0f } };
	CHECK(!RackShare::RackThere(gun, inside));
}

TEST("rack share: a copy an inventory took is taken back from the inventory that can be named, else counted a duplicate")
{
	using RackShare::Holder;
	CHECK(RackShare::HolderOf(true, -1.0f, 0) == Holder::kNamed);
	CHECK(RackShare::HolderOf(true, 1.0f, 2) == Holder::kNamed);
	CHECK(RackShare::HolderOf(false, 0.0f, 1) == Holder::kPlayer);
	CHECK(RackShare::HolderOf(false, RackShare::PICKUP_PAIRING_SECONDS, 1) == Holder::kPlayer);
	CHECK(RackShare::HolderOf(false, RackShare::PICKUP_PAIRING_SECONDS + 0.5f, 1) == Holder::kUnknown);
	CHECK(RackShare::HolderOf(false, -1.0f, 0) == Holder::kUnknown);  // our player picked up none of its base
}

TEST("rack share: our player's own gun of the copy's kind is never taken back for it")
{
	using RackShare::Holder;
	using Clock = RackShare::PickupLog::Clock;
	constexpr std::uint32_t PISTOL = 0x0004F46A;
	constexpr std::uint32_t RIFLE = 0x0004F46B;
	const auto              t0 = Clock::time_point{} + std::chrono::hours{ 1 };
	const auto              ms = [&](int a_ms) { return t0 + std::chrono::milliseconds{ a_ms }; };
	const auto              holder = [](const RackShare::PickupLog::Recent& a_recent) { return RackShare::HolderOf(false, a_recent.since, a_recent.count); };

	// Our player takes the copy: their one pickup of that kind half a second before the copy is found gone pairs, and is
	// used up, so it never stands for a second copy.
	RackShare::PickupLog log;
	log.Add(PISTOL, t0);
	auto recent = log.Pair(PISTOL, ms(500));
	CHECK(recent.count == 1);
	CHECK(holder(recent) == Holder::kPlayer);
	CHECK(log.Size() == 0);
	CHECK(holder(log.Pair(PISTOL, ms(600))) == Holder::kUnknown);

	// A pickup the frame saw after it began (its event comes on another thread) is just now, and pairs.
	log.Add(PISTOL, ms(1100));
	CHECK(holder(log.Pair(PISTOL, ms(1000))) == Holder::kPlayer);

	// Our player looted their own pistol seconds before an NPC took the copy: too old to pair, and kept (not used up).
	log.Add(PISTOL, ms(2000));
	CHECK(holder(log.Pair(PISTOL, ms(2000 + 3000))) == Holder::kUnknown);
	CHECK(log.Size() == 1);
	// A pickup of another kind just now is no pickup of the copy.
	log.Add(RIFLE, ms(5100));
	CHECK(holder(log.Pair(PISTOL, ms(5200))) == Holder::kUnknown);
	CHECK(log.Size() == 2);

	// Two pickups of that kind just now: which one was the copy can't be told, so neither is taken, and neither is used up.
	log.Clear();
	log.Add(PISTOL, ms(6000));
	log.Add(PISTOL, ms(6200));
	recent = log.Pair(PISTOL, ms(6500));
	CHECK(recent.count == 2);
	CHECK(holder(recent) == Holder::kUnknown);
	CHECK(log.Size() == 2);
	CHECK(log.Pair(PISTOL, ms(6600)).count == 2);

	// Pickups older than the log keeps go, and no more than MAX are kept (the oldest first).
	CHECK(log.Pair(RIFLE, ms(6000) + RackShare::PickupLog::KEPT + std::chrono::seconds{ 1 }).count == 0);
	CHECK(log.Size() == 0);
	for (std::size_t i = 0; i < RackShare::PickupLog::MAX + 4; ++i) {
		log.Add(RIFLE, ms(7000));
	}
	CHECK(log.Size() == RackShare::PickupLog::MAX);

	// Within an activation's reach of the copy, up to a rack's top slot, in its space: a pickup there may be the copy.
	const Spot copy{ 0, SANCTUARY_WORLD, 100.0f, 200.0f, 150.0f };
	CHECK(RackShare::InReach({ 0, SANCTUARY_WORLD, 100.0f, 80.0f, 0.0f }, copy));    // beside the rack, the copy at its top
	CHECK(!RackShare::InReach({ 0, SANCTUARY_WORLD, 100.0f, 600.0f, 0.0f }, copy));  // across the room: not their pickup
	CHECK(!RackShare::InReach({ HOME_CELL, 0, 100.0f, 200.0f, 150.0f }, copy));     // the same numbers in another space
}

TEST("rack share: a reference found to be no rack is asked again, so a rack whose script is bound late is seen")
{
	using Clock = RackShare::RackBook::Clock;
	RackShare::RackBook book;
	const auto          t0 = Clock::time_point{} + std::chrono::hours{ 1 };
	const auto          copyRack = RackShare::RackBook::Key(0xFF000A01, 0x0100B001);  // the shared workshop's copy, just placed
	CHECK(!book.Find(copyRack, t0).has_value());
	CHECK(!book.Set(copyRack, false, t0));  // its script not bound yet
	REQUIRE(book.Find(copyRack, t0).has_value());
	CHECK(!*book.Find(copyRack, t0));
	CHECK(book.Find(copyRack, t0 + RackShare::NOT_A_RACK_FOR - std::chrono::milliseconds{ 1 }).has_value());
	// By the time the friend's weapon is looked at again, the no has run out: the VM is asked again, and the rack is seen.
	CHECK(!book.Find(copyRack, t0 + RackShare::NO_RACK_WAIT).has_value());
	CHECK(book.Set(copyRack, true, t0 + RackShare::NO_RACK_WAIT));  // a rack found: said once
	CHECK(!book.Set(copyRack, true, t0 + RackShare::NO_RACK_WAIT));
	// A yes holds for the load.
	REQUIRE(book.Find(copyRack, t0 + std::chrono::hours{ 5 }).has_value());
	CHECK(*book.Find(copyRack, t0 + std::chrono::hours{ 5 }));
	// Another base under the same id (a scrap, and the id given again) is another reference.
	CHECK(!book.Find(RackShare::RackBook::Key(0xFF000A01, 0x0100B002), t0).has_value());
	// A load forgets every answer.
	book.Clear();
	CHECK(!book.Find(copyRack, t0).has_value());
}

TEST("rack share: the noes kept are bounded, and the racks found are never dropped for room")
{
	using Clock = RackShare::RackBook::Clock;
	RackShare::RackBook book;
	const auto          t0 = Clock::time_point{} + std::chrono::hours{ 1 };
	const auto          rack = RackShare::RackBook::Key(1, 1);
	book.Set(rack, true, t0);
	for (std::uint32_t i = 2; book.Size() < RackShare::RackBook::MAX_ANSWERS; ++i) {
		book.Set(RackShare::RackBook::Key(i, 1), false, t0);
	}
	const auto later = t0 + RackShare::NO_RACK_WAIT;
	book.Set(RackShare::RackBook::Key(0xFFFF, 2), false, later);
	CHECK(book.Size() == 2u);  // the noes that ran out went
	CHECK(*book.Find(rack, later));
}

TEST("rack share: a friend's list said again looks afresh only at the weapons that changed")
{
	const std::vector<RackWeapon> last{ Entry(0xFF000801, 100.0f, 0.0f), Entry(0xFF000802, 200.0f, 0.0f), Entry(0xFF000803, 300.0f, 0.0f) };
	// The same list again (the 30 s resend), in another order: nothing.
	const std::vector<RackWeapon> again{ last[2], last[0], last[1] };
	CHECK(RackShare::Changed(last, again).empty());
	// One moved, one gone, one new: those three.
	auto now = last;
	now[0].item.x += 50.0f;
	now.pop_back();
	now.push_back(Entry(0xFF000804, 400.0f, 0.0f));
	auto changed = RackShare::Changed(last, now);
	std::ranges::sort(changed);
	CHECK((changed == std::vector<std::uint32_t>{ 0xFF000801, 0xFF000803, 0xFF000804 }));
	// The first list, and an emptied one: every weapon.
	CHECK(RackShare::Changed({}, last).size() == 3u);
	CHECK(RackShare::Changed(last, {}).size() == 3u);
}
