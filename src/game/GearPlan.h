#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// What a mirrored NPC (one another player's game runs) is told to put on so it looks like its owner's
// copy. Each game rolls NPC gear on its own, so the two copies wear different things. The mirror is a real
// NPC of this game (a settler, a companion, a raider whose corpse will be looted), and whatever is done to
// it stays in this game's save: so it is only ever dressed from its own inventory, by subtraction.
// Nothing is given to it (a report used to add the owner's items: farmable copies of power armor pieces
// and uniques, kept in the save) and nothing it wears is taken off (the settler the host dressed in combat
// armor lost it to the joiner's roll, for good). Pure, so it can be tested without the game.
namespace GearPlan
{
	// The slots an item fills, as a mask: an armor's biped slots in the low 32 bits, and one bit of its own
	// for a weapon held in the hands and one for a grenade or mine. Two items whose masks overlap cannot be
	// worn together: equipping one takes the other off.
	using Slots = std::uint64_t;
	constexpr Slots WEAPON_SLOT = Slots{ 1 } << 32;
	constexpr Slots THROWABLE_SLOT = Slots{ 1 } << 33;

	// The items to equip on a mirror, in a_wanted's order. a_worn: what it wears now; a_wanted: what the
	// owner's copy wears. a_has(item): it carries one (an armor or a weapon of this game); a_isPowerArmorPiece
	// (item): a piece belongs to a frame and is never equipped on a body; a_slots(item): see Slots.
	// - An actor in a power armor frame is left alone: what it wears is the frame's.
	// - Only items it carries and does not wear yet.
	// - Never one that would take off something it wears that the owner's copy does not (that is its own, or
	//   given to it in this game): where the two copies differ, the mirror keeps its own.
	template <class Has, class IsPowerArmorPiece, class SlotsOf>
	std::vector<std::uint32_t> Equip(const std::vector<std::uint32_t>& a_worn, const std::vector<std::uint32_t>& a_wanted, bool a_inFrame, Has a_has,
		IsPowerArmorPiece a_isPowerArmorPiece, SlotsOf a_slots)
	{
		std::vector<std::uint32_t> equip;
		if (a_inFrame) {
			return equip;
		}
		Slots kept = 0;  // filled by what it wears that the owner's copy does not
		for (const auto worn : a_worn) {
			if (!std::ranges::contains(a_wanted, worn)) {
				kept |= a_slots(worn);
			}
		}
		for (const auto item : a_wanted) {
			if (std::ranges::contains(a_worn, item) || std::ranges::contains(equip, item) || a_isPowerArmorPiece(item) || !a_has(item) || (a_slots(item) & kept) != 0) {
				continue;
			}
			equip.push_back(item);
		}
		return equip;
	}
}
