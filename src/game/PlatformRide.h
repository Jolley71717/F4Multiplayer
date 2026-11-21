#pragma once

#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

// A player riding something that moves (a lift's car, moved by its own script in each game): their stand-in in a friend's
// game is placed where they report being, so it sank into the car or hung above it by the lag between the two games' cars,
// and "fell through the floor or the ceiling" (a Nexus report, 0.20.4). Since protocol 85 the player's state names what they
// stand on when it is a thing that moves (the reference, the node of its 3D their feet rest on, and where they are from that
// node), and a friend's game places the stand-in from its own copy of that node, so it rides the friend's car wherever that
// is in its travel. Nothing is named while they stand on the ground or a floor, so nothing else changes.
// Kept free of the game so the rules can be tested.
namespace PlatformRide
{
	// What the game reads under the player's feet each sample: the collision body that supports the character controller,
	// its scene node, and the reference that node belongs to (0 when it is none a friend's game has: the ground, a thing made
	// at runtime).
	struct Support
	{
		std::uint32_t ref = 0;
		std::string   node;
		float         x = 0, y = 0, z = 0;  // where the node is drawn (its world translate)
		// Its body is an animated static's (the collision layer of a lift's car, a drawbridge, a moving platform): something
		// the game's own animation moves, never physics.
		bool animated = false;
	};

	// What goes in the state: the reference and node, and the player's place from the node.
	struct Report
	{
		std::uint32_t ref = 0;
		std::string   node;
		float         dx = 0, dy = 0, dz = 0;
	};

	// A support that moved this much between two samples is moving.
	inline constexpr float MOVED = 0.5f;
	// ... and is reported for this long after it last moved, so the stand-in stays put on the car as it settles.
	inline constexpr auto MOVING_FOR = std::chrono::seconds(2);
	// A friend's game places the stand-in from its own node only within this of where the player reports being: further,
	// its copy of the thing is somewhere else altogether (its car at the other end of its travel), and the report stands.
	inline constexpr float REACH = 1024.0f;
	// ... and no longer than APART_FOR while its node stays more than APART from there: two cars a lag apart close that
	// gap within the ride, two that stand at different stops (a press that was never replayed, a cell loaded later) never
	// do, and then the stand-in belongs where its player is, not on our car.
	inline constexpr float APART = 160.0f;
	inline constexpr auto  APART_FOR = std::chrono::seconds(3);
	// A rider with nothing under their feet (the car dropped from under them, a hop) is still its rider for this long.
	inline constexpr auto HOLD = std::chrono::milliseconds(400);
	// The node's name on the wire.
	inline constexpr std::size_t MAX_NODE_NAME = 64;

	// The sender's side: what under the player is named. An animated static (Support::animated) always, moving or not: when
	// the friend is the one who pressed the button, their car leaves a network trip before ours, and a stand-in named onto
	// the car only once ours moves would hang in the air or stand in the floor for that trip. Anything else only while it is
	// seen to move, from sample to sample, and for MOVING_FOR after.
	class Sender
	{
	public:
		using Clock = std::chrono::steady_clock;

		Report Sample(const Support& a_support, float a_x, float a_y, float a_z, Clock::time_point a_now)
		{
			if (a_support.ref == 0 || a_support.node.empty() || a_support.node.size() > MAX_NODE_NAME) {
				last = {};
				movedAt = {};
				return {};
			}
			if (a_support.ref == last.ref && a_support.node == last.node) {
				const float dx = a_support.x - last.x, dy = a_support.y - last.y, dz = a_support.z - last.z;
				if (dx * dx + dy * dy + dz * dz > MOVED * MOVED) {
					movedAt = a_now;
				}
			} else {
				movedAt = {};
			}
			last = a_support;
			if (!a_support.animated && (movedAt == Clock::time_point{} || a_now - movedAt > MOVING_FOR)) {
				return {};
			}
			return { a_support.ref, a_support.node, a_x - a_support.x, a_y - a_support.y, a_z - a_support.z };
		}

	private:
		Support           last;
		Clock::time_point movedAt{};
	};

	// The receiver's side: where the stand-in goes, given where our own node is drawn: from the node, or where they report
	// being when our node is nowhere near that.
	struct Spot
	{
		float x = 0, y = 0, z = 0;
		bool  rode = false;   // placed from our node
		float apart = 0.0f;   // how far our node's place for them is from where they report being (0 when no node)
	};

	inline Spot Place(const Protocol::PlayerState& a_state, bool a_haveNode, float a_nodeX, float a_nodeY, float a_nodeZ)
	{
		if (a_state.platform == 0 || !a_haveNode) {
			return { a_state.x, a_state.y, a_state.z, false, 0.0f };
		}
		const float x = a_nodeX + a_state.px, y = a_nodeY + a_state.py, z = a_nodeZ + a_state.pz;
		const float dx = x - a_state.x, dy = y - a_state.y, dz = z - a_state.z;
		const float apart = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (apart > REACH) {
			return { a_state.x, a_state.y, a_state.z, false, apart };
		}
		return { x, y, z, true, apart };
	}

	// One friend's stand-in over time: Place, and the report again once our node has stayed more than APART from it for
	// APART_FOR (until it comes back within APART).
	class Placer
	{
	public:
		using Clock = std::chrono::steady_clock;

		Spot Place(const Protocol::PlayerState& a_state, bool a_haveNode, float a_nodeX, float a_nodeY, float a_nodeZ, Clock::time_point a_now)
		{
			auto spot = PlatformRide::Place(a_state, a_haveNode, a_nodeX, a_nodeY, a_nodeZ);
			if (!spot.rode || spot.apart <= APART) {
				apartSince = {};
				return spot;
			}
			if (apartSince == Clock::time_point{}) {
				apartSince = a_now;
			}
			if (a_now - apartSince > APART_FOR) {
				return { a_state.x, a_state.y, a_state.z, false, spot.apart };
			}
			return spot;
		}

	private:
		Clock::time_point apartSince{};
	};
}
