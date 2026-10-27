#include "Test.h"

#include "DropStore.h"
#include "SessionFile.h"

#include <cstring>
#include <vector>

// The server's book of items players give each other by dropping them: each goes to one taker, and what a game's save
// holds after a load is made to agree with the book, so a reload neither copies a given item nor loses it.
namespace
{
	constexpr std::uint32_t ALICE = 1;
	constexpr std::uint32_t BOB = 2;
	constexpr std::uint32_t CAROL = 3;
	constexpr std::uint64_t SESSION = 0x5E55105E55105E55ull;

	Protocol::DroppedItem Gun(std::uint64_t a_nonce)
	{
		Protocol::DroppedItem item;
		item.nonce = a_nonce;
		item.base = 0x0004F46A;
		item.count = 1;
		item.templateIndex = 3;
		item.mods = { { 0x0004F46B, 0, 0 } };
		item.worldspace = 0x0000003C;
		item.x = 100.0f;
		return item;
	}

	Protocol::DropRecord Record(std::vector<std::uint64_t> a_made, std::vector<std::uint32_t> a_taken, std::uint64_t a_session = SESSION)
	{
		return { a_session, std::move(a_made), std::move(a_taken) };
	}

	// a_owner drops a_item and their game swaps it for a copy: it lies there for anyone. Its drop ID.
	std::uint32_t Laid(DropStore& a_store, std::uint32_t a_owner, const Protocol::DroppedItem& a_item)
	{
		const auto added = a_store.Add(a_owner, a_item);
		REQUIRE(added.drop);
		REQUIRE(a_store.Confirm(a_owner, added.drop->dropId));
		return added.drop->dropId;
	}
}

TEST("drop store: a drop is numbered and kept as its dropper's, with what it carries")
{
	DropStore  store;
	const auto added = store.Add(ALICE, Gun(11));
	REQUIRE(added.outcome == DropStore::Outcome::kAdded);
	REQUIRE(added.drop);
	CHECK(added.drop->dropId != 0);
	CHECK(added.drop->ownerId == ALICE);
	CHECK(added.drop->takerId == 0);
	CHECK(added.drop->mods == Gun(11).mods);
	REQUIRE(store.Confirm(ALICE, added.drop->dropId));
	CHECK(store.LyingCount() == 1);
	CHECK(store.Find(added.drop->dropId) != nullptr);
}

// The dropper's game hasn't swapped the item it dropped for a copy yet: an offer to it alone, which nobody can take.
TEST("drop store: a drop is only offered to its dropper until their game confirms it; nobody takes, lists or saves an offer")
{
	DropStore  store;
	const auto id = store.Add(ALICE, Gun(11)).drop->dropId;
	CHECK(store.LyingCount() == 0);
	CHECK(store.OfferCount() == 1);
	CHECK(store.Find(id) == nullptr);
	CHECK(store.Lying().empty());
	CHECK(store.All().empty());
	CHECK(!store.Take(BOB, id));
	CHECK(!store.Take(ALICE, id));
	CHECK(!store.Confirm(BOB, id));  // only its dropper's game
	const auto laid = store.Confirm(ALICE, id);
	REQUIRE(laid);
	CHECK(laid->dropId == id);
	CHECK(!store.Confirm(ALICE, id));  // once
	CHECK(store.Take(BOB, id));
}

TEST("drop store: an offer its dropper kept (picked up again) or left without confirming goes; others' offers stay")
{
	DropStore  store;
	const auto kept = store.Add(ALICE, Gun(11)).drop->dropId;
	CHECK(!store.Keep(BOB, kept));
	CHECK(store.Keep(ALICE, kept));
	CHECK(!store.Confirm(ALICE, kept));
	CHECK(store.OfferCount() == 0);
	store.Add(ALICE, Gun(12));
	store.Add(ALICE, Gun(13));
	const auto bobs = store.Add(BOB, Gun(12)).drop->dropId;
	const auto laid = Laid(store, ALICE, Gun(14));
	CHECK(store.DropOffers(ALICE) == 2);
	CHECK(store.OfferCount() == 1);
	CHECK(store.Find(laid) != nullptr);  // confirmed: it lies there whoever leaves
	CHECK(store.Confirm(BOB, bobs));
}

