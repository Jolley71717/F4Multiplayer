// Giving weapons and armor through a container (the host's bContainerGifts), against a real server: GiftGame.h plays
// each game with the plugin's own GiftBook::Client and DropBook::Record in it. Every test counts the items of a class
// across the session's worlds and what lies in the session, with every menu closed and every packet read: whatever is
// raced, reloaded, cut off, late, forged or replayed, an item given is never made twice and never lost, but for the one
// case the install guide names (a giver's save from before the put, loaded after a friend took it).

#include "GiftGame.h"

#include "SessionFile.h"

#include <cstring>
#include <fstream>

using namespace GiftTest;
using namespace std::chrono_literals;

namespace
{
	using GiftBook::ClassOf;
	using GiftBook::Notice;

	constexpr std::uint64_t ALICE_ID = 0x0000'A11C'E000'0001;
	constexpr std::uint64_t BOB_ID = 0x0000'B0B0'0000'0002;
	constexpr std::uint64_t CAROL_ID = 0x0000'C0C0'0000'0003;

	Server::Options Gifts(bool a_on = true)
	{
		Server::Options options;
		options.rules.containerGifts = a_on;
		return options;
	}

	// Every game reads what the server sent it, a few times over, so what one sends reaches the others and back.
	void SettleAll(Fixture& a_f, std::initializer_list<Game*> a_games)
	{
		for (int round = 0; round < 3; ++round) {
			for (auto* game : a_games) {
				game->Receive(a_f);
			}
		}
	}

	// The items of a_class in every world, and lying in the session as a_seenBy heard (every menu closed: no copy counted).
	std::int64_t Total(const GiftBook::Class& a_class, std::initializer_list<const Game*> a_games, const Game& a_seenBy)
	{
		std::int64_t n = a_seenBy.Lying(a_class);
		for (const auto* game : a_games) {
			n += game->Holds(a_class);
		}
		return n;
	}

	// Alice's gun put in the chest and given: it lies in the session (Alice's menu closed again). Its drop ID.
	std::uint32_t GiveGun(Fixture& a_f, Game& a_alice, std::initializer_list<Game*> a_all, const GiftBook::Stack& a_gun = Gun())
	{
		a_alice.Give(PLAYER, a_gun);
		a_alice.Open(a_f, CHEST);
		REQUIRE(a_alice.Put(a_f, CHEST, ClassOf(a_gun)));
		a_alice.Receive(a_f);  // the offer: the gun is her copy of the gift now
		a_alice.Close(a_f);
		SettleAll(a_f, a_all);
		std::uint32_t id = 0;
		for (const auto& [dropId, drop] : a_alice.gifts.Lying()) {
			if (GiftBook::ClassOf(StackOf(drop)) == ClassOf(a_gun) && dropId > id) {
				id = dropId;
			}
		}
		REQUIRE(id != 0);
		return id;
	}

	// a_game opens the chest, takes the gift's copy and reads the answer; the menu closes again.
	void TakeGift(Fixture& a_f, Game& a_game, const GiftBook::Class& a_class, std::initializer_list<Game*> a_all)
	{
		a_game.Open(a_f, CHEST);
		REQUIRE(a_game.gifts.Copies() >= 1);
		REQUIRE(a_game.Take(a_f, CHEST, a_class));
		SettleAll(a_f, a_all);
		a_game.Close(a_f);
	}

	void Send(Fixture& a_f, const Game& a_game, std::vector<std::uint8_t> a_packet)
	{
		a_f.fake->Deliver(a_game.peer, std::move(a_packet));
	}

	std::vector<std::uint8_t> Claim(std::uint32_t a_dropId, Protocol::DropClaim a_op)
	{
		return Protocol::Encode(Protocol::ClaimDrop{ a_dropId, a_op });
	}
}

// ---- give and take ---------------------------------------------------------------------------------------------

