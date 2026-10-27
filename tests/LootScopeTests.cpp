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

TEST("loot scope: where a holder's rule and an item's meet, the holder's wins")
{
	// A vendor's chest is a trade whatever it holds: a weapon in it is the vendor's stock, not a weapon
	// left out of the session, and a quest item put in is sold.
	CHECK(Every(VENDOR_CHEST, ItemFacts{ .kind = Item::kWeapon }, Scope::kVendor));
	CHECK(Of(VENDOR_CHEST, ItemFacts{ .kind = Item::kMisc, .questAlias = true }, Direction::kPut) == Scope::kVendor);
	// A settlement's workbench is each player's, quest stack or bobblehead.
	CHECK(Of(WORKBENCH, ItemFacts{ .kind = Item::kMisc, .questAlias = true }, Direction::kTake) == Scope::kPerPlayer);
	CHECK(Every(WORKBENCH, ItemFacts{ .kind = Item::kMisc, .featured = true }, Scope::kPerPlayer));
	CHECK(Every(FRAME, ItemFacts{ .kind = Item::kKey }, Scope::kPerPlayer));
	// On a living NPC what is personal stays personal (it never waits for the NPC to die): a key a
	// friend pickpocketed is still in our NPC's pocket for us.
	CHECK(Every(LIVING, ItemFacts{ .kind = Item::kKey }, Scope::kPersonal));
	CHECK(Of(LIVING, ItemFacts{ .kind = Item::kAid, .questAlias = true }, Direction::kTake) == Scope::kPersonal);
	// A corpse whose kill hasn't reached us holds what it held: a quest item on it is still personal.
	CHECK(Of(KILL_ON_ITS_WAY, ItemFacts{ .kind = Item::kMisc, .questAlias = true }, Direction::kTake) == Scope::kPersonal);
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
		a_game.items[PIPE_PISTOL] = { .kind = Item::kWeapon };
		a_game.items[LEATHER_ARM] = { .kind = Item::kArmor };
		a_game.items[AMMO_38] = { .kind = Item::kAmmo };
		a_game.items[STEEL] = { .kind = Item::kComponent };
		a_game.items[CAPS] = { .kind = Item::kMisc };
		a_game.items[PAPER_NOTE] = { .kind = Item::kBook };
		a_game.items[CORE] = { .kind = Item::kAmmo };
		a_game.holderFacts[WORKBENCH_REF] = { .kind = Holder::kContainer, .workshop = true };
		a_game.holderFacts[VENDOR_REF] = { .kind = Holder::kContainer, .vendorChest = true };
		a_game.holderFacts[FRAME_REF] = { .kind = Holder::kFurniture };
		a_game.holderFacts[RAIDER] = { .kind = Holder::kActor, .alive = true };
		a_game.holderFacts[SETTLER] = { .kind = Holder::kActor, .alive = true };
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
	CHECK(alice.loot.Stats().sent == 1);
	CHECK(alice.KeptHere() == 2);
	UncheckedMove(f, carol, CHEST_REF, PIPE_PISTOL, 1);  // a legendary pistol, reported anyway
	bob.Receive(f);

	CHECK(bob.In(CHEST_REF, AMMO_38) == 50);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 0);
	CHECK(bob.In(CHEST_REF, LEATHER_ARM) == 0);
	CHECK(bob.LeftOut() == 1);

	// Taking one out is not shared either: the friend's copy stays where it is.
	bob.holders[CHEST_REF][PIPE_PISTOL] = 1;  // the chest's own pistol, in every game
	alice.holders[CHEST_REF][PIPE_PISTOL] += 1;
	alice.Take(f, CHEST_REF, PIPE_PISTOL, 2);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 1);
	CHECK(alice.loot.Stats().sent == 1);
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
	CHECK(alice.loot.Stats().sent == 0);
	UncheckedMove(f, carol, WORKBENCH_REF, STEEL, 200);
	UncheckedMove(f, carol, VENDOR_REF, CAPS, -500);
	UncheckedMove(f, carol, FRAME_REF, CORE, -1);
	bob.Receive(f);

	CHECK(bob.In(WORKBENCH_REF, STEEL) == 0);
	CHECK(bob.In(VENDOR_REF, CAPS) == 500);  // Bob's vendor still has the caps to pay him
	CHECK(bob.In(FRAME_REF, CORE) == 1);     // and his frame its core
	CHECK(bob.LeftOut() == 3);
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
	alice.Kill(f, RAIDER);
	alice.Take(f, RAIDER, AMMO_38, 20);
	alice.Take(f, RAIDER, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().sent == 1);  // the gun is hers alone
	// Alice pickpockets the settler: not reported.
	alice.Take(f, SETTLER, CAPS, 30);
	CHECK(alice.loot.Stats().sent == 1);
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
	CHECK(bob.loot.Waiting().size() == 1);
	// Should the settler die, the caps had gone with the pickpocket: the corpse has none.
	f.fake->Deliver(carol.peer, DeathOf(SETTLER));
	bob.Receive(f);
	CHECK(bob.loot.Waiting().empty());
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
	CHECK(alice.loot.Stats().sent == 0);
	UncheckedMove(f, carol, CHEST_REF, PAPER_NOTE, -1);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, PAPER_NOTE) == 1);
	CHECK(alice.Carried(PAPER_NOTE) == 1);
}