// A load reached the dropper's game before the offer did: its record says whether its world still has the item.
TEST("drop store: a dropper's record settles their offers: one their world swapped out lies there, one it has goes")
{
	DropStore  store;
	const auto swapped = store.Add(ALICE, Gun(11)).drop->dropId;
	const auto held = store.Add(ALICE, Gun(12)).drop->dropId;
	const auto bobs = store.Add(BOB, Gun(11)).drop->dropId;
	const auto out = store.Reconcile(ALICE, Record({ 11 }, { bobs }), SESSION);
	REQUIRE(out.confirmed.size() == 1);
	CHECK(out.confirmed[0].dropId == swapped);
	REQUIRE(out.unoffered.size() == 1);
	CHECK(out.unoffered[0].dropId == held);
	CHECK(out.unoffered[0].takerId == 0);
	CHECK(out.withdrawn.empty());  // never lay anywhere: nobody else is told
	CHECK(out.changed);
	CHECK(store.Find(swapped) != nullptr);
	CHECK(store.OfferCount() == 1);  // Bob's stays his to confirm, whatever her record names
	CHECK(store.Confirm(BOB, bobs));
}

TEST("drop store: the same drop reported twice is kept once, and a drop needs its game's number")
{
	DropStore  store;
	const auto first = store.Add(ALICE, Gun(11));
	const auto again = store.Add(ALICE, Gun(11));
	CHECK(again.outcome == DropStore::Outcome::kRepeat);
	REQUIRE(again.drop);
	CHECK(again.drop->dropId == first.drop->dropId);
	CHECK(store.OfferCount() == 1);
	REQUIRE(store.Confirm(ALICE, first.drop->dropId));
	CHECK(store.Add(ALICE, Gun(11)).outcome == DropStore::Outcome::kRepeat);  // lying there by now: the same answer
	CHECK(store.LyingCount() == 1);
	// Another player's game may number one the same: theirs is another drop.
	CHECK(store.Add(BOB, Gun(11)).outcome == DropStore::Outcome::kAdded);
	CHECK(store.Add(ALICE, Gun(0)).outcome == DropStore::Outcome::kInvalid);
}

TEST("drop store: two players taking the same item at once: the first gets it, the second nothing")
{
	DropStore  store;
	const auto id = Laid(store, ALICE, Gun(11));
	const auto bob = store.Take(BOB, id);
	const auto carol = store.Take(CAROL, id);
	REQUIRE(bob);
	CHECK(bob->takerId == BOB);
	CHECK(!carol);
	CHECK(store.LyingCount() == 0);
	CHECK(store.TakerOf(id) == BOB);
	// Nor can it be taken again later, by them or anyone.
	CHECK(!store.Take(BOB, id));
	CHECK(!store.Take(ALICE, id));
}

TEST("drop store: the dropper may take their own item back")
{
	DropStore  store;
	const auto id = Laid(store, ALICE, Gun(11));
	const auto back = store.Take(ALICE, id);
	REQUIRE(back);
	CHECK(back->takerId == ALICE);
}

TEST("drop store: a take its game couldn't finish lies there again, under its own number; only the taker can give it back")
{
	DropStore  store;
	const auto id = Laid(store, ALICE, Gun(11));
	REQUIRE(store.Take(BOB, id));
	CHECK(!store.GiveBack(CAROL, id));
	const auto back = store.GiveBack(BOB, id);
	REQUIRE(back);
	CHECK(back->dropId == id);
	CHECK(back->takerId == 0);
	CHECK(store.LyingCount() == 1);
	CHECK(!store.GiveBack(BOB, id));  // once
	REQUIRE(store.Take(CAROL, id));
}

TEST("drop store: an unknown item can't be taken or given back")
{
	DropStore store;
	CHECK(!store.Take(BOB, 99));
	CHECK(!store.GiveBack(BOB, 99));
	CHECK(store.TakerOf(99) == 0);
}