TEST("gifts: a gun put in a chest lies there for a friend, who takes it once, and the giver's chest shows it go")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	const auto aliceId = alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	CHECK(alice.rule);
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);

	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	CHECK(alice.gifts.Puts() == 1);
	CHECK(bob.Lying(gun) == 0);
	alice.Receive(f);  // the offer: her gun stays on screen as her copy of the gift
	CHECK(alice.In(CHEST, gun) == 1);
	CHECK(alice.gifts.Copies() == 1);
	CHECK(alice.Noticed(Notice::kGiven) == 1);
	SettleAll(f, { &alice, &bob });
	REQUIRE(bob.Lying(gun) == 1);
	const auto drop = bob.gifts.Lying().begin()->second;
	CHECK(drop.ownerId == aliceId);
	CHECK(drop.holder == CHEST);
	CHECK(drop.name == "Old Reliable");
	CHECK(drop.mods == Gun().mods);

	// Bob's copy of the chest shows it; he takes it while Alice still looks.
	bob.Open(f, CHEST);
	CHECK(bob.In(CHEST, gun) == 1);
	REQUIRE(bob.Take(f, CHEST, gun));
	CHECK(bob.Carried(gun) == 0);  // held back until the server says
	CHECK(bob.gifts.Claiming(drop.dropId));
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	CHECK(bob.addsToPlayer == 1);
	CHECK(bob.gifts.Counters().received == 1);
	CHECK(alice.In(CHEST, gun) == 0);  // gone from her chest as she watched
	CHECK(alice.gifts.Copies() == 0);
	CHECK(alice.Lying(gun) == 0);
	bob.Close(f);
	alice.Close(f);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);

	// Neither a second take nor a second look makes another.
	bob.Open(f, CHEST);
	CHECK(bob.gifts.Copies() == 0);
	bob.Close(f);
	SettleAll(f, { &alice, &bob });
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

TEST("gifts: the giver sees their own gift in the chest and can take it back, through the server like anyone")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	CHECK(alice.Holds(gun) == 0);  // out of her world: it lies in the session
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);

	alice.Open(f, CHEST);
	CHECK(alice.gifts.Shows(id));
	REQUIRE(alice.Take(f, CHEST, gun));
	SettleAll(f, { &alice, &bob });
	alice.Close(f);
	CHECK(alice.Carried(gun) == 1);
	CHECK(bob.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);

	// Taken back before the menu closed: her copy is the gift, and her take of it is a claim like Bob's would be.
	const auto again = GiveGun(f, alice, { &alice, &bob });
	alice.Open(f, CHEST);
	REQUIRE(alice.Take(f, CHEST, gun));
	SettleAll(f, { &alice, &bob });
	alice.Close(f);
	CHECK(alice.Carried(gun) == 2);  // (GiveGun gave her another)
	CHECK(!bob.gifts.Lying().contains(again));
	CHECK(Total(gun, { &alice, &bob }, bob) == 2);
}

TEST("gifts: an item taken back before its offer came was never given, and one whose menu closed first leaves the chest")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	REQUIRE(alice.Take(f, CHEST, gun));  // straight back, before the server answered
	SettleAll(f, { &alice, &bob });
	alice.Close(f);
	CHECK(alice.Carried(gun) == 1);
	CHECK(alice.gifts.Counters().kept == 1);
	CHECK(bob.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	CHECK(alice.Noticed(Notice::kGiven) == 0);

	// Put and the menu closed before the offer: the offer takes it out of the chest, and it lies in the session.
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	CHECK(alice.gifts.Puts() == 1);
	alice.Close(f);
	CHECK(alice.In(CHEST, gun) == 1);  // until the offer
	SettleAll(f, { &alice, &bob });
	CHECK(alice.In(CHEST, gun) == 0);
	CHECK(bob.Lying(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

TEST("gifts: two friends taking one gift at once: the server's first gets it, the other's take is undone with a notice")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	Game       carol{ 3, CAROL_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	carol.Join(f, "Carol");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob, &carol });
	bob.Open(f, CHEST);
	carol.Open(f, CHEST);
	REQUIRE(bob.gifts.Shows(id));
	REQUIRE(carol.gifts.Shows(id));
	REQUIRE(bob.Take(f, CHEST, gun));
	REQUIRE(carol.Take(f, CHEST, gun));
	CHECK(bob.Carried(gun) + carol.Carried(gun) == 0);
	SettleAll(f, { &alice, &bob, &carol });
	bob.Close(f);
	carol.Close(f);
	CHECK(bob.Carried(gun) == 1);
	CHECK(carol.Carried(gun) == 0);
	CHECK(carol.Noticed(Notice::kTakenFirst) == 1);
	CHECK(carol.gifts.Counters().takenFirst == 1);
	CHECK(carol.gifts.Claims() == 0);
	CHECK(carol.addsToPlayer == 0);
	CHECK(Total(gun, { &alice, &bob, &carol }, carol) == 1);
}

// ---- reloads ---------------------------------------------------------------------------------------------------

TEST("gifts: a taker's save from before the take puts the gift back in the chest for anyone; one from after keeps it")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	const auto before = bob.SaveGame(f);
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	const auto after = bob.SaveGame(f);

	bob.LoadGame(f, before);
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Carried(gun) == 0);
	CHECK(bob.gifts.Lying().contains(id));
	CHECK(alice.gifts.Lying().contains(id));
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);

	// Taken again, then the save from after the first take: the record says Bob's world holds the take it made then.
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	bob.LoadGame(f, after);
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	CHECK(!alice.gifts.Lying().contains(id));
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
	CHECK(!f.Logged("someone else holds too"));
}