// ---- emptied: each game rolls a chest's and an NPC's leveled loot on its own -------------------------

namespace
{
	constexpr std::uint32_t STIMPAK_ID = 0x0004A007;
	constexpr std::uint32_t CHEST_2 = 0x0002C002;

	std::vector<std::uint8_t> EmptiedOf(std::uint32_t a_holder)
	{
		return Protocol::Encode(Protocol::ContainerChange{ a_holder, Protocol::CONTAINER_EMPTIED, -1 }, TestServer::MT::kReportContainer);
	}
}

TEST("loot sync: a chest emptied of the session's things is emptied for the friend too, whatever their game rolled; a weapon, a note and a quest's stack stay")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->items[STIMPAK_ID] = { .kind = Item::kAid };
		game->Join(f, "P");
	}
	// Two rolls of the same chest.
	alice.holders[CHEST_REF] = { { AMMO_38, 20 }, { STEEL, 3 }, { PIPE_PISTOL, 1 }, { PAPER_NOTE, 1 } };
	bob.holders[CHEST_REF] = { { AMMO_38, 7 }, { CAPS, 12 }, { STIMPAK_ID, 2 }, { PIPE_PISTOL, 1 }, { PAPER_NOTE, 1 }, { CORE, 1 } };
	bob.questStacks.insert({ CHEST_REF, CORE });

	alice.Take(f, CHEST_REF, AMMO_38, 20);
	CHECK(alice.loot.Stats().emptiedSent == 0);  // the steel is still there
	alice.Take(f, CHEST_REF, STEEL, 3);
	CHECK(alice.loot.Stats().sent == 2);
	CHECK(alice.loot.Stats().emptiedSent == 1);  // only the gun and the note are left, hers alone
	bob.Receive(f);

	CHECK(bob.In(CHEST_REF, AMMO_38) == 0);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);
	CHECK(bob.In(CHEST_REF, STIMPAK_ID) == 0);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 1);
	CHECK(bob.In(CHEST_REF, PAPER_NOTE) == 1);
	CHECK(bob.In(CHEST_REF, CORE) == 1);
	CHECK(bob.loot.Stats().emptiedApplied == 1);
	CHECK(bob.loot.Stats().emptiedStacks == 2);  // the caps and the stimpaks (the ammo went with Alice's take)

	// Alice's own game doesn't get its emptying back.
	alice.Receive(f);
	CHECK(alice.applied == 0);
	CHECK(alice.loot.Stats().emptiedApplied == 0);
}

