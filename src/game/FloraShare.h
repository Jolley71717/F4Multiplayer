#pragma once

#include <cstdint>

// Plants (the game's flora: tatos, mutfruit, carrots, hubflower, fungus) picked by a player are picked in every game.
// Picking one gives its ingredient straight to the player and marks the plant harvested until it grows back; no item
// moves between references, so neither the loot nor the pickup report saw it, and a friend still found the plant full.
//
// The game's harvest event names the plant and who picked it: our player's pick of a plant every game has goes to the
// server as a pickup of that plant (kReportPickup, no new message), which keeps it with the session's pickups and gives
// it to every player, joiners and loads too. A receiver marks its plant harvested (the console's SetHarvested): it
// gets no ingredient, and the plant grows back on its own clock. A build from before this takes the pickup of a plant
// for something not an item and leaves it, as before. The server keeps a pickup once a session, so a plant picked
// again after it grew back is picked only in that game.
// Kept free of the game so it can be tested; WorldSync reads the facts from the engine.
namespace FloraShare
{
	// Whether a harvest in our game is reported: our player picked it, and every game has the plant (Protocol::IsShareableRef:
	// not one a settlement grew at runtime, which only its builder's game has by that id).
	constexpr bool Reports(bool a_byOurPlayer, bool a_shareable)
	{
		return a_byOurPlayer && a_shareable;
	}

	// What a friend's pickup does to a reference in our world.
	enum class Pickup : std::uint8_t
	{
		kItem,      // not a plant: a loose item, taken out of the world as before
		kHarvest,   // a plant still full here: marked harvested
		kHarvested, // a plant already picked here: nothing to do
	};

	constexpr Pickup OnPickup(bool a_flora, bool a_harvestedHere)
	{
		if (!a_flora) {
			return Pickup::kItem;
		}
		return a_harvestedHere ? Pickup::kHarvested : Pickup::kHarvest;
	}

	// TESForm::formFlags bit the engine sets on a harvested plant's reference (its record's "harvested" flag).
	inline constexpr std::uint32_t HARVESTED_FLAG = 1u << 13;

	constexpr bool Harvested(std::uint32_t a_formFlags)
	{
		return (a_formFlags & HARVESTED_FLAG) != 0;
	}
}