TEST("gifts: a giver's save from before the put takes the gift back out of the session")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	const auto before = alice.SaveGame(f);
	alice.inventories.clear();  // (GiveGun gives her the gun again)
	const auto id = GiveGun(f, alice, { &alice, &bob });
	const auto after = alice.SaveGame(f);
	bob.Open(f, CHEST);  // Bob watches the chest
	REQUIRE(bob.gifts.Shows(id));

	alice.LoadGame(f, before);
	SettleAll(f, { &alice, &bob });
	CHECK(alice.Carried(gun) == 1);
	CHECK(!bob.gifts.Lying().contains(id));
	CHECK(bob.In(CHEST, gun) == 0);  // his copy left his chest as he watched
	bob.Close(f);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);

	// Her save from after the put: the gun left her world in it, so it lies there again.
	alice.LoadGame(f, after);
	SettleAll(f, { &alice, &bob });
	CHECK(alice.Holds(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

// The case that can't be put right, as for a dropped item: behind the host's setting, and in the install guide.
TEST("gifts: a giver's save from before the put loaded after a friend took it: both have it, and the server says so")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	const auto before = alice.SaveGame(f);
	alice.inventories.clear();
	GiveGun(f, alice, { &alice, &bob });
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	alice.LoadGame(f, before);
	SettleAll(f, { &alice, &bob });
	CHECK(alice.Carried(gun) == 1);
	CHECK(bob.Carried(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, bob) == 2);
	CHECK(f.Logged("someone else holds too"));
}

// ---- links that go ---------------------------------------------------------------------------------------------

TEST("gifts: a take whose grant was lost with the link lies there again once the taker is back")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	bob.Open(f, CHEST);
	REQUIRE(bob.Take(f, CHEST, gun));  // the claim goes; the grant never reaches Bob's game
	bob.Disconnect(f);
	CHECK(bob.Holds(gun) == 0);  // nothing given back by the link going
	SettleAll(f, { &alice });
	CHECK(!alice.gifts.Lying().contains(id));  // granted to Bob, as the server saw it
	bob.Join(f, "Bob");
	SettleAll(f, { &alice, &bob });
	CHECK(bob.gifts.Lying().contains(id));
	CHECK(alice.gifts.Lying().contains(id));
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
}

TEST("gifts: a take whose claim never went still lies there, and the taker's copy leaves with the link")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	bob.Open(f, CHEST);
	REQUIRE(bob.Take(f, CHEST, gun, false));  // no frame before the link goes
	bob.Disconnect(f);
	CHECK(bob.Holds(gun) == 0);
	CHECK(bob.In(CHEST, gun) == 0);
	SettleAll(f, { &alice });
	CHECK(alice.gifts.Lying().contains(id));
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
	bob.Join(f, "Bob");
	SettleAll(f, { &alice, &bob });
	CHECK(bob.gifts.Lying().contains(id));
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
}