TEST("loot sync: taking only a weapon and a note from a chest that holds nothing else the session shares empties the friend's copy too")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->Join(f, "P");
	}
	// Alice's roll held no junk at all; Bob's rolled some.
	alice.holders[CHEST_REF] = { { PIPE_PISTOL, 1 }, { PAPER_NOTE, 1 } };
	bob.holders[CHEST_REF] = { { PIPE_PISTOL, 1 }, { PAPER_NOTE, 1 }, { STEEL, 5 }, { AMMO_38, 3 } };

	alice.Take(f, CHEST_REF, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 1);  // the note left is hers alone: nothing the session shares is left
	CHECK(alice.loot.Stats().sent == 0);          // the gun is not the session's
	alice.Take(f, CHEST_REF, PAPER_NOTE, 1);
	CHECK(alice.loot.Stats().sent == 0);          // nor is the note
	CHECK(alice.loot.Stats().emptiedSent == 1);  // told once: not again for each thing of hers taken after
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, STEEL) == 0);
	CHECK(bob.In(CHEST_REF, AMMO_38) == 0);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 1);  // his own gun and note stay
	CHECK(bob.In(CHEST_REF, PAPER_NOTE) == 1);
	CHECK(bob.loot.Stats().emptiedApplied == 1);

	// A chest that still holds something the session shares isn't bare: nothing goes out.
	alice.holders[CHEST_2] = { { PIPE_PISTOL, 1 }, { STEEL, 4 } };
	bob.holders[CHEST_2] = { { PIPE_PISTOL, 1 }, { STEEL, 4 } };
	alice.Take(f, CHEST_2, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 1);
	bob.Receive(f);
	CHECK(bob.In(CHEST_2, STEEL) == 4);
}

TEST("loot sync: a corpse looted bare by the joiner is bare for the host, both ways; a partial loot sends no emptying")
{
	Fixture f;
	Game    host{ 1, 0xA11CE }, joiner{ 2, 0xB0B };
	for (auto* game : { &host, &joiner }) {
		Describe(*game);
		game->items[STIMPAK_ID] = { .kind = Item::kAid };
		game->Join(f, "P");
	}
	host.holders[RAIDER] = { { AMMO_38, 5 }, { STIMPAK_ID, 2 }, { PIPE_PISTOL, 1 }, { LEATHER_ARM, 1 } };
	joiner.holders[RAIDER] = { { AMMO_38, 14 }, { CAPS, 9 }, { PIPE_PISTOL, 1 } };
	host.holders[SETTLER] = { { CAPS, 30 }, { STEEL, 2 } };
	joiner.holders[SETTLER] = { { CAPS, 4 }, { STIMPAK_ID, 1 } };

	// The joiner kills the raider and takes both stacks it sees in one frame: the corpse goes out emptied.
	joiner.Kill(f, RAIDER);
	joiner.Take(f, RAIDER, AMMO_38, 14, false);
	joiner.Take(f, RAIDER, CAPS, 9, false);
	joiner.Frame(f);
	CHECK(joiner.loot.Stats().emptiedSent == 1);
	host.Receive(f);
	CHECK(host.In(RAIDER, AMMO_38) == 0);
	CHECK(host.In(RAIDER, STIMPAK_ID) == 0);
	CHECK(host.In(RAIDER, PIPE_PISTOL) == 1);
	CHECK(host.In(RAIDER, LEATHER_ARM) == 1);

	// The host kills the settler and takes only the caps: the steel is still there, nothing more goes out.
	host.Kill(f, SETTLER);
	host.Take(f, SETTLER, CAPS, 30);
	CHECK(host.loot.Stats().emptiedSent == 0);
	joiner.Receive(f);
	CHECK(joiner.In(SETTLER, CAPS) == 0);
	CHECK(joiner.In(SETTLER, STIMPAK_ID) == 1);
	// Then the steel: now it is bare, and so is the joiner's.
	host.Take(f, SETTLER, STEEL, 2);
	CHECK(host.loot.Stats().emptiedSent == 1);
	joiner.Receive(f);
	CHECK(joiner.In(SETTLER, STIMPAK_ID) == 0);
	CHECK(joiner.loot.Stats().emptiedApplied == 1);
}

