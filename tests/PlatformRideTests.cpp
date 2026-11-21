// A player riding a lift's car (game/PlatformRide.h): what their state names while the car moves, and where a friend's game
// places their stand-in from its own car.

#include "Test.h"

#include "game/PlatformRide.h"

using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;

	PlatformRide::Support Car(float a_z, const char* a_node = "ElevatorPlatform")
	{
		return { 0x00110358, a_node, 100.0f, 200.0f, a_z };
	}
}

TEST("PlatformRide: nothing is named on the ground, nor on a thing that holds still")
{
	PlatformRide::Sender sender;
	const auto t0 = Clock::now();
	CHECK(sender.Sample({}, 1, 2, 3, t0).ref == 0);
	CHECK(sender.Sample(Car(0), 110, 210, 10, t0).ref == 0);         // first seen: not known to move
	CHECK(sender.Sample(Car(0), 112, 210, 10, t0 + 50ms).ref == 0);  // still: the player walked, the car did not
	CHECK(sender.Sample(Car(0.3f), 112, 210, 10, t0 + 100ms).ref == 0);  // a jitter under MOVED
}

TEST("PlatformRide: a moving car is named with the player's place from its node, for MOVING_FOR after it stops")
{
	PlatformRide::Sender sender;
	const auto t0 = Clock::now();
	CHECK(sender.Sample(Car(0), 110, 210, 10, t0).ref == 0);
	const auto moving = sender.Sample(Car(30), 110, 210, 40, t0 + 50ms);
	CHECK(moving.ref == 0x00110358);
	CHECK(moving.node == "ElevatorPlatform");
	CHECK(moving.dx == 10.0f);
	CHECK(moving.dy == 10.0f);
	CHECK(moving.dz == 10.0f);
	// stopped at the top: still named for a while, then not
	CHECK(sender.Sample(Car(300), 110, 210, 310, t0 + 1s).ref != 0);
	CHECK(sender.Sample(Car(300), 110, 210, 310, t0 + 2s).ref != 0);
	CHECK(sender.Sample(Car(300), 110, 210, 310, t0 + 3s + 100ms).ref == 0);
	// moving again: named again
	CHECK(sender.Sample(Car(290), 110, 210, 300, t0 + 4s).ref != 0);
}

TEST("PlatformRide: stepping onto another thing, or off, starts afresh")
{
	PlatformRide::Sender sender;
	const auto t0 = Clock::now();
	CHECK(sender.Sample(Car(0), 110, 210, 10, t0).ref == 0);
	CHECK(sender.Sample(Car(30), 110, 210, 40, t0 + 50ms).ref != 0);
	PlatformRide::Support other = Car(40, "Counterweight");
	other.ref = 0x00110359;
	CHECK(sender.Sample(other, 110, 210, 50, t0 + 100ms).ref == 0);  // another thing: not yet seen to move
	CHECK(sender.Sample({}, 110, 210, 50, t0 + 150ms).ref == 0);     // the ground
	CHECK(sender.Sample(Car(60), 110, 210, 70, t0 + 200ms).ref == 0);  // back on the car: its last place forgotten
	CHECK(sender.Sample(Car(70), 110, 210, 80, t0 + 250ms).ref != 0);
	// a node name too long for the wire names nothing
	CHECK(sender.Sample(Car(80, std::string(PlatformRide::MAX_NODE_NAME + 1, 'x').c_str()), 110, 210, 90, t0 + 300ms).ref == 0);
}

TEST("PlatformRide: a friend's game places the stand-in from its own node, unless that is nowhere near")
{
	Protocol::PlayerState state;
	state.x = 110;
	state.y = 210;
	state.z = 40;
	// nothing ridden: where they report being
	auto spot = PlatformRide::Place(state, true, 100, 200, 30);
	CHECK(!spot.rode);
	CHECK(spot.z == 40);
	state.platform = 0x00110358;
	state.platformNode = "ElevatorPlatform";
	state.px = 10;
	state.py = 10;
	state.pz = 10;
	// our car a little behind theirs: from our node
	spot = PlatformRide::Place(state, true, 100, 200, 5);
	CHECK(spot.rode);
	CHECK(spot.x == 110);
	CHECK(spot.z == 15);
	// our car at the other end of its travel: where they report being
	spot = PlatformRide::Place(state, true, 100, 200, 30 - PlatformRide::REACH - 1);
	CHECK(!spot.rode);
	CHECK(spot.z == 40);
	// no node of ours: where they report being
	spot = PlatformRide::Place(state, false, 0, 0, 0);
	CHECK(!spot.rode);
	CHECK(spot.x == 110);
}