TEST("drop store: one player can't fill the session, nor everyone together beyond its cap")
{
	DropStore store{ DropStore::Limits{ .lying = 3, .taken = 10, .perOwner = 2 } };
	CHECK(store.Add(ALICE, Gun(1)).outcome == DropStore::Outcome::kAdded);
	CHECK(store.Add(ALICE, Gun(2)).outcome == DropStore::Outcome::kAdded);
	CHECK(store.Add(ALICE, Gun(3)).outcome == DropStore::Outcome::kOwnerFull);
	CHECK(store.Add(BOB, Gun(1)).outcome == DropStore::Outcome::kAdded);
	CHECK(store.Add(CAROL, Gun(1)).outcome == DropStore::Outcome::kFull);  // offers count
	// Taken items make room again.
	REQUIRE(store.Confirm(ALICE, 1));
	REQUIRE(store.Take(BOB, 1));
	CHECK(store.Add(ALICE, Gun(3)).outcome == DropStore::Outcome::kAdded);
}

TEST("drop store: takes are remembered up to the cap, the oldest forgotten first")
{
	DropStore store{ DropStore::Limits{ .lying = 10, .taken = 2, .perOwner = 10 } };
	std::vector<std::uint32_t> ids;
	for (std::uint64_t n = 1; n <= 3; ++n) {
		ids.push_back(Laid(store, ALICE, Gun(n)));
		REQUIRE(store.Take(BOB, ids.back()));
	}
	CHECK(store.TakenCount() == 2);
	CHECK(store.TakerOf(ids[0]) == 0);
	CHECK(store.TakerOf(ids[2]) == BOB);
}

// The giver loads a save from before they dropped it: the item is back in their inventory, so it lies nowhere else.
TEST("drop store: a drop its owner's world has again (a save from before it) is withdrawn")
{
	DropStore  store;
	const auto kept = Laid(store, ALICE, Gun(11));
	const auto undone = Laid(store, ALICE, Gun(12));
	const auto bobs = Laid(store, BOB, Gun(12));
	const auto out = store.Reconcile(ALICE, Record({ 11 }, {}), SESSION);
	REQUIRE(out.withdrawn.size() == 1);
	CHECK(out.withdrawn[0].dropId == undone);
	CHECK(out.withdrawn[0].takerId == 0);
	CHECK(out.returned.empty());
	CHECK(out.changed);
	CHECK(store.Find(kept) != nullptr);
	CHECK(store.Find(undone) == nullptr);
	CHECK(store.Find(bobs) != nullptr);  // another player's, whatever its number
}

// The taker loads a save from before they took it: it isn't in their inventory, so it lies there again for anyone.
TEST("drop store: an item its taker's world lacks (a save from before the take) lies there again")
{
	DropStore  store;
	const auto id = Laid(store, ALICE, Gun(11));
	REQUIRE(store.Take(BOB, id));
	const auto out = store.Reconcile(BOB, Record({}, {}), SESSION);
	REQUIRE(out.returned.size() == 1);
	CHECK(out.returned[0].dropId == id);
	CHECK(out.returned[0].takerId == 0);
	CHECK(store.Find(id) != nullptr);
	// A save from after the take has it: nothing changes.
	DropStore  other;
	const auto id2 = Laid(other, ALICE, Gun(11));
	REQUIRE(other.Take(BOB, id2));
	const auto same = other.Reconcile(BOB, Record({}, { id2 }), SESSION);
	CHECK(same.returned.empty());
	CHECK(same.withdrawn.empty());
	CHECK(!same.changed);
	CHECK(other.TakerOf(id2) == BOB);
}

TEST("drop store: a giver's save from after the drop keeps it lying; one from before a take of it by another is only counted")
{
	DropStore  store;
	const auto lying = Laid(store, ALICE, Gun(11));
	const auto taken = Laid(store, ALICE, Gun(12));
	REQUIRE(store.Take(BOB, taken));
	const auto after = store.Reconcile(ALICE, Record({ 11, 12 }, {}), SESSION);
	CHECK(after.withdrawn.empty());
	CHECK(after.dupRisk == 0);
	// From before both: the lying one goes back to her; Bob keeps the one he has, and both have it now.
	const auto before = store.Reconcile(ALICE, Record({}, {}), SESSION);
	REQUIRE(before.withdrawn.size() == 1);
	CHECK(before.withdrawn[0].dropId == lying);
	CHECK(before.dupRisk == 1);
	CHECK(store.TakerOf(taken) == BOB);
}