TEST("loot sync: an emptying waits for a holder the friend's game hasn't stocked yet, and for an NPC still alive there; a vendor's chest and a workbench keep theirs")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
	}
	// A chest Bob's game has never loaded: its loot is rolled when it first loads, and the emptying waits for that.
	bob.unstocked.insert(CHEST_2);
	alice.holders[CHEST_2] = { { AMMO_38, 10 } };
	alice.Take(f, CHEST_2, AMMO_38, 10);
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 2);  // the take, and the emptying behind it
	bob.unstocked.erase(CHEST_2);
	bob.holders[CHEST_2] = { { CAPS, 25 }, { PIPE_PISTOL, 1 } };
	bob.Wait(f, std::chrono::seconds{ 1 });
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(CHEST_2, CAPS) == 0);
	CHECK(bob.In(CHEST_2, PIPE_PISTOL) == 1);

	// An emptying of a living NPC (a changed build's) waits for its death.
	bob.holders[SETTLER] = { { CAPS, 30 } };
	f.fake->Deliver(carol.peer, EmptiedOf(SETTLER));
	bob.Receive(f);
	CHECK(bob.In(SETTLER, CAPS) == 30);
	CHECK(bob.loot.Waiting().size() == 1);
	f.fake->Deliver(carol.peer, DeathOf(SETTLER));
	bob.Receive(f);
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(SETTLER, CAPS) == 0);

	// A vendor's chest and workbench storage are each player's: nothing leaves them.
	bob.holders[VENDOR_REF] = { { CAPS, 500 } };
	bob.holders[WORKBENCH_REF] = { { STEEL, 40 } };
	f.fake->Deliver(carol.peer, EmptiedOf(VENDOR_REF));
	f.fake->Deliver(carol.peer, EmptiedOf(WORKBENCH_REF));
	bob.Receive(f);
	CHECK(bob.In(VENDOR_REF, CAPS) == 500);
	CHECK(bob.In(WORKBENCH_REF, STEEL) == 40);
	CHECK(bob.LeftOut() == 2);
}

