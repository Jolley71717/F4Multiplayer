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

// ---- both ends, three games and a real server ---------------------------------------------------------

#include "LootGame.h"

namespace
{
	using LootTest::Game;
	using TestServer::Fixture;

	constexpr std::uint32_t CHEST_REF = 0x0002C001;
	constexpr std::uint32_t WORKBENCH_REF = 0x0002C010;
	constexpr std::uint32_t VENDOR_REF = 0x0002C020;
	constexpr std::uint32_t FRAME_REF = 0x0002C030;
	constexpr std::uint32_t RAIDER = 0x0001F001;
	constexpr std::uint32_t SETTLER = 0x0001F002;
	constexpr std::uint32_t PIPE_PISTOL = 0x0004A001;
	constexpr std::uint32_t LEATHER_ARM = 0x0004A002;
	constexpr std::uint32_t AMMO_38 = 0x0004A003;
	constexpr std::uint32_t STEEL = 0x0004A004;
	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t PAPER_NOTE = 0x0004A005;
	constexpr std::uint32_t CORE = 0x0004A006;

	// The same game data in every game: what the forms are.
	void Describe(Game& a_game)
	{
		auto& w = a_game.world;
		w.items[PIPE_PISTOL] = { .kind = Item::kWeapon };
		w.items[LEATHER_ARM] = { .kind = Item::kArmor };
		w.items[AMMO_38] = { .kind = Item::kAmmo };
		w.items[STEEL] = { .kind = Item::kComponent };
		w.items[CAPS] = { .kind = Item::kMisc };
		w.items[PAPER_NOTE] = { .kind = Item::kBook };
		w.items[CORE] = { .kind = Item::kAmmo };
		w.holders[WORKBENCH_REF] = { .kind = Holder::kContainer, .workshop = true };
		w.holders[VENDOR_REF] = { .kind = Holder::kContainer, .vendorChest = true };
		w.holders[FRAME_REF] = { .kind = Holder::kFurniture };
		w.holders[RAIDER] = { .kind = Holder::kActor, .alive = true };
		w.holders[SETTLER] = { .kind = Holder::kActor, .alive = true };
	}

	// A joiner that reports every move it makes (a build from before the rule, or one changed to): the move is made in its own world too.
	void UncheckedMove(Fixture& a_f, Game& a_game, std::uint32_t a_holder, std::uint32_t a_item, std::int32_t a_count)
	{
		a_game.holders[a_holder][a_item] += a_count;
		a_game.SendRaw(a_f, a_holder, a_item, a_count);
	}

	std::vector<std::uint8_t> DeathOf(std::uint32_t a_actor)
	{
		return Protocol::Encode(Protocol::ActorDeath{ a_actor, 0, true }, TestServer::MT::kReportDeath);
	}
}

TEST("loot sync: a weapon or armor put in a shared chest stays out of a friend's chest, even from a joiner that reports it, while ammo beside it gets there")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
	}
	alice.carried = { { PIPE_PISTOL, 1 }, { LEATHER_ARM, 1 }, { AMMO_38, 50 } };
	alice.Put(f, CHEST_REF, PIPE_PISTOL, 1);
	alice.Put(f, CHEST_REF, LEATHER_ARM, 1);
	alice.Put(f, CHEST_REF, AMMO_38, 50);
	CHECK(alice.sent == 1);
	CHECK(alice.notShared == 2);
	UncheckedMove(f, carol, CHEST_REF, PIPE_PISTOL, 1);  // a legendary pistol, reported anyway
	bob.Receive(f);

	CHECK(bob.In(CHEST_REF, AMMO_38) == 50);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 0);
	CHECK(bob.In(CHEST_REF, LEATHER_ARM) == 0);
	CHECK(bob.notShared == 1);

	// Taking one out is not shared either: the friend's copy stays where it is.
	bob.holders[CHEST_REF][PIPE_PISTOL] = 1;  // the chest's own pistol, in every game
	alice.holders[CHEST_REF][PIPE_PISTOL] += 1;
	alice.Take(f, CHEST_REF, PIPE_PISTOL, 2);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 1);
	CHECK(alice.sent == 1);
}

TEST("loot sync: workbench storage, a vendor's chest and a power armor frame stay each player's, even when a joiner reports the moves")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
		game->holders[VENDOR_REF] = { { CAPS, 500 } };
		game->holders[FRAME_REF] = { { CORE, 1 } };
	}
	alice.carried = { { STEEL, 200 }, { CAPS, 100 } };
	alice.Put(f, WORKBENCH_REF, STEEL, 200);
	alice.Put(f, VENDOR_REF, CAPS, 100);
	alice.Take(f, FRAME_REF, CORE, 1);
	CHECK(alice.sent == 0);
	UncheckedMove(f, carol, WORKBENCH_REF, STEEL, 200);
	UncheckedMove(f, carol, VENDOR_REF, CAPS, -500);
	UncheckedMove(f, carol, FRAME_REF, CORE, -1);
	bob.Receive(f);

	CHECK(bob.In(WORKBENCH_REF, STEEL) == 0);
	CHECK(bob.In(VENDOR_REF, CAPS) == 500);  // Bob's vendor still has the caps to pay him
	CHECK(bob.In(FRAME_REF, CORE) == 1);     // and his frame its core
	CHECK(bob.notShared == 3);
}

TEST("loot sync: a corpse looted after its kill is looted for the friend too, though their game hasn't killed it yet; a living NPC's pockets aren't")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
		game->holders[RAIDER] = { { AMMO_38, 20 }, { PIPE_PISTOL, 1 } };
		game->holders[SETTLER] = { { CAPS, 30 } };
	}

	// Alice kills the raider; her game reports the death, then her looting.
	alice.world.holders[RAIDER].alive = false;
	f.fake->Deliver(alice.peer, DeathOf(RAIDER));
	alice.Take(f, RAIDER, AMMO_38, 20);
	alice.Take(f, RAIDER, PIPE_PISTOL, 1);
	CHECK(alice.sent == 1);  // the gun is hers alone
	// Alice pickpockets the settler: not reported.
	alice.Take(f, SETTLER, CAPS, 30);
	CHECK(alice.sent == 1);
	bob.Receive(f);

	// Bob's raider still stands (not loaded here, its kill waits), but the session says it died.
	CHECK(bob.dead.contains(RAIDER));
	CHECK(bob.In(RAIDER, AMMO_38) == 0);
	CHECK(bob.In(RAIDER, PIPE_PISTOL) == 1);
	CHECK(bob.In(SETTLER, CAPS) == 30);

	// A pickpocketing reported anyway reaches Bob's game, and waits: the settler is alive there.
	UncheckedMove(f, carol, SETTLER, CAPS, -30);
	bob.Receive(f);
	CHECK(bob.In(SETTLER, CAPS) == 30);
	CHECK(bob.waiting.size() == 1);
	// Should the settler die, the caps had gone with the pickpocket: the corpse has none.
	f.fake->Deliver(carol.peer, DeathOf(SETTLER));
	bob.Receive(f);
	CHECK(bob.waiting.empty());
	CHECK(bob.In(SETTLER, CAPS) == 0);
}

TEST("loot sync: a paper note taken from a chest stays in the friend's chest for them to read")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
		game->holders[CHEST_REF] = { { PAPER_NOTE, 1 } };
	}
	alice.Take(f, CHEST_REF, PAPER_NOTE, 1);
	CHECK(alice.sent == 0);
	UncheckedMove(f, carol, CHEST_REF, PAPER_NOTE, -1);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, PAPER_NOTE) == 1);
	CHECK(alice.Carried(PAPER_NOTE) == 1);
}
