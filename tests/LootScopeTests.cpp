// Which loot is the session's and which stays in each player's game (LootScope), the rule alone.

#include "Test.h"

#include "game/LootScope.h"

#include <array>

using namespace LootScope;

namespace
{
	constexpr std::array BOTH{ Direction::kTake, Direction::kPut };

	constexpr HolderFacts CHEST{ .kind = Holder::kContainer };
	constexpr HolderFacts CORPSE{ .kind = Holder::kActor, .alive = false };
	constexpr HolderFacts LIVING{ .kind = Holder::kActor, .alive = true };
	constexpr HolderFacts KILL_ON_ITS_WAY{ .kind = Holder::kActor, .alive = true, .knownDead = true };
	constexpr HolderFacts WORKBENCH{ .kind = Holder::kContainer, .workshop = true };
	constexpr HolderFacts FRAME{ .kind = Holder::kFurniture };
	constexpr HolderFacts VENDOR_CHEST{ .kind = Holder::kContainer, .vendorChest = true };

	constexpr ItemFacts AMMO{ .kind = Item::kAmmo };
	constexpr ItemFacts JUNK{ .kind = Item::kMisc };
	constexpr ItemFacts STIMPAK{ .kind = Item::kAid };

	bool Every(const HolderFacts& a_holder, const ItemFacts& a_item, Scope a_scope)
	{
		bool all = true;
		for (const auto direction : BOTH) {
			all = all && Of(a_holder, a_item, direction) == a_scope;
		}
		return all;
	}
}

TEST("loot scope: ammo, chems, food and junk in a chest or on a corpse are everyone's, both ways")
{
	for (const auto& holder : { CHEST, CORPSE }) {
		CHECK(Every(holder, AMMO, Scope::kShared));
		CHECK(Every(holder, STIMPAK, Scope::kShared));
		CHECK(Every(holder, JUNK, Scope::kShared));
		CHECK(Every(holder, ItemFacts{ .kind = Item::kComponent }, Scope::kShared));
		CHECK(Every(holder, ItemFacts{ .kind = Item::kIngredient }, Scope::kShared));
	}
}

TEST("loot scope: keys, holotapes, paper notes, bobbleheads and magazines are each player's own")
{
	for (const auto& holder : { CHEST, CORPSE }) {
		CHECK(Every(holder, ItemFacts{ .kind = Item::kKey }, Scope::kPersonal));
		CHECK(Every(holder, ItemFacts{ .kind = Item::kNote }, Scope::kPersonal));
		CHECK(Every(holder, ItemFacts{ .kind = Item::kBook }, Scope::kPersonal));  // a paper note, no keyword needed
		CHECK(Every(holder, ItemFacts{ .kind = Item::kMisc, .featured = true }, Scope::kPersonal));
	}
}

TEST("loot scope: a stack a quest holds stays when taken, but putting the same item in is shared")
{
	const ItemFacts questStack{ .kind = Item::kMisc, .questAlias = true };
	CHECK(Of(CHEST, questStack, Direction::kTake) == Scope::kPersonal);
	CHECK(Of(CHEST, questStack, Direction::kPut) == Scope::kShared);
}

TEST("loot scope: weapons and armor never travel, modded, legendary or plain")
{
	// Only the base form crosses: a friend would get a plain copy, or lose their legendary to a take.
	for (const auto& holder : { CHEST, CORPSE }) {
		CHECK(Every(holder, ItemFacts{ .kind = Item::kWeapon }, Scope::kWeaponArmor));
		CHECK(Every(holder, ItemFacts{ .kind = Item::kArmor }, Scope::kWeaponArmor));
	}
}

TEST("loot scope: a workbench, a power armor frame and a vendor's chest are each player's, whatever the item")
{
	for (const auto& item : { AMMO, JUNK, ItemFacts{ .kind = Item::kWeapon }, ItemFacts{ .kind = Item::kKey } }) {
		CHECK(Every(WORKBENCH, item, Scope::kPerPlayer));
		CHECK(Every(FRAME, item, Scope::kPerPlayer));
		CHECK(Every(VENDOR_CHEST, item, Scope::kVendor));
	}
}

TEST("loot scope: a living NPC's inventory waits until it is dead: pickpocketing, a companion's trade, or a kill still on its way")
{
	CHECK(Every(LIVING, AMMO, Scope::kNotYetDead));
	CHECK(Every(LIVING, STIMPAK, Scope::kNotYetDead));
	// The session says it died; our game hasn't killed it yet: its loot is the corpse's.
	CHECK(Every(KILL_ON_ITS_WAY, AMMO, Scope::kShared));
	// What is never shared stays so on a living NPC too.
	CHECK(Every(LIVING, ItemFacts{ .kind = Item::kWeapon }, Scope::kWeaponArmor));
	CHECK(Every(LIVING, ItemFacts{ .kind = Item::kKey }, Scope::kPersonal));
}

TEST("loot scope: the player, a friend's stand-in, anything made at runtime and things that aren't items are left alone")
{
	for (const auto kind : { Holder::kPlayer, Holder::kRuntime, Holder::kOther }) {
		CHECK(Every(HolderFacts{ .kind = kind }, AMMO, Scope::kUnsupported));
	}
	CHECK(Every(CHEST, ItemFacts{ .kind = Item::kOther }, Scope::kUnsupported));
	CHECK(Every(WORKBENCH, ItemFacts{ .kind = Item::kOther }, Scope::kUnsupported));
}

TEST("loot scope: a loose item picked up is gone for everyone, unless everyone collects their own")
{
	CHECK(OfPickup(AMMO) == Scope::kShared);
	CHECK(OfPickup(ItemFacts{ .kind = Item::kWeapon }) == Scope::kShared);  // the same placed reference in every game
	CHECK(OfPickup(ItemFacts{ .kind = Item::kBook }) == Scope::kPersonal);
	CHECK(OfPickup(ItemFacts{ .kind = Item::kNote }) == Scope::kPersonal);
	CHECK(OfPickup(ItemFacts{ .kind = Item::kMisc, .featured = true }) == Scope::kPersonal);
	CHECK(OfPickup(ItemFacts{ .kind = Item::kMisc, .questAlias = true }) == Scope::kPersonal);
	CHECK(OfPickup(ItemFacts{ .kind = Item::kOther }) == Scope::kUnsupported);
}