TEST("loot sync: a take from a chest the friend's game holds without an inventory yet waits for its roll, through a save and a reload, both ways")
{
	constexpr std::uint32_t CHEST_3 = 0x0002C005;
	constexpr std::uint32_t CHEST_4 = 0x0002C006;
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->Join(f, "P");
	}

	// Alice loots part of a chest in a place Bob isn't: his game holds the chest in memory, with no inventory made yet.
	alice.holders[CHEST_2] = { { AMMO_38, 10 }, { STEEL, 2 } };
	bob.unstocked.insert(CHEST_2);
	alice.Take(f, CHEST_2, AMMO_38, 10);
	CHECK(alice.loot.Stats().emptiedSent == 0);  // the steel is still there: only the take goes out
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 1);
	CHECK(bob.applied == 0);

	// Bob saves and reloads before going there: the save's record keeps it waiting, and the catch-up doesn't bring it twice.
	const auto saved = bob.SaveGame(f);
	CHECK(saved.waiting.size() == 1);
	bob.LoadGame(f, saved);
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 1);
	CHECK(bob.applied == 0);

	// He walks there: his game makes the chest's inventory with its own roll, and the take goes in then.
	bob.unstocked.erase(CHEST_2);
	bob.holders[CHEST_2] = { { AMMO_38, 6 }, { CAPS, 3 } };
	bob.Wait(f, std::chrono::seconds{ 1 });
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(CHEST_2, AMMO_38) == 0);  // what Alice took, from his copy too
	CHECK(bob.In(CHEST_2, CAPS) == 3);     // a partial loot leaves the rest of his roll

	// A chest not in Bob's memory at all waits for its place, then for its inventory as the place loads.
	alice.holders[CHEST_REF] = { { AMMO_38, 5 }, { STEEL, 1 } };
	bob.away.insert(CHEST_REF);
	alice.Take(f, CHEST_REF, AMMO_38, 5);
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 1);
	bob.away.erase(CHEST_REF);
	bob.unstocked.insert(CHEST_REF);
	bob.Wait(f, std::chrono::seconds{ 1 });
	CHECK(bob.loot.Waiting().size() == 1);
	bob.unstocked.erase(CHEST_REF);
	bob.holders[CHEST_REF] = { { AMMO_38, 9 } };
	bob.Wait(f, std::chrono::seconds{ 1 });
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(CHEST_REF, AMMO_38) == 4);

	// The other way: Bob loots a chest bare that Alice's game holds without an inventory. The take and the emptying wait, and
	// go in, in order, once her game has rolled it.
	bob.holders[CHEST_3] = { { STEEL, 4 } };
	alice.unstocked.insert(CHEST_3);
	bob.Take(f, CHEST_3, STEEL, 4);
	CHECK(bob.loot.Stats().emptiedSent == 1);
	alice.Receive(f);
	CHECK(alice.loot.Waiting().size() == 2);
	alice.unstocked.erase(CHEST_3);
	alice.holders[CHEST_3] = { { STEEL, 1 }, { AMMO_38, 8 }, { PIPE_PISTOL, 1 } };
	alice.Wait(f, std::chrono::seconds{ 1 });
	CHECK(alice.loot.Waiting().empty());
	CHECK(alice.In(CHEST_3, STEEL) == 0);
	CHECK(alice.In(CHEST_3, AMMO_38) == 0);
	CHECK(alice.In(CHEST_3, PIPE_PISTOL) == 1);

	// A put goes in at once, as before, and a take the session doesn't share is left out at once, not held.
	bob.unstocked.insert(CHEST_4);
	bob.unstocked.insert(VENDOR_REF);
	alice.Put(f, CHEST_4, AMMO_38, 3);
	UncheckedMove(f, carol, VENDOR_REF, CAPS, -500);
	const auto leftOut = bob.LeftOut();
	bob.Receive(f);
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(CHEST_4, AMMO_38) == 3);
	CHECK(bob.LeftOut() == leftOut + 1);
}

TEST("loot sync: a friend who reloads a save from before the emptying, or joins after it, finds the chest bare; what was put back after it is there")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->Join(f, "P");
	}
	alice.holders[CHEST_REF] = { { AMMO_38, 20 } };
	bob.holders[CHEST_REF] = { { CAPS, 12 }, { STEEL, 4 } };
	const auto before = bob.SaveGame(f);

	alice.Take(f, CHEST_REF, AMMO_38, 20);
	alice.Put(f, CHEST_REF, AMMO_38, 5);  // she leaves some back
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);
	CHECK(bob.In(CHEST_REF, AMMO_38) == 5);

	bob.LoadGame(f, before);
	CHECK(bob.In(CHEST_REF, CAPS) == 12);  // the old save's chest
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);
	CHECK(bob.In(CHEST_REF, STEEL) == 0);
	CHECK(bob.In(CHEST_REF, AMMO_38) == 5);

	// A third game joining later, with its own roll.
	Game carol{ 3, 0xCA401 };
	Describe(carol);
	carol.holders[CHEST_REF] = { { CAPS, 3 }, { STEEL, 1 } };
	carol.Join(f, "P");
	carol.Receive(f);
	CHECK(carol.In(CHEST_REF, CAPS) == 0);
	CHECK(carol.In(CHEST_REF, STEEL) == 0);
	CHECK(carol.In(CHEST_REF, AMMO_38) == 5);
}