// Alice drops an item and picks it back up herself; then she loads a save from before the drop: she has it once, in
// her inventory, and it lies nowhere.
TEST("drop store: an item its owner took back, then a save from before the drop: it lies nowhere")
{
	DropStore  store;
	const auto id = Laid(store, ALICE, Gun(11));
	REQUIRE(store.Take(ALICE, id));
	const auto out = store.Reconcile(ALICE, Record({}, {}), SESSION);
	CHECK(out.returned.empty());
	CHECK(out.withdrawn.empty());
	CHECK(!out.changed);  // hers already
	CHECK(store.LyingCount() == 0);
	CHECK(store.TakerOf(id) == ALICE);
	// A save from after the drop, before taking it back: her world has no item, so it lies there again.
	DropStore  other;
	const auto id2 = Laid(other, ALICE, Gun(11));
	REQUIRE(other.Take(ALICE, id2));
	const auto mid = other.Reconcile(ALICE, Record({ 11 }, {}), SESSION);
	REQUIRE(mid.returned.size() == 1);
	CHECK(other.Find(id2) != nullptr);
}

// A world with no record of the session (a new game, a save from before it), then the save that has it: each load makes
// the book agree with the world loaded last, so nothing is lost or copied by going through the wrong one first.
TEST("drop store: a world without the record, then the save with it: what it gave lies there again, what it took is its own again")
{
	DropStore  store;
	const auto given = Laid(store, ALICE, Gun(11));      // Alice's, lying
	const auto bobs = Laid(store, BOB, Gun(21));         // Bob's, Alice took it
	REQUIRE(store.Take(ALICE, bobs));
	const auto wrong = store.Reconcile(ALICE, Record({}, {}), SESSION);
	REQUIRE(wrong.withdrawn.size() == 1);
	CHECK(wrong.withdrawn[0].dropId == given);
	CHECK(wrong.withdrawn[0].takerId == 0);
	REQUIRE(wrong.returned.size() == 1);
	CHECK(wrong.returned[0].dropId == bobs);
	CHECK(store.TakerOf(given) == ALICE);
	const auto right = store.Reconcile(ALICE, Record({ 11 }, { bobs }), SESSION);
	REQUIRE(right.returned.size() == 1);
	CHECK(right.returned[0].dropId == given);
	REQUIRE(right.withdrawn.size() == 1);
	CHECK(right.withdrawn[0].dropId == bobs);
	CHECK(right.withdrawn[0].takerId == ALICE);
	CHECK(store.Find(given) != nullptr);
	CHECK(store.TakerOf(bobs) == ALICE);
	CHECK(right.dupRisk == 0);
	// The same record again changes nothing.
	CHECK(!store.Reconcile(ALICE, Record({ 11 }, { bobs }), SESSION).changed);
}

// A save that has no record of this session (from before it, or another host's) holds none of its transfers.
TEST("drop store: a record of another session counts as none")
{
	DropStore  store;
	const auto mine = Laid(store, ALICE, Gun(11));
	const auto took = Laid(store, BOB, Gun(5));
	REQUIRE(store.Take(ALICE, took));
	const auto out = store.Reconcile(ALICE, Record({ 11 }, { took }, SESSION + 1), SESSION);
	REQUIRE(out.withdrawn.size() == 1);
	CHECK(out.withdrawn[0].dropId == mine);
	REQUIRE(out.returned.size() == 1);
	CHECK(out.returned[0].dropId == took);
}

TEST("drop store: what's kept goes into the session file and comes back, numbering on from where it was")
{
	DropStore  store;
	const auto lying = Laid(store, ALICE, Gun(11));
	const auto taken = Laid(store, ALICE, Gun(12));
	REQUIRE(store.Take(BOB, taken));

	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 1;
	snapshot.sessionId = SESSION;
	snapshot.drops = store.All();
	CHECK(!snapshot.Empty());
	const auto read = SessionFile::Decode(SessionFile::Encode(snapshot));
	REQUIRE(read);
	REQUIRE(read->drops.size() == 2);

	DropStore again;
	for (const auto& drop : read->drops) {
		again.Restore(drop);
	}
	REQUIRE(again.Find(lying) != nullptr);
	CHECK(*again.Find(lying) == *store.Find(lying));
	CHECK(again.TakerOf(taken) == BOB);
	const auto next = Laid(again, CAROL, Gun(1));
	CHECK(next > taken);
	// Bob's reload from before his take still brings it back after the restart.
	CHECK(again.Reconcile(BOB, Record({}, {}), SESSION).returned.size() == 1);
}