TEST("gifts: a giver's kPlaced lost with the link: their record says it left their world, and it lies there")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	alice.Handle(f, alice.Fetch(f), false);  // the offer: the gun leaves her world, and her kPlaced waits for the frame
	alice.Disconnect(f);
	CHECK(alice.Holds(gun) == 0);
	SettleAll(f, { &bob });
	CHECK(bob.Lying(gun) == 0);  // only an offer: nobody can take it
	CHECK(Total(gun, { &alice, &bob }, bob) == 0);  // nowhere until her record (it waits on the server, saved)
	alice.Join(f, "Alice");
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Lying(gun) == 1);
	CHECK(alice.Lying(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

TEST("gifts: a giver's offer lost with the link: the item stays in their chest, and the offer goes on their record")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	alice.Disconnect(f);  // before the offer came
	CHECK(alice.In(CHEST, gun) == 1);
	alice.Join(f, "Alice");
	SettleAll(f, { &alice, &bob });
	CHECK(alice.In(CHEST, gun) == 1);
	CHECK(bob.Lying(gun) == 0);
	CHECK(alice.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	// Nothing of it is left on the server: Bob can't take it, and it isn't saved.
	CHECK(alice.gifts.Counters().given == 0);
}

// ---- late answers ----------------------------------------------------------------------------------------------

TEST("gifts: a claim asked again before its answer is granted once, and the second answer adds nothing")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	GiveGun(f, alice, { &alice, &bob });
	bob.Open(f, CHEST);
	REQUIRE(bob.Take(f, CHEST, gun));
	bob.Wait(f, GiftBook::CLAIM_RETRY + 1s);  // the answer is late: asked again
	SettleAll(f, { &alice, &bob });
	bob.Close(f);
	CHECK(bob.Carried(gun) == 1);
	CHECK(bob.addsToPlayer == 1);
	CHECK(bob.gifts.Claims() == 0);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
}

TEST("gifts: a grant that comes after the taker loaded a save gives the gift back, and it lies there once")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	const auto before = bob.SaveGame(f);
	bob.Open(f, CHEST);
	REQUIRE(bob.Take(f, CHEST, gun));
	bob.LoadGame(f, before);  // before the grant came
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Holds(gun) == 0);
	CHECK(bob.addsToPlayer == 0);
	CHECK(bob.gifts.Lying().contains(id));
	CHECK(alice.gifts.Lying().contains(id));
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
	// And its giving back (if the server heard it before the record) never lays it twice.
	Send(f, bob, Claim(id, Protocol::DropClaim::kGiveBack));
	SettleAll(f, { &alice, &bob });
	CHECK(alice.Lying(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
}

TEST("gifts: an offer that comes after the giver stopped waiting for it is kept: the item never left their chest")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	alice.Close(f);
	alice.Wait(f, GiftBook::PUT_FORGET + 1s);
	CHECK(alice.gifts.Puts() == 0);
	SettleAll(f, { &alice, &bob });
	CHECK(alice.gifts.Counters().lateOffers == 1);
	CHECK(alice.In(CHEST, gun) == 1);
	CHECK(bob.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
	// The server forgot the offer: a later record of hers changes nothing.
	alice.LoadGame(f, alice.SaveGame(f));
	SettleAll(f, { &alice, &bob });
	CHECK(bob.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

// ---- forged and replayed messages ------------------------------------------------------------------------------

TEST("gifts: the server acts only on a player's own moves, once each: others' confirms, keeps and give-backs do nothing")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	Game       carol{ 3, CAROL_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	carol.Join(f, "Carol");
	const auto gun = ClassOf(Gun());

	// Alice's offer: Bob can't say it lies there, nor that it was kept.
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	const auto offers = Decoded(alice.Fetch(f), MT::kDropPlaced, [](auto a_data) { return Protocol::DecodeDroppedItem(a_data); });
	REQUIRE(offers.size() == 1);
	const auto id = offers[0].dropId;
	Send(f, bob, Claim(id, Protocol::DropClaim::kPlaced));
	Send(f, bob, Claim(id, Protocol::DropClaim::kKept));
	Send(f, bob, Claim(id, Protocol::DropClaim::kTake));
	SettleAll(f, { &bob, &carol });
	CHECK(bob.Lying(gun) == 0);
	CHECK(bob.Holds(gun) == 0);
	alice.Handle(f, { Protocol::Encode(offers[0], MT::kDropPlaced) });  // her game has it still: the offer goes through
	alice.Close(f);
	SettleAll(f, { &alice, &bob, &carol });
	REQUIRE(carol.Lying(gun) == 1);

	// Her report again (the same nonce) is the same gift, and her kPlaced again changes nothing.
	Send(f, alice, Protocol::Encode(offers[0], MT::kReportDrop));
	Send(f, alice, Claim(id, Protocol::DropClaim::kPlaced));
	SettleAll(f, { &alice, &bob, &carol });
	CHECK(carol.Lying(gun) == 1);
	CHECK(alice.gifts.Counters().lateOffers == 0);
	CHECK(Total(gun, { &alice, &bob, &carol }, carol) == 1);

	// Bob takes it. His take again, Carol's take, and Carol's give-back of his take do nothing; nor does Alice's keep.
	TakeGift(f, bob, gun, { &alice, &bob, &carol });
	CHECK(bob.Carried(gun) == 1);
	Send(f, bob, Claim(id, Protocol::DropClaim::kTake));
	Send(f, carol, Claim(id, Protocol::DropClaim::kTake));
	Send(f, carol, Claim(id, Protocol::DropClaim::kGiveBack));
	Send(f, alice, Claim(id, Protocol::DropClaim::kKept));
	Send(f, alice, Claim(id, Protocol::DropClaim::kGiveBack));
	SettleAll(f, { &alice, &bob, &carol });
	CHECK(bob.Carried(gun) == 1);
	CHECK(bob.addsToPlayer == 1);
	CHECK(carol.Holds(gun) == 0);
	CHECK(carol.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob, &carol }, carol) == 1);

	// A give-back is the taker's own move, once: after it the gift lies there, and the same again does nothing more.
	// (His game sends it only when the item couldn't go into his inventory.)
	bob.inventories[PLAYER].clear();
	Send(f, bob, Claim(id, Protocol::DropClaim::kGiveBack));
	SettleAll(f, { &alice, &bob, &carol });
	Send(f, bob, Claim(id, Protocol::DropClaim::kGiveBack));
	SettleAll(f, { &alice, &bob, &carol });
	CHECK(carol.Lying(gun) == 1);
	CHECK(alice.Lying(gun) == 1);
	CHECK(Total(gun, { &alice, &bob, &carol }, carol) == 1);
}

TEST("gifts: without the host's rule a gift is refused, and a game with it off reports none")
{
	Fixture    f{ Gifts(false) };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	CHECK(!alice.rule);
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	CHECK(alice.gifts.Puts() == 0);
	CHECK(alice.gifts.Counters().puts == 1);
	CHECK(alice.gifts.Counters().reported == 0);
	alice.Close(f);
	CHECK(alice.In(CHEST, gun) == 1);  // in her own copy of the chest, as in 0.9.x

	// A modified game's report anyway is refused (no drop ID), and nobody else hears of it.
	auto item = GiftBook::ItemOf(Gun(), CHEST, { .worldspace = 0x0000003C, .x = -79000.0f, .y = 89000.0f, .z = 7800.0f }, 42);
	Send(f, alice, Protocol::Encode(item, MT::kReportDrop));
	const auto answer = Decoded(alice.Fetch(f), MT::kDropPlaced, [](auto a_data) { return Protocol::DecodeDroppedItem(a_data); });
	REQUIRE(answer.size() == 1);
	CHECK(answer[0].dropId == 0);
	CHECK(Count(bob.Fetch(f), MT::kDropPlaced) == 0);

	// Nor is a gift of two decoded, rule or not: a gift is one item.
	item.count = 2;
	CHECK(!Protocol::DecodeDroppedItem(Protocol::Encode(item, MT::kReportDrop)));
}

// A game that took the rule for on (it can't: the welcome says) or a session full: the server keeps nothing, so the item
// stays where it was put, in that player's own copy of the chest.
TEST("gifts: a gift the server refuses stays in the giver's chest, with a notice")
{
	Fixture    f{ Gifts(false) };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	alice.gifts.Welcome(alice.localId, true);
	const auto gun = ClassOf(Gun());
	alice.Give(PLAYER, Gun());
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, gun));
	CHECK(alice.gifts.Counters().reported == 1);
	SettleAll(f, { &alice, &bob });
	alice.Close(f);
	CHECK(alice.gifts.Counters().refused == 1);
	CHECK(alice.gifts.Puts() == 0);
	CHECK(alice.Noticed(Notice::kNotGiven) == 1);
	CHECK(alice.In(CHEST, gun) == 1);
	CHECK(bob.Lying(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, bob) == 1);
}