TEST("loot scope: a chest that respawns and an NPC hold a roll of loot; a container that keeps things, a workbench and a frame don't")
{
	CHECK(Rolled(CHEST));
	CHECK(Rolled(CORPSE));
	CHECK(!Rolled(HolderFacts{ .kind = Holder::kContainer, .storage = true }));
	CHECK(!Rolled(FRAME));
	CHECK(!Rolled(HolderFacts{ .kind = Holder::kPlayer }));
	// Storage changes nothing about what a take or a put shares.
	CHECK(Every(HolderFacts{ .kind = Holder::kContainer, .storage = true }, JUNK, Scope::kShared));
}

TEST("loot scope: storage is what the player made their own: owned, a settlement's workshop, or in a settlement they own")
{
	CHECK(IsStorage({ .playerOwned = true }));        // a home bought (its cell is PlayerFaction's), or a container they own
	CHECK(IsStorage({ .workshopItem = true }));       // a workbench, or a chest built there
	CHECK(IsStorage({ .inOwnedSettlement = true }));  // a Sanctuary house's dresser once Sanctuary is theirs
	// A dungeon's one-off loot that never respawns (DN035_EasyCityDownsWallSafe), and a settlement's dresser before the
	// settlement is anyone's, are a roll of loot: neither says storage on its own.
	CHECK(!IsStorage({}));
}

TEST("loot sync: a friend's stash in the same home container stays when the other player takes its only item, or only a gun, or an older build sends an emptying")
{
	constexpr std::uint32_t HOME_CHEST = 0x0002C003;
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		Describe(*game);
		game->holderFacts[HOME_CHEST] = { .kind = Holder::kContainer, .storage = true };  // a Sanctuary house's dresser, Sanctuary theirs
		game->Join(f, "P");
	}
	// Each player keeps their own things in it, in their own world.
	alice.holders[HOME_CHEST] = { { STEEL, 1 } };
	bob.holders[HOME_CHEST] = { { CAPS, 50 }, { AMMO_38, 30 }, { STEEL, 2 }, { PIPE_PISTOL, 1 } };

	alice.Take(f, HOME_CHEST, STEEL, 1);
	CHECK(alice.loot.Stats().sent == 1);
	CHECK(alice.loot.Stats().emptiedSent == 0);  // hers is bare, but it keeps things: Bob's may hold his stash
	bob.Receive(f);
	CHECK(bob.In(HOME_CHEST, STEEL) == 1);  // the one she took
	CHECK(bob.In(HOME_CHEST, CAPS) == 50);
	CHECK(bob.In(HOME_CHEST, AMMO_38) == 30);

	// Only a gun from it: nothing goes out at all.
	alice.holders[HOME_CHEST] = { { PIPE_PISTOL, 1 } };
	alice.Take(f, HOME_CHEST, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 0);

	// A build from before this sends the emptying anyway: Bob's game leaves his stash.
	const auto leftOut = bob.LeftOut();
	f.fake->Deliver(carol.peer, EmptiedOf(HOME_CHEST));
	bob.Receive(f);
	CHECK(bob.In(HOME_CHEST, CAPS) == 50);
	CHECK(bob.In(HOME_CHEST, AMMO_38) == 30);
	CHECK(bob.In(HOME_CHEST, STEEL) == 1);
	CHECK(bob.LeftOut() == leftOut + 1);
	CHECK(bob.loot.Stats().emptiedApplied == 0);
}