TEST("drop store: the session file holds any number of drops, in packets")
{
	DropStore store{ DropStore::Limits{ .lying = 1000, .taken = 10, .perOwner = 1000 } };
	for (std::uint64_t n = 1; n <= 150; ++n) {
		Laid(store, ALICE, Gun(n));
	}
	SessionFile::Snapshot snapshot;
	snapshot.drops = store.All();
	const auto read = SessionFile::Decode(SessionFile::Encode(snapshot));
	REQUIRE(read);
	CHECK(read->drops == snapshot.drops);
}

// The layout before drops (29) still reads, and holds none; a drop record in a file that says 29 is damage.
TEST("session file: a file of the layout before drops reads as one with none")
{
	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 1;
	snapshot.deadActors = { 0x0004B2BA };
	auto                bytes = SessionFile::Encode(snapshot);
	const std::uint16_t old = 29;
	std::memcpy(bytes.data() + 4, &old, sizeof(old));
	const auto read = SessionFile::Decode(bytes);
	REQUIRE(read);
	CHECK(read->deadActors == snapshot.deadActors);
	CHECK(read->drops.empty());

	snapshot.drops = { Gun(1) };
	snapshot.drops[0].dropId = 1;
	snapshot.drops[0].ownerId = ALICE;
	auto withDrops = SessionFile::Encode(snapshot);
	std::memcpy(withDrops.data() + 4, &old, sizeof(old));
	CHECK(!SessionFile::Decode(withDrops));
}

namespace
{
	constexpr std::uint32_t CHEST = 0x0001D9F6;

	// A gun put in a chest: a gift (src/game/GiftBook.h).
	Protocol::DroppedItem Gift(std::uint64_t a_nonce)
	{
		auto item = Gun(a_nonce);
		item.holder = CHEST;
		return item;
	}

	std::uint16_t LayoutOf(const std::vector<std::uint8_t>& a_bytes)
	{
		std::uint16_t layout = 0;
		std::memcpy(&layout, a_bytes.data() + 4, sizeof(layout));
		return layout;
	}
}

// A giver's game takes the gift out of its world as the offer comes, and gives nothing back on a link that went: so the
// offer can't go with them, as a loose drop's does. Their next record says whether the item left their world.
TEST("drop store: a gift's offer stays when its giver leaves, and their next record settles it either way")
{
	DropStore  store;
	const auto swapped = store.Add(ALICE, Gift(11)).drop->dropId;
	const auto held = store.Add(ALICE, Gift(12)).drop->dropId;
	const auto loose = store.Add(ALICE, Gun(13)).drop->dropId;
	CHECK(store.DropOffers(ALICE) == 1);  // the loose drop's alone (Alice's game gives that item back as it leaves)
	CHECK(store.OfferCount() == 2);
	CHECK(!store.Confirm(ALICE, loose));
	CHECK(!store.Take(BOB, swapped));  // still an offer: nobody takes it
	CHECK(store.LyingOf(ALICE) == 2);  // and it counts against her share

	// She is back: her world took 11 out (the swap came before the link went), and still has 12.
	const auto out = store.Reconcile(ALICE, Record({ 11 }, {}), SESSION);
	REQUIRE(out.confirmed.size() == 1);
	CHECK(out.confirmed[0].dropId == swapped);
	CHECK(out.confirmed[0].holder == CHEST);
	REQUIRE(out.unoffered.size() == 1);
	CHECK(out.unoffered[0].dropId == held);
	CHECK(store.OfferCount() == 0);
	CHECK(store.Take(BOB, swapped));
}

