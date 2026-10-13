#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Which item moves between the player and a container (or an NPC) are the session's, and which stay in
// the game they were made in. One rule for both ends: the sender reports only what is shared, and the
// receiver applies only what is shared, so what a joiner reports anyway (a build from before the rule,
// or a changed one) stays out.
// Kept free of the game so it can be tested; WorldSync reads the facts from the engine.
//
// Per player: keys, paper notes, holotapes, bobbleheads, magazines, all weapons and armor, vendors and
// caps, companions and settlers (any living NPC), settlement workbench storage, power armor frames.
// Shared: ammo, chems, food, junk and other loot in containers and on corpses.
namespace LootScope
{
	// What kind of thing moved (its base form's type).
	enum class Item : std::uint8_t
	{
		kOther,  // not something an inventory holds
		kKey,
		kNote,   // holotapes
		kBook,   // paper notes and magazines
		kWeapon,
		kArmor,  // clothes and power armor pieces too
		kMisc,
		kAmmo,
		kAid,    // chems, food, drink
		kIngredient,
		kComponent,
	};

	struct ItemFacts
	{
		Item kind = Item::kOther;
		bool featured = false;    // the FeaturedItem keyword: bobbleheads and magazines
		bool questAlias = false;  // the holder's stack of it fills a quest alias (only known for a take, and only on the receiver)
	};

	// What the item moved into or out of.
	enum class Holder : std::uint8_t
	{
		kOther,      // a reference that holds nothing we know how to share
		kPlayer,     // the player, or a friend's stand-in
		kRuntime,    // made by this game at runtime (FF......): no other game has it
		kContainer,  // a chest, a safe, a workbench
		kFurniture,  // a power armor frame
		kActor,      // an NPC, alive or a corpse
	};

	struct HolderFacts
	{
		Holder kind = Holder::kOther;
		bool   workshop = false;     // a settlement's workbench (its reference carries the workshop data)
		bool   vendorChest = false;  // a faction's merchant container: a vendor's stock and caps
		bool   alive = false;        // an actor the engine says is alive
		bool   knownDead = false;    // an actor the session says is dead (its kill may still be on its way here)
	};

	enum class Direction : std::uint8_t
	{
		kTake,  // from the holder into the player's inventory
		kPut,   // from the player's inventory into the holder
	};

	enum class Scope : std::uint8_t
	{
		kShared,       // everyone's: reported, and applied in every game
		kPersonal,     // everyone takes their own: keys, notes, bobbleheads, quest items
		kPerPlayer,    // the holder is each player's own: workbench storage, power armor frames
		kWeaponArmor,  // only the base form travels, so another game would get a plain copy (or lose its legendary)
		kVendor,       // trading: vendors and caps are each player's
		kUnsupported,  // nothing sensible to do: the player, a stand-in, something made at runtime, not an item
		kNotYetDead,   // a living NPC: pickpocketing or trading with a companion on the sender; a corpse whose kill hasn't happened yet on a receiver
	};

	constexpr bool IsInventory(Item a_item)
	{
		return a_item != Item::kOther;
	}

	// Whether every player collects their own: taking one leaves everyone else's.
	constexpr bool IsPersonal(const ItemFacts& a_item, Direction a_direction)
	{
		switch (a_item.kind) {
		case Item::kKey:
		case Item::kNote:
		case Item::kBook:
			return true;
		default:
			return a_item.featured || (a_direction == Direction::kTake && a_item.questAlias);
		}
	}

	// A move between the player and a holder, from either end.
	constexpr Scope Of(const HolderFacts& a_holder, const ItemFacts& a_item, Direction a_direction)
	{
		switch (a_holder.kind) {
		case Holder::kContainer:
		case Holder::kFurniture:
		case Holder::kActor:
			break;
		default:
			return Scope::kUnsupported;
		}
		if (!IsInventory(a_item.kind)) {
			return Scope::kUnsupported;
		}
		if (a_holder.kind == Holder::kFurniture || a_holder.workshop) {
			return Scope::kPerPlayer;
		}
		if (a_holder.vendorChest) {
			return Scope::kVendor;
		}
		if (IsPersonal(a_item, a_direction)) {
			return Scope::kPersonal;
		}
		if (a_item.kind == Item::kWeapon || a_item.kind == Item::kArmor) {
			return Scope::kWeaponArmor;
		}
		if (a_holder.kind == Holder::kActor && a_holder.alive && !a_holder.knownDead) {
			return Scope::kNotYetDead;
		}
		return Scope::kShared;
	}

	// A loose item picked up from the world. Each game's copy of a placed item is that same reference, so a
	// loose weapon is taken from the friend's world as it is (only moves between inventories lose what a
	// weapon is); what everyone collects for themselves stays.
	constexpr Scope OfPickup(const ItemFacts& a_item)
	{
		if (!IsInventory(a_item.kind)) {
			return Scope::kUnsupported;
		}
		return IsPersonal(a_item, Direction::kTake) ? Scope::kPersonal : Scope::kShared;
	}

	constexpr std::string_view Name(Scope a_scope)
	{
		switch (a_scope) {
		case Scope::kShared:
			return "shared";
		case Scope::kPersonal:
			return "personal";
		case Scope::kPerPlayer:
			return "perPlayer";
		case Scope::kWeaponArmor:
			return "weaponArmor";
		case Scope::kVendor:
			return "vendor";
		case Scope::kUnsupported:
			return "unsupported";
		case Scope::kNotYetDead:
			return "notYetDead";
		}
		return "?";
	}

	inline constexpr std::size_t SCOPES = 7;
}