// ---- the host leaving ------------------------------------------------------------------------------------------

TEST("gifts: what lies in a chest and a giver's offer waiting for its record outlive the host's server stopping")
{
	const TempSessionFile file;
	auto                  options = Gifts();
	options.sessionFile = file.path.string();
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	const auto spare = Gun("Spare");
	{
		Fixture f{ options };
		alice.Join(f, "Alice");
		bob.Join(f, "Bob");
		GiveGun(f, alice, { &alice, &bob });
		// A second gun's offer whose kPlaced never went: the host quit (its server with it).
		alice.Give(PLAYER, spare);
		alice.Open(f, CHEST);
		REQUIRE(alice.Put(f, CHEST, ClassOf(spare)));
		alice.Handle(f, alice.Fetch(f), false);
		alice.LinkLost();
		bob.LinkLost();
	}  // stopping saves
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(saved->drops.size() == 2);
	CHECK(std::ranges::count(saved->drops, Protocol::DROP_OFFERED, &Protocol::DroppedItem::takerId) == 1);
	{
		std::ifstream       in{ file.path, std::ios::binary };
		std::array<char, 6> header{};
		in.read(header.data(), header.size());
		std::uint16_t version = 0;
		std::memcpy(&version, header.data() + 4, sizeof(version));
		CHECK(version == SessionFile::GIFTS);
	}
	const auto gun = ClassOf(Gun());
	const auto spareClass = ClassOf(spare);
	{
		Fixture f{ options };
		alice.Join(f, "Alice");
		bob.Join(f, "Bob");
		SettleAll(f, { &alice, &bob });
		CHECK(bob.Lying(gun) == 1);
		CHECK(bob.Lying(spareClass) == 1);  // her record says it left her world: it lies there now
		CHECK(Total(gun, { &alice, &bob }, bob) == 1);
		CHECK(Total(spareClass, { &alice, &bob }, bob) == 1);
		TakeGift(f, bob, gun, { &alice, &bob });
		TakeGift(f, bob, spareClass, { &alice, &bob });
		CHECK(bob.Carried(gun) == 1);
		CHECK(bob.Carried(spareClass) == 1);
		CHECK(Total(gun, { &alice, &bob }, bob) == 1);
		CHECK(Total(spareClass, { &alice, &bob }, bob) == 1);
	}
}