TEST("loot sync: what a friend puts in a chest just as another player takes it bare stays in both games, whichever the server orders first")
{
	constexpr std::uint32_t JET = 0x0004A008;
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->items[JET] = { .kind = Item::kAid };
		game->Join(f, "P");
	}
	// Bob's put reaches the server first; Alice hadn't seen it when she took her copy bare.
	alice.holders[CHEST_REF] = { { AMMO_38, 20 } };
	bob.holders[CHEST_REF] = { { CAPS, 12 } };
	bob.carried[JET] = 1;
	bob.Put(f, CHEST_REF, JET, 1);
	alice.Take(f, CHEST_REF, AMMO_38, 20);
	CHECK(alice.loot.Stats().emptiedSent == 1);
	bob.Receive(f);
	alice.Receive(f);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);  // his own roll goes
	CHECK(bob.In(CHEST_REF, JET) == 1);   // his put stays
	CHECK(alice.In(CHEST_REF, JET) == 1);
	CHECK(bob.loot.Stats().emptiedKept == 1);

	// Alice's emptying reaches the server first; Bob put his in before it came.
	alice.holders[CHEST_2] = { { STEEL, 3 } };
	bob.holders[CHEST_2] = { { CAPS, 7 } };
	bob.carried[JET] = 1;
	alice.Take(f, CHEST_2, STEEL, 3);
	bob.Put(f, CHEST_2, JET, 1);
	bob.Receive(f);
	alice.Receive(f);
	CHECK(bob.In(CHEST_2, CAPS) == 0);
	CHECK(bob.In(CHEST_2, JET) == 1);
	CHECK(alice.In(CHEST_2, JET) == 1);

	// Once taken, what was put in is the friend's to lose like anything else: Alice takes the jet, and hers is bare again.
	alice.Take(f, CHEST_2, JET, 1);
	CHECK(alice.loot.Stats().emptiedSent == 3);
	bob.Receive(f);
	CHECK(bob.In(CHEST_2, JET) == 0);
}

TEST("loot sync: a friend's take of the chest's own roll doesn't use up what another friend put in the same moment, of the same item")
{
	constexpr std::uint32_t STIM = 0x0004A009;
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->items[STIM] = { .kind = Item::kAid };
		game->Join(f, "P");
	}
	// A raider's chest: each game rolled two stimpaks in it. Bob puts three of his in just as Alice takes her copy's two
	// and leaves it bare, before his put has reached her.
	alice.holders[CHEST_REF] = { { STIM, 2 } };
	bob.holders[CHEST_REF] = { { STIM, 2 }, { CAPS, 9 } };
	bob.carried[STIM] = 3;
	bob.Put(f, CHEST_REF, STIM, 3);
	alice.Take(f, CHEST_REF, STIM, 2);
	CHECK(alice.loot.Stats().emptiedSent == 1);
	bob.Receive(f);
	alice.Receive(f);
	// Her take is taken from his copy's own two first; his three stay, as they are in hers.
	CHECK(bob.In(CHEST_REF, STIM) == 3);
	CHECK(alice.In(CHEST_REF, STIM) == 3);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);
	CHECK(bob.loot.Stats().emptiedKept == 3);
}

TEST("loot sync: a chest told bare whose loot respawns is told again when only a gun is taken from it; a gun put back and taken again is not")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	for (auto* game : { &alice, &bob }) {
		Describe(*game);
		game->Join(f, "P");
	}
	alice.holders[CHEST_REF] = { { PIPE_PISTOL, 1 } };
	bob.holders[CHEST_REF] = { { PIPE_PISTOL, 1 }, { STEEL, 5 } };
	alice.Take(f, CHEST_REF, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 1);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, STEEL) == 0);

	// She puts the gun back and takes it again: nothing new in it, nothing new goes out.
	alice.Put(f, CHEST_REF, PIPE_PISTOL, 1);
	alice.Take(f, CHEST_REF, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 1);

	// Days later the cell resets: each game rolls the chest again.
	alice.holders[CHEST_REF] = { { PIPE_PISTOL, 1 } };
	bob.holders[CHEST_REF] = { { PIPE_PISTOL, 1 }, { STEEL, 2 }, { CAPS, 9 } };
	alice.Take(f, CHEST_REF, PIPE_PISTOL, 1);
	CHECK(alice.loot.Stats().emptiedSent == 2);
	bob.Receive(f);
	CHECK(bob.In(CHEST_REF, STEEL) == 0);
	CHECK(bob.In(CHEST_REF, CAPS) == 0);
	CHECK(bob.In(CHEST_REF, PIPE_PISTOL) == 1);
}
