#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// What our player's gear report (Equipment::Read on our player, sent to friends) says they hold in their hands.
// Kept free of the game so it can be tested.
// The report used to list every item of which the inventory has a stack flagged equipped. Those are the inventory's
// flags, not what the engine holds: with two copies of one gun (a plain 10mm and a modded one), switching to another gun
// could leave a stale flag on a 10mm stack, and the report named the 10mm and the new gun at once for the rest of the
// session. A stand-in holds one gun, so each friend's game found it "not wearing their gear" every few seconds and
// dressed it again, putting its gun away each time: for about 9 s after every gear change, their shots showed no trail.
// Now a hand weapon the engine's own equipped list (the actor's middle-high process) does not hold is left out when it
// holds another flagged one: the stale flag. Anything else is left as the inventory says, so nothing is lost when the
// two disagree for another reason (a switch in flight, a list that can't be read).
namespace GearReport
{
	// a_items: the base forms the inventory has flagged equipped (armor and weapons), sorted. a_held: the base forms of
	// the weapons the engine's equipped list holds, or nullopt when there is none to read. a_inHand(item): a weapon held in
	// the hand (not a grenade or mine, which the inventory counts as equipped while one is carried and which the hand
	// weapons never displace; nor armor). Returns a_items with the stale hand weapons left out: those the engine doesn't
	// hold, when it holds another of a_items' hand weapons. Armor and throwables are never touched.
	template <class InHand>
	std::vector<std::uint32_t> HandsAsHeld(std::vector<std::uint32_t> a_items, const std::optional<std::vector<std::uint32_t>>& a_held, InHand&& a_inHand)
	{
		if (!a_held) {
			return a_items;
		}
		const auto held = [&](std::uint32_t a_item) { return std::ranges::find(*a_held, a_item) != a_held->end(); };
		const bool holdsOne = std::ranges::any_of(a_items, [&](std::uint32_t a_item) { return a_inHand(a_item) && held(a_item); });
		if (!holdsOne) {
			return a_items;  // the engine holds none of them: unarmed with a stale flag, or a switch in flight
		}
		std::erase_if(a_items, [&](std::uint32_t a_item) { return a_inHand(a_item) && !held(a_item); });
		return a_items;
	}

	// Which of an item's stacks flagged equipped the report reads the mods of: the one whose instance is the one the
	// engine holds (two copies of a gun, a stale flag on the other), else the first, as before. a_flagged: the stacks'
	// instance data, in the inventory's order (nullptr for a stack without one); a_held: the engine's, or nullptr.
	// Only compared, never read. Returns an index into a_flagged (0 when it is empty).
	inline std::size_t StackToRead(const std::vector<const void*>& a_flagged, const void* a_held)
	{
		if (a_held) {
			for (std::size_t i = 0; i < a_flagged.size(); ++i) {
				if (a_flagged[i] == a_held) {
					return i;
				}
			}
		}
		return 0;
	}
}