TEST("gifts: a gift lies there while its giver is away, and their return changes nothing")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto gun = ClassOf(Gun());
	const auto id = GiveGun(f, alice, { &alice, &bob });
	alice.Disconnect(f);
	SettleAll(f, { &bob });
	CHECK(bob.gifts.Lying().contains(id));
	TakeGift(f, bob, gun, { &bob });
	CHECK(bob.Carried(gun) == 1);
	alice.Join(f, "Alice");
	SettleAll(f, { &alice, &bob });
	CHECK(alice.Holds(gun) == 0);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
	CHECK(!f.Logged("someone else holds too"));
}

// ---- what is a gift --------------------------------------------------------------------------------------------

TEST("gifts: a workbench, a merchant's chest and a companion aren't gift places, and a quest item or runtime mod is no gift")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	alice.holderFacts[BENCH] = { .kind = LootScope::Holder::kContainer, .workshop = true };
	alice.holderFacts[VENDOR] = { .kind = LootScope::Holder::kContainer, .vendorChest = true };
	constexpr std::uint32_t COMPANION = 0x0002A1B4;
	alice.holderFacts[COMPANION] = { .kind = LootScope::Holder::kContainer, .companion = true };
	const auto gun = ClassOf(Gun());
	for (const auto holder : { BENCH, VENDOR, COMPANION }) {
		alice.Give(PLAYER, Gun());
		alice.Open(f, holder);
		CHECK(!alice.gifts.Viewing());
		REQUIRE(alice.Put(f, holder, gun));
		alice.Close(f);
		CHECK(alice.In(holder, gun) == 1);
	}
	CHECK(alice.gifts.Counters().reported == 0);

	auto quest = Gun("Kellogg's Pistol");
	quest.quest = true;
	auto runtime = Gun("Odd", { { 0xFF000801, 0, 0 } });
	alice.Give(PLAYER, quest);
	alice.Give(PLAYER, runtime);
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, ClassOf(quest)));
	REQUIRE(alice.Put(f, CHEST, ClassOf(runtime)));
	alice.Close(f);
	SettleAll(f, { &alice, &bob });
	CHECK(alice.gifts.Counters().notGiven == 2);
	CHECK(alice.Noticed(Notice::kNotGiven) == 1);  // said once a session
	CHECK(alice.In(CHEST, ClassOf(quest)) == 1);
	CHECK(alice.In(CHEST, ClassOf(runtime)) == 1);
	CHECK(bob.gifts.Lying().empty());
}