// The host stops (or the dedicated server restarts) while a gift is still an offer: the file keeps it as one, so the
// giver's record settles it after the restart, and it never lies before.
TEST("drop store: a gift's offer goes into the session file and comes back as an offer; a loose drop's offer never does")
{
	DropStore  store;
	const auto lying = Laid(store, ALICE, Gift(11));
	const auto offer = store.Add(ALICE, Gift(12)).drop->dropId;
	store.Add(ALICE, Gun(13));  // a loose drop's offer: not saved
	const auto all = store.All();
	REQUIRE(all.size() == 2);
	CHECK(all[1].dropId == offer);
	CHECK(all[1].takerId == Protocol::DROP_OFFERED);

	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 1;
	snapshot.sessionId = SESSION;
	snapshot.drops = all;
	const auto bytes = SessionFile::Encode(snapshot);
	CHECK(LayoutOf(bytes) == SessionFile::GIFTS);
	const auto read = SessionFile::Decode(bytes);
	REQUIRE(read);
	CHECK(read->drops == all);

	DropStore again;
	for (const auto& drop : read->drops) {
		again.Restore(drop);
	}
	CHECK(again.LyingCount() == 1);
	CHECK(again.OfferCount() == 1);
	REQUIRE(again.Find(lying) != nullptr);
	CHECK(again.Find(lying)->holder == CHEST);
	CHECK(again.Find(offer) == nullptr);
	CHECK(!again.Take(BOB, offer));
	CHECK(again.TakerOf(offer) == 0);
	const auto out = again.Reconcile(ALICE, Record({ 11, 12 }, {}), SESSION);
	REQUIRE(out.confirmed.size() == 1);
	CHECK(out.confirmed[0].dropId == offer);
	CHECK(out.confirmed[0].takerId == 0);
	CHECK(again.Take(BOB, offer));

	// A saved offer that names nobody, or no container, is no offer: left out.
	DropStore odd;
	auto      noOwner = all[1];
	noOwner.ownerId = 0;
	odd.Restore(noOwner);
	auto noHolder = all[1];
	noHolder.holder = 0;
	odd.Restore(noHolder);
	CHECK(odd.OfferCount() == 0);
	CHECK(odd.LyingCount() == 0);
}

// Layout 39 holds each drop's container. A session with no gift is still written in the layout an older build reads,
// and a file of an older layout reads its drops as loose; one that says it is older but holds holders is damage.
TEST("session file: a session with gifts takes layout 39; one without stays 36 or 38, and an older file's drops lie loose")
{
	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 1;
	snapshot.sessionId = SESSION;
	auto drop = Gun(1);
	drop.dropId = 1;
	drop.ownerId = ALICE;
	snapshot.drops = { drop };
	const auto loose = SessionFile::Encode(snapshot);
	CHECK(LayoutOf(loose) == SessionFile::OBJECT_PLACES);
	for (const std::uint16_t layout : { SessionFile::ALL_THREE, SessionFile::TRAPS_AND_DESTRUCTIONS, SessionFile::OBJECT_PLACES, SessionFile::STORAGE }) {
		auto bytes = loose;
		std::memcpy(bytes.data() + 4, &layout, sizeof(layout));
		const auto read = SessionFile::Decode(bytes);
		REQUIRE(read);
		CHECK(read->drops == snapshot.drops);
	}
	auto withOwners = snapshot;
	withOwners.storageOwners = { { 0x00020000, 1 } };
	CHECK(LayoutOf(SessionFile::Encode(withOwners)) == SessionFile::STORAGE);

	auto gifts = withOwners;
	gifts.drops.push_back(Gift(2));
	gifts.drops.back().dropId = 2;
	gifts.drops.back().ownerId = BOB;
	const auto bytes = SessionFile::Encode(gifts);
	CHECK(LayoutOf(bytes) == SessionFile::GIFTS);
	CHECK(SessionFile::Readable(SessionFile::GIFTS));
	const auto read = SessionFile::Decode(bytes);
	REQUIRE(read);
	CHECK(read->drops == gifts.drops);
	CHECK(read->storageOwners == gifts.storageOwners);
	auto older = bytes;
	const std::uint16_t storage = SessionFile::STORAGE;
	std::memcpy(older.data() + 4, &storage, sizeof(storage));
	CHECK(!SessionFile::Decode(older));
	CHECK(!SessionFile::Readable(40));  // a later build's
}
