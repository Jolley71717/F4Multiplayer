#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
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
	// A friend's rack weapon is shown only where our game has the rack that holds it in theirs: a rack of that base within
	// RACK_SPOT of where theirs stands (the friend's own, as WorkshopSync shows it at a settlement we share, put at the
	// owner's own coordinates; or one in our world). With none, a copy would hang in mid air, or in a wall of a settlement
	// our world has built otherwise; any rack near would not do, as racks stand side by side (one of theirs our world lacks
	// beside one it has). A rack holds its weapons within its own bounds, RACK_NEAR of it at most: an entry whose rack is
	// farther from its weapon is no rack's.
	inline constexpr float RACK_SPOT = 8.0f;
	inline constexpr float RACK_NEAR = 160.0f;

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

	// Where the rack that holds a_entry's weapon stands in its owner's game (in the weapon's space).
	inline Spot RackSpotOf(const RackWeapon& a_entry)
	{
		return { a_entry.item.cell, a_entry.item.worldspace, a_entry.rackX, a_entry.rackY, a_entry.rackZ };
	}

	// A rack our game has: its base and where it stands.
	struct Rack
	{
		std::uint32_t base = 0;
		Spot          at;
	};

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

	// The rack that holds a_entry's weapon in its owner's game stands here too: one of a_racks (the racks our game has by
	// that rack's place) is of its base, within RACK_SPOT of its place.
	inline bool RackThere(const RackWeapon& a_entry, std::span<const Rack> a_racks)
	{
		const auto theirs = RackSpotOf(a_entry);
		if (a_entry.rackBase == 0 || Distance3(SpotOf(a_entry), theirs) > RACK_NEAR) {
			return false;
		}
		return std::ranges::any_of(a_racks, [&](const Rack& a_rack) {
			return a_rack.base == a_entry.rackBase && SameSpace(theirs, a_rack.at) && Distance3(theirs, a_rack.at) <= RACK_SPOT;
		});
	}

	// What became of a copy that left the world past its blocked activation (an inventory took it): taken back out of the
	// inventory that holds it, when that is known (a persistent reference names it; our player's one pickup of that weapon
	// within reach this lately names theirs); else it can't be found, and both games have the weapon (logged as a
	// duplicate). The pairing is tight, so it never takes back our player's own gun of that kind looted beside a copy an
	// NPC took: the copy is found gone within half a second of the pickup (RackDisplays looks every 500 ms; the window
	// leaves room for slow frames), and two pickups of that kind in it are two guns we can't tell apart.
	inline constexpr float PICKUP_PAIRING_SECONDS = 2.0f;
	inline constexpr float PICKUP_REACH = 300.0f;  // our player this near the copy: an activation's reach, up to a rack's top slot
	enum class Holder : std::uint8_t
	{
		kNamed,    // the engine names the inventory that took it
		kPlayer,   // our player picked up one weapon of its base, beside it, just now
		kUnknown,  // nobody we can name
	};
	// a_pickups: our player's pickups of the copy's base within the window, beside it; a_secondsSince: the last one's age.
	inline Holder HolderOf(bool a_named, float a_secondsSince, int a_pickups)
	{
		if (a_named) {
			return Holder::kNamed;
		}
		return a_pickups == 1 && a_secondsSince >= 0.0f && a_secondsSince <= PICKUP_PAIRING_SECONDS ? Holder::kPlayer : Holder::kUnknown;
	}

	// Our player stands where a pickup of a copy could have been theirs: in its space, within PICKUP_REACH of it.
	inline bool InReach(const Spot& a_player, const Spot& a_copy)
	{
		return SameSpace(a_player, a_copy) && Distance3(a_player, a_copy) <= PICKUP_REACH;
	}

	// Weapons our player took from the world lately, by base, for HolderOf. A pickup is used up when it pairs with a copy
	// (exactly one of that base in the window), so it never stands for two; with two in the window neither is used, as
	// neither pairs. Kept KEPT at most, MAX of them (the oldest goes first).
	class PickupLog
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr std::size_t          MAX = 16;
		static constexpr std::chrono::seconds KEPT{ 30 };

		void Add(std::uint32_t a_base, Clock::time_point a_at)
		{
			if (pickups.size() >= MAX) {
				pickups.erase(pickups.begin());
			}
			pickups.emplace_back(a_base, a_at);
		}

		// Our player's pickups of a_base within PICKUP_PAIRING_SECONDS of a_now: how many, and how long ago the last was
		// (-1: none). The one that pairs is used up.
		struct Recent
		{
			int   count = 0;
			float since = -1.0f;
		};
		Recent Pair(std::uint32_t a_base, Clock::time_point a_now)
		{
			std::erase_if(pickups, [&](const auto& a_pickup) { return a_now - a_pickup.second > KEPT; });
			Recent out;
			auto   last = pickups.end();
			for (auto it = pickups.begin(); it != pickups.end(); ++it) {
				// A pickup the frame saw after a_now was taken (its event comes on another thread) is just now.
				const float since = std::max(0.0f, std::chrono::duration<float>(a_now - it->second).count());
				if (it->first == a_base && since <= PICKUP_PAIRING_SECONDS) {
					++out.count;
					out.since = since;
					last = it;
				}
			}
			if (out.count == 1) {
				pickups.erase(last);
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return pickups.size(); }

		void Clear() { pickups.clear(); }

	private:
		std::vector<std::pair<std::uint32_t, Clock::time_point>> pickups;
	};

	// The same weapon shown the same way: its item, its place and angles within MOVED and TURNED, and on the same rack
	// within MOVED of its place.
	inline bool Same(const RackWeapon& a_a, const RackWeapon& a_b)
	{
		const auto turned = [](float a_x, float a_y) { return std::fabs(a_x - a_y) > TURNED; };
		const auto a = SpotOf(a_a);
		const auto b = SpotOf(a_b);
		return a_a.key == a_b.key && a_a.item.base == a_b.item.base && a_a.item.templateIndex == a_b.item.templateIndex && a_a.item.mods == a_b.item.mods &&
		       a_a.item.name == a_b.item.name && SameSpace(a, b) && Distance3(a, b) <= MOVED && !turned(a_a.angleX, a_b.angleX) && !turned(a_a.angleY, a_b.angleY) &&
		       !turned(a_a.angleZ, a_b.angleZ) && a_a.rackBase == a_b.rackBase && Distance3(RackSpotOf(a_a), RackSpotOf(a_b)) <= MOVED;
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

	// A friend's weapon with no rack of ours by it is looked at again after NO_RACK_WAIT (its cell may still be loading). A
	// reference found to be no rack is believed for NOT_A_RACK_FOR only, less than that, so each look asks again: the
	// shared workshop's copy of a friend's rack, just placed, or a reference of a cell still attaching, may not have its
	// script bound yet. A rack found stays one for the load.
	inline constexpr std::chrono::milliseconds NO_RACK_WAIT{ 5000 };
	inline constexpr std::chrono::milliseconds NOT_A_RACK_FOR{ 4000 };
	static_assert(NOT_A_RACK_FOR < NO_RACK_WAIT);

	// What our game found of references it looked at for the rack script, by RackKey.
	class RackBook
	{
	public:
		using Clock = std::chrono::steady_clock;

		// Answers kept: past it, the noes that ran out go (a yes is never dropped for room).
		static constexpr std::size_t MAX_ANSWERS = 4096;

		// A reference's id and its base: an id the game gives another reference after a scrap isn't the rack it was.
		static std::uint64_t Key(std::uint32_t a_refId, std::uint32_t a_baseId) { return (std::uint64_t{ a_refId } << 32) | a_baseId; }

		// The answer that still holds: a yes, or a no not yet run out. None: ask the VM.
		[[nodiscard]] std::optional<bool> Find(std::uint64_t a_key, Clock::time_point a_now) const
		{
			const auto it = answers.find(a_key);
			if (it == answers.end() || (!it->second.rack && a_now >= it->second.until)) {
				return std::nullopt;
			}
			return it->second.rack;
		}

		// What the VM said. True: a rack not known as one before (said once a load).
		bool Set(std::uint64_t a_key, bool a_rack, Clock::time_point a_now)
		{
			if (answers.size() >= MAX_ANSWERS && !answers.contains(a_key)) {
				std::erase_if(answers, [&](const auto& a_entry) { return !a_entry.second.rack && a_now >= a_entry.second.until; });
			}
			auto&      answer = answers[a_key];
			const bool found = a_rack && !answer.rack;
			answer = { a_rack, a_rack ? Clock::time_point::max() : a_now + NOT_A_RACK_FOR };
			return found;
		}

		[[nodiscard]] std::size_t Size() const { return answers.size(); }

		void Clear() { answers.clear(); }

	private:
		struct Answer
		{
			bool              rack = false;
			Clock::time_point until{};
		};

		std::unordered_map<std::uint64_t, Answer> answers;
	};

	// A friend's new list: the keys of theirs whose weapon is new, changed or gone since a_last. Only those are looked at
	// afresh (one that couldn't be shown, or had no rack here): the same list said again every 30 s leaves the rest as they
	// were, and their lines in the log said once.
	inline std::vector<std::uint32_t> Changed(std::span<const RackWeapon> a_last, std::span<const RackWeapon> a_now)
	{
		std::vector<std::uint32_t> out;
		for (const auto& entry : a_now) {
			const auto it = std::ranges::find(a_last, entry.key, &RackWeapon::key);
			if (it == a_last.end() || !Same(*it, entry)) {
				out.push_back(entry.key);
			}
		}
		for (const auto& entry : a_last) {
			if (std::ranges::find(a_now, entry.key, &RackWeapon::key) == a_now.end()) {
				out.push_back(entry.key);
			}
		}
		return out;
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