TEST("gifts: no save holds a copy, and a gift whose mod a game lacks shows there not at all")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	Game       carol{ 3, CAROL_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	carol.Join(f, "Carol");
	const auto gun = ClassOf(Gun());
	GiveGun(f, alice, { &alice, &bob, &carol });
	bob.Open(f, CHEST);
	CHECK(bob.In(CHEST, gun) == 1);
	const auto save = bob.SaveGame(f);
	CHECK(bob.In(CHEST, gun) == 0);
	CHECK(GiftBook::CountOf(GiftBook::Count(save.inventories.contains(CHEST) ? save.inventories.at(CHEST) : std::vector<GiftBook::Stack>{}), gun) == 0);
	CHECK(Total(gun, { &alice, &bob, &carol }, bob) == 1);

	const auto dlc = Gun("Far Harbor", { { DLC_MOD, 0, 0 } });
	bob.lacking.insert(DLC_MOD);
	GiveGun(f, alice, { &alice, &bob, &carol }, dlc);
	bob.Open(f, CHEST);
	CHECK(bob.gifts.Copies() == 1);  // the first gun's, not this one
	CHECK(bob.In(CHEST, ClassOf(dlc)) == 0);
	CHECK(bob.gifts.Counters().unplaceable == 1);
	bob.Close(f);
	TakeGift(f, carol, ClassOf(dlc), { &alice, &bob, &carol });
	CHECK(carol.Carried(ClassOf(dlc)) == 1);
	CHECK(Total(ClassOf(dlc), { &alice, &bob, &carol }, bob) == 1);
}

TEST("gifts: a power armor piece keeps its condition, and a take-all with the taker's own like gun takes both once")
{
	Fixture    f{ Gifts() };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	const auto armor = ClassOf(Armor(0.42f));
	alice.Give(PLAYER, Armor(0.42f));
	alice.Open(f, CHEST);
	REQUIRE(alice.Put(f, CHEST, armor));
	alice.Receive(f);
	alice.Close(f);
	SettleAll(f, { &alice, &bob });
	TakeGift(f, bob, armor, { &alice, &bob });
	CHECK(bob.Carried(armor) == 1);
	CHECK(bob.Carried(ClassOf(Armor(0.43f))) == 0);
	CHECK(Total(armor, { &alice, &bob }, bob) == 1);

	// Bob's own chest holds a gun like the gift: Take All takes both, his own straight and the gift once granted.
	const auto gun = ClassOf(Gun());
	bob.Give(CHEST, Gun());
	GiveGun(f, alice, { &alice, &bob });
	bob.Open(f, CHEST);
	CHECK(bob.In(CHEST, gun) == 2);
	REQUIRE(bob.Take(f, CHEST, gun, false));
	REQUIRE(bob.Take(f, CHEST, gun));
	CHECK(bob.gifts.Claims() == 1);
	CHECK(bob.Carried(gun) == 1);  // his own; the gift held back
	SettleAll(f, { &alice, &bob });
	bob.Close(f);
	CHECK(bob.Carried(gun) == 2);
	CHECK(Total(gun, { &alice, &bob }, alice) == 2);

	// Taking just one of two like guns takes the gift first: his own stays in his chest.
	bob.Give(CHEST, Gun());
	bob.inventories[PLAYER].clear();
	GiveGun(f, alice, { &alice, &bob });
	bob.Open(f, CHEST);
	REQUIRE(bob.Take(f, CHEST, gun));
	SettleAll(f, { &alice, &bob });
	bob.Close(f);
	CHECK(bob.Carried(gun) == 1);
	CHECK(bob.In(CHEST, gun) == 1);
	CHECK(alice.Lying(gun) == 0);
}

TEST("gifts: shared storage on, a gift in a home's chest works the same: storage never carries weapons or armor")
{
	auto options = Gifts();
	options.rules.sharedStorage = true;
	Fixture    f{ options };
	Game       alice{ 1, ALICE_ID };
	Game       bob{ 2, BOB_ID };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	for (auto* game : { &alice, &bob }) {
		game->holderFacts[CHEST] = { .kind = LootScope::Holder::kContainer, .storage = true };
	}
	const auto gun = ClassOf(Gun());
	GiveGun(f, alice, { &alice, &bob });
	TakeGift(f, bob, gun, { &alice, &bob });
	CHECK(bob.Carried(gun) == 1);
	CHECK(Total(gun, { &alice, &bob }, alice) == 1);
}
