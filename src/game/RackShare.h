#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Weapons on weapon racks, shown to friends (src/game/RackDisplays.cpp). Contraptions Workshop's weapon rack takes a
// weapon put in it, drops it into the world (a reference made at runtime, in the rack's game alone) and holds it still at
// one of its nodes, linked to the rack. So a friend saw an empty rack. Each game now tells the others what its own racks
// show (kReportRackWeapons: the whole list), and each other game shows those weapons as copies where they are: held still,
// nobody can take one, workshop mode can't scrap or store one, and none is ever saved. The weapon stays its owner's, in
// its owner's rack, as a weapon in a container does: a rack is not a way to give an item (a dropped one is, Drops).
// Kept free of the game so the rules can be tested.
namespace RackShare
{
	using RackWeapon = Protocol::RackWeapon;

	// A friend's rack weapon is shown while our player is this near its place (game units, across the ground), and its
	// copy goes past REMOVE_RANGE (the gap keeps one at the edge from coming and going).
	inline constexpr float PLACE_RANGE = 6144.0f;
	inline constexpr float REMOVE_RANGE = 8192.0f;
	// Our own rack's weapon this near a friend's place is the same slot of the same rack: ours is shown, theirs not (two
	// weapons in one slot).
	inline constexpr float SAME_SLOT = 16.0f;
	// The owner's walk: racks this near our player are looked at; a weapon the list kept from before that lies well inside
	// it and isn't found any more has left its rack.
	inline constexpr float SCAN_RADIUS = 6144.0f;
	inline constexpr float GONE_INSIDE = 4096.0f;
	// A weapon this far from where the last report put it, or turned this much, is reported again (a rack moved in
	// workshop mode carries its weapons).
	inline constexpr float MOVED = 2.0f;
	inline constexpr float TURNED = 0.02f;

	// Where something is: its interior cell, or its worldspace (the other 0), and its place.
	struct Spot
	{
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0, y = 0, z = 0;
	};

	inline Spot SpotOf(const RackWeapon& a_entry)
	{
		return { a_entry.item.cell, a_entry.item.worldspace, a_entry.item.x, a_entry.item.y, a_entry.item.z };
	}

	// In the same interior cell, or in the same worldspace outside.
	inline bool SameSpace(const Spot& a_a, const Spot& a_b)
	{
		return a_a.cell != 0 ? a_a.cell == a_b.cell : (a_b.cell == 0 && a_a.worldspace != 0 && a_a.worldspace == a_b.worldspace);
	}

	// Within a_range across the ground, in the same space.
	inline bool Near(const Spot& a_a, const Spot& a_b, float a_range)
	{
		if (!SameSpace(a_a, a_b)) {
			return false;
		}
		const float dx = a_a.x - a_b.x;
		const float dy = a_a.y - a_b.y;
		return dx * dx + dy * dy <= a_range * a_range;
	}

	inline float Distance3(const Spot& a_a, const Spot& a_b)
	{
		const float dx = a_a.x - a_b.x;
		const float dy = a_a.y - a_b.y;
		const float dz = a_a.z - a_b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// Our own racks show a weapon in a_entry's slot: theirs isn't shown over it.
	inline bool OursThere(const RackWeapon& a_entry, std::span<const RackWeapon> a_ours)
	{
		const auto at = SpotOf(a_entry);
		return std::ranges::any_of(a_ours, [&](const RackWeapon& a_own) {
			const auto own = SpotOf(a_own);
			return SameSpace(at, own) && Distance3(at, own) <= SAME_SLOT;
		});
	}

	// The same weapon shown the same way: its item, and its place and angles within MOVED and TURNED.
	inline bool Same(const RackWeapon& a_a, const RackWeapon& a_b)
	{
		const auto turned = [](float a_x, float a_y) { return std::fabs(a_x - a_y) > TURNED; };
		const auto a = SpotOf(a_a);
		const auto b = SpotOf(a_b);
		return a_a.key == a_b.key && a_a.item.base == a_b.item.base && a_a.item.templateIndex == a_b.item.templateIndex && a_a.item.mods == a_b.item.mods &&
		       a_a.item.name == a_b.item.name && SameSpace(a, b) && Distance3(a, b) <= MOVED && !turned(a_a.angleX, a_b.angleX) && !turned(a_a.angleY, a_b.angleY) &&
		       !turned(a_a.angleZ, a_b.angleZ);
	}

	// Two lists that show the same (in any order): the last report still holds.
	inline bool SameList(std::span<const RackWeapon> a_a, std::span<const RackWeapon> a_b)
	{
		if (a_a.size() != a_b.size()) {
			return false;
		}
		return std::ranges::all_of(a_a, [&](const RackWeapon& a_entry) {
			const auto it = std::ranges::find(a_b, a_entry.key, &RackWeapon::key);
			return it != a_b.end() && Same(a_entry, *it);
		});
	}

	// The owner's list after a walk of the racks near a_here: what the walk found, and what the last list held that lies
	// out of the walk's sight (another space, or past GONE_INSIDE: its cell may not be loaded), so a friend standing by a
	// rack our player walked away from still sees its weapon. Up to MAX_RACK_WEAPONS, the found ones first.
	inline std::vector<RackWeapon> Merge(std::span<const RackWeapon> a_last, std::vector<RackWeapon> a_found, const Spot& a_here)
	{
		auto out = std::move(a_found);
		if (out.size() > Protocol::MAX_RACK_WEAPONS) {
			out.resize(Protocol::MAX_RACK_WEAPONS);
		}
		for (const auto& entry : a_last) {
			if (out.size() >= Protocol::MAX_RACK_WEAPONS) {
				break;
			}
			if (Near(SpotOf(entry), a_here, GONE_INSIDE)) {
				continue;  // in sight and not found: it left its rack
			}
			if (std::ranges::find(out, entry.key, &RackWeapon::key) != out.end()) {
				continue;
			}
			// A weapon found now in its slot under another key (the rack dropped it again): the found one stands.
			const auto at = SpotOf(entry);
			if (std::ranges::any_of(out, [&](const RackWeapon& a_found) { return SameSpace(SpotOf(a_found), at) && Distance3(SpotOf(a_found), at) <= SAME_SLOT; })) {
				continue;
			}
			out.push_back(entry);
		}
		return out;
	}

	// The key our copies of a friend's rack weapons go by: the friend, and the entry's key in their game.
	inline std::uint64_t CopyKey(std::uint32_t a_playerId, std::uint32_t a_key)
	{
		return (std::uint64_t{ a_playerId } << 32) | a_key;
	}
}
