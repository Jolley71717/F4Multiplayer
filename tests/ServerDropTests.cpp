// Items players give each other by dropping them: the server tells every game, grants each to one taker, makes a
// game's reloaded world agree with the session, and keeps what lies there through a restart.

#include "ServerFixture.h"

#include "SessionFile.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	Protocol::DroppedItem Gun(std::uint64_t a_nonce)
	{
		Protocol::DroppedItem item;
		item.nonce = a_nonce;
		item.base = 0x0004F46A;
		item.count = 1;
		item.templateIndex = 3;
		item.mods = { { 0x0004F46B, 0, 0 }, { 0x001C3D37, 1, 0 } };
		item.name = "Old Reliable";
		item.worldspace = 0x0000003C;
		item.x = -59420.5f;
		item.y = 72560.25f;
		item.z = 6174.0f;
		return item;
	}

	std::vector<Protocol::DroppedItem> Placed(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kDropPlaced, [](auto a_data) { return Protocol::DecodeDroppedItem(a_data); });
	}

	std::vector<Protocol::DropGone> Gone(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kDropGone, [](auto a_data) { return Protocol::DecodeDropGone(a_data); });
	}

	// The drops the lists in a_packets name, and whether the last packet said it was the last.
	std::pair<std::vector<Protocol::DroppedItem>, bool> Listed(const Packets& a_packets)
	{
		std::vector<Protocol::DroppedItem> out;
		bool                               last = false;
		for (const auto& list : Decoded(a_packets, MT::kDropList, [](auto a_data) { return Protocol::DecodeDropList(a_data); })) {
			out.insert(out.end(), list.drops.begin(), list.drops.end());
			last = list.last;
		}
		return { out, last };
	}

	// a_peer's player drops a_item: the offer they get (to them alone).
	Protocol::DroppedItem Offer(Fixture& a_f, PeerId a_peer, const Protocol::DroppedItem& a_item)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_item, MT::kReportDrop));
		const auto placed = Placed(Received(a_f, a_peer));
		REQUIRE(placed.size() == 1);
		return placed[0];
	}

	// a_peer's player drops a_item and their game swaps it for a copy (kPlaced): it lies there for everyone. The drop.
	Protocol::DroppedItem Drop(Fixture& a_f, PeerId a_peer, const Protocol::DroppedItem& a_item)
	{
		const auto offer = Offer(a_f, a_peer, a_item);
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::ClaimDrop{ offer.dropId, Protocol::DropClaim::kPlaced }));
		CHECK(Placed(Received(a_f, a_peer)).size() == 1);  // their game hears it lies there (it has its copy already)
		return offer;
	}

	void Claim(Fixture& a_f, PeerId a_peer, std::uint32_t a_dropId, Protocol::DropClaim a_op = Protocol::DropClaim::kTake)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::ClaimDrop{ a_dropId, a_op }));
	}

	void SendRecord(Fixture& a_f, PeerId a_peer, const Protocol::DropRecord& a_record)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_record));
	}
}

TEST("server drops: a drop is offered to its dropper alone, and goes to every other game, as theirs, once their game swapped it")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	const auto drop = Offer(f, 1, Gun(77));
	CHECK(drop.dropId != 0);
	CHECK(drop.ownerId == alice);
	CHECK(drop.nonce == 77);
	CHECK(drop.mods == Gun(77).mods);
	CHECK(drop.name == "Old Reliable");
	CHECK(Placed(Received(f, 2)).empty());  // Alice's game still has her item
	Claim(f, 2, drop.dropId, Protocol::DropClaim::kPlaced);  // nor can anyone but her say it swapped it
	CHECK(Placed(Received(f, 2)).empty());
	Claim(f, 1, drop.dropId, Protocol::DropClaim::kPlaced);
	const auto bob = Placed(Received(f, 2));
	REQUIRE(bob.size() == 1);
	CHECK(bob[0] == drop);
	// Alice's game hears it too: until then it would give the item back to her world if she left (her kPlaced may
	// have gone with the link).
	const auto alice2 = Placed(Received(f, 1));
	REQUIRE(alice2.size() == 1);
	CHECK(alice2[0] == drop);
}

// The dropper's game hasn't swapped its item for a copy yet (or never will: it left first): nobody else can have it.
TEST("server drops: an offer can't be taken, listed or kept past its dropper leaving or picking it up again")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob", 0xB));
	REQUIRE(welcome);
	Clear(f, { 1, 2 });
	const auto offered = Offer(f, 1, Gun(1));
	Claim(f, 2, offered.dropId);
	auto gone = Gone(Received(f, 2));
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].takerId == 0);  // refused: not granted to Bob
	CHECK(Gone(Received(f, 1)).empty());
	SendRecord(f, 2, { welcome->sessionId, {}, {} });
	CHECK(Listed(Received(f, 2)).first.empty());

	// Picked up again before the offer came: it never lay there.
	const auto kept = Offer(f, 1, Gun(2));
	Claim(f, 1, kept.dropId, Protocol::DropClaim::kKept);
	Claim(f, 1, kept.dropId, Protocol::DropClaim::kPlaced);  // too late: gone
	CHECK(Placed(Received(f, 2)).empty());

	// Alice leaves before her game says it swapped the first: it goes with her.
	f.fake->Drop(1);
	Claim(f, 2, offered.dropId);
	gone = Gone(Received(f, 2));
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].takerId == 0);
	JoinAs(f, 1, "Alice", 0xA);
	Clear(f, { 1, 2 });
	Claim(f, 1, offered.dropId, Protocol::DropClaim::kPlaced);
	CHECK(Placed(Received(f, 2)).empty());
}

// The dropper loaded a save before the offer reached their game: their record says whether their world still has it.
TEST("server drops: a dropper's record settles an offer a load overtook: laid down if their world swapped it out, else gone")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice", 0xA));
	REQUIRE(welcome);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	const auto swapped = Offer(f, 1, Gun(1));
	const auto held = Offer(f, 1, Gun(2));
	SendRecord(f, 1, { welcome->sessionId, { 1 }, {} });
	const auto aliceGot = Received(f, 1);
	const auto gone = Gone(aliceGot);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].dropId == held.dropId);
	const auto [list, last] = Listed(aliceGot);
	REQUIRE(list.size() == 1);
	CHECK(list[0].dropId == swapped.dropId);
	const auto bobGot = Received(f, 2);
	const auto bob = Placed(bobGot);
	REQUIRE(bob.size() == 1);
	CHECK(bob[0].dropId == swapped.dropId);
	CHECK(Gone(bobGot).empty());  // he never heard of the other
}

TEST("server drops: two players taking the same item at once: one gets it, and every game hears who")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	const auto bob = JoinAs(f, 2, "Bob", 0xB);
	JoinAs(f, 3, "Carol", 0xC);
	Clear(f, { 1, 2, 3 });
	const auto drop = Drop(f, 1, Gun(77));
	Clear(f, { 2, 3 });
	Claim(f, 2, drop.dropId);
	Claim(f, 3, drop.dropId);
	for (const PeerId peer : { 1, 2, 3 }) {
		const auto gone = Gone(Received(f, peer));
		REQUIRE(!gone.empty());
		for (const auto& entry : gone) {
			CHECK(entry.dropId == drop.dropId);
			CHECK(entry.takerId == bob);  // Carol's refusal names Bob too: her copy goes, and nobody else's take is said
		}
	}
}

// Workshop mode scraps a loose item: Bob's game claims the drop as his take (Drops' Scraps), and his record says his world
// took it from that moment. Nothing new on the wire: the server's rules for a take are what keeps the item in one place.
TEST("server drops: an item scrapped in workshop mode is the scrapper's take: gone for everyone, and never listed or laid down again")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	const auto bobWelcome = f.Join(2, Fixture::MakeHello("Bob", 0xB));
	REQUIRE(bobWelcome);
	const auto carolWelcome = f.Join(3, Fixture::MakeHello("Carol", 0xC));
	REQUIRE(carolWelcome);
	const auto session = bobWelcome->sessionId;
	Clear(f, { 1, 2, 3 });
	const auto scrapped = Drop(f, 1, Gun(1));
	const auto lying = Drop(f, 1, Gun(2));
	Clear(f, { 1, 2, 3 });

	Claim(f, 2, scrapped.dropId);  // Bob's copy went in a scrap
	for (const PeerId peer : { 1, 2, 3 }) {
		const auto gone = Gone(Received(f, peer));
		REQUIRE(gone.size() == 1);
		CHECK(gone[0].dropId == scrapped.dropId);
		CHECK(gone[0].takerId == bobWelcome->playerId);  // every friend's copy goes
	}
	// Carol's game, a moment behind, asks for it: refused, naming Bob.
	Claim(f, 3, scrapped.dropId);
	const auto refused = Gone(Received(f, 3));
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].takerId == bobWelcome->playerId);

	// Bob's record (he took it at the scrap) changes nothing, and lists only what still lies there.
	SendRecord(f, 2, { session, {}, { scrapped.dropId } });
	const auto bobGot = Received(f, 2);
	const auto [bobList, bobLast] = Listed(bobGot);
	CHECK(bobLast);
	REQUIRE(bobList.size() == 1);
	CHECK(bobList[0].dropId == lying.dropId);
	CHECK(Placed(Received(f, 1)).empty());
	CHECK(Placed(Received(f, 3)).empty());
	SendRecord(f, 3, { carolWelcome->sessionId, {}, {} });
	const auto [carolList, carolLast] = Listed(Received(f, 3));
	REQUIRE(carolList.size() == 1);
	CHECK(carolList[0].dropId == lying.dropId);
}

TEST("server drops: a scrap a moment after someone else's take leaves the item theirs, and the scrapper's record lays nothing down")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	const auto bobWelcome = f.Join(2, Fixture::MakeHello("Bob", 0xB));
	REQUIRE(bobWelcome);
	const auto carol = JoinAs(f, 3, "Carol", 0xC);
	Clear(f, { 1, 2, 3 });
	const auto drop = Drop(f, 1, Gun(1));
	Clear(f, { 2, 3 });
	Claim(f, 3, drop.dropId);  // Carol takes it
	Claim(f, 2, drop.dropId);  // Bob's copy went in a scrap just after
	const auto lost = Gone(Received(f, 2));
	REQUIRE(!lost.empty());
	for (const auto& entry : lost) {
		CHECK(entry.takerId == carol);  // Bob's game takes the scrap back out of its record (Record::Untook)
	}
	Clear(f, { 1, 3 });
	SendRecord(f, 2, { bobWelcome->sessionId, {}, {} });
	CHECK(Listed(Received(f, 2)).first.empty());
	CHECK(Placed(Received(f, 1)).empty());
	CHECK(Placed(Received(f, 3)).empty());
}

TEST("server drops: a take its game couldn't finish lies there again for everyone")
{
	Fixture f;
	JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	const auto drop = Drop(f, 1, Gun(77));
	Claim(f, 2, drop.dropId);
	Clear(f, { 1, 2 });
	Claim(f, 2, drop.dropId, Protocol::DropClaim::kGiveBack);
	const auto again = Placed(Received(f, 1));
	REQUIRE(again.size() == 1);
	CHECK(again[0].dropId == drop.dropId);
	CHECK(again[0].takerId == 0);
	CHECK(Placed(Received(f, 2)).size() == 1);
}

TEST("server drops: a drop past the session's share is refused to its dropper only (no drop ID), so their game keeps it")
{
	Fixture f;
	JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	for (std::uint64_t n = 1; n <= 256; ++n) {
		f.fake->Deliver(1, Protocol::Encode(Gun(n), MT::kReportDrop));
	}
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Protocol::Encode(Gun(999), MT::kReportDrop));
	const auto refused = Placed(Received(f, 1));
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].dropId == 0);
	CHECK(refused[0].nonce == 999);
	CHECK(Placed(Received(f, 2)).empty());
}

TEST("server drops: a record answers with what lies there; a taker's reload from before the take puts it back for everyone")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	const auto welcome = f.Join(3, Fixture::MakeHello("Carol", 0xC));
	REQUIRE(welcome);
	Clear(f, { 1, 2, 3 });
	const auto lying = Drop(f, 1, Gun(1));
	const auto taken = Drop(f, 1, Gun(2));
	Claim(f, 2, taken.dropId);
	Clear(f, { 1, 2, 3 });

	// Carol's world says it has nothing of the session: she is told what lies there.
	SendRecord(f, 3, { welcome->sessionId, {}, {} });
	auto [carolList, last] = Listed(Received(f, 3));
	CHECK(last);
	REQUIRE(carolList.size() == 1);
	CHECK(carolList[0].dropId == lying.dropId);

	// Bob loads a save from before his take: the gun lies there again, in every game.
	SendRecord(f, 2, { welcome->sessionId, {}, {} });
	const auto bobGot = Received(f, 2);
	const auto [bobList, bobLast] = Listed(bobGot);
	CHECK(bobLast);
	CHECK(bobList.size() == 2);
	const auto aliceGot = Placed(Received(f, 1));
	REQUIRE(aliceGot.size() == 1);
	CHECK(aliceGot[0].dropId == taken.dropId);
	CHECK(f.Logged("lacks 1 it was said to hold"));
}

TEST("server drops: a giver's reload from before the drop takes it back from every game; a record of another session counts as none")
{
	Fixture    f;
	JoinAs(f, 1, "Alice", 0xA);
	const auto welcome = f.Join(2, Fixture::MakeHello("Bob", 0xB));
	REQUIRE(welcome);
	Clear(f, { 1, 2 });
	const auto kept = Drop(f, 1, Gun(1));
	const auto undone = Drop(f, 1, Gun(2));
	Clear(f, { 2 });

	SendRecord(f, 1, { welcome->sessionId, { 1 }, {} });
	const auto aliceGot = Received(f, 1);
	const auto gone = Gone(Received(f, 2));
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].dropId == undone.dropId);
	CHECK(gone[0].takerId == 0);
	const auto [list, last] = Listed(aliceGot);
	REQUIRE(list.size() == 1);
	CHECK(list[0].dropId == kept.dropId);

	// A save of another session (another host's, or from before this one) has none of it: the rest goes back too.
	std::this_thread::sleep_for(300ms);  // past the time between two records
	SendRecord(f, 1, { welcome->sessionId + 1, { 1 }, {} });
	const auto back = Gone(Received(f, 2));
	REQUIRE(back.size() == 1);
	CHECK(back[0].dropId == kept.dropId);
}

// The list is up to ~385 KB: a game sending records over and over must not have it sent each time.
TEST("server drops: records sent over and over are settled at most four times a second, the last one too, and the list goes once")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice", 0xA));
	REQUIRE(welcome);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	Drop(f, 1, Gun(1));
	const auto undone = Drop(f, 1, Gun(2));
	Clear(f, { 2 });
	for (int i = 0; i < 50; ++i) {
		SendRecord(f, 1, { welcome->sessionId, { 1, 2 }, {} });
	}
	SendRecord(f, 1, { welcome->sessionId, { 1 }, {} });  // the last: a save from before the second drop
	const auto first = Received(f, 1);
	CHECK(Count(first, MT::kDropList) == 1);
	// The last one is settled at the tick (the second drop goes back to her)...
	Packets bob;
	REQUIRE(Await(f, 2, MT::kDropGone, bob, 2000ms));
	const auto gone = Gone(bob);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].dropId == undone.dropId);
	// ... but not answered with the list again.
	CHECK(Count(During(f, 1, 500ms), MT::kDropList) == 0);
	std::this_thread::sleep_for(300ms);
	SendRecord(f, 1, { welcome->sessionId, { 1 }, {} });
	CHECK(Count(Received(f, 1), MT::kDropList) == 0);
}

// "Take all" from a big chest, or a long fight, can spend a player's event budget: a drop or a claim dropped then would
// leave an item in no inventory, or in two.
TEST("server drops: reports and claims get through while the player's event budget is spent")
{
	Fixture f;
	JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	const auto spend = [&](PeerId a_peer) {
		for (int i = 0; i < 600; ++i) {
			f.fake->Deliver(a_peer, Protocol::Encode(Protocol::PlayerStatus{}, MT::kReportStatus));
		}
	};
	spend(1);
	const auto drop = Drop(f, 1, Gun(1));
	CHECK(f.Logged("sends too many events"));
	const auto bobGot = Placed(Received(f, 2));
	REQUIRE(bobGot.size() == 1);
	CHECK(bobGot[0].dropId == drop.dropId);
	spend(2);
	Claim(f, 2, drop.dropId);
	const auto gone = Gone(Received(f, 1));
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].takerId != 0);
	spend(2);
	Claim(f, 2, drop.dropId, Protocol::DropClaim::kGiveBack);
	CHECK(Placed(Received(f, 1)).size() == 1);
}

TEST("server drops: what lies in the session, and who took what, survive the host's server restarting")
{
	const TempSessionFile file;
	Server::Options       options;
	options.sessionFile = file.path.string();
	Protocol::DroppedItem lying;
	Protocol::DroppedItem taken;
	std::uint64_t         session = 0;
	{
		Fixture    f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice", 0xA));
		REQUIRE(welcome);
		session = welcome->sessionId;
		JoinAs(f, 2, "Bob", 0xB);
		Clear(f, { 1, 2 });
		lying = Drop(f, 1, Gun(1));
		taken = Drop(f, 1, Gun(2));
		Claim(f, 2, taken.dropId);
		Clear(f, { 1, 2 });
	}  // stopping saves
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(saved->drops.size() == 2);
	{
		Fixture    f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice", 0xA));
		REQUIRE(welcome);
		CHECK(welcome->sessionId == session);
		JoinAs(f, 2, "Bob", 0xB);
		Clear(f, { 1, 2 });
		SendRecord(f, 1, { session, { 1, 2 }, {} });
		const auto [list, last] = Listed(Received(f, 1));
		REQUIRE(list.size() == 1);
		CHECK(list[0] == lying);
		// Bob's save from before his take still puts it back after the restart.
		SendRecord(f, 2, { session, {}, {} });
		const auto [bobList, bobLast] = Listed(Received(f, 2));
		CHECK(bobList.size() == 2);
		const auto back = Placed(Received(f, 1));
		REQUIRE(back.size() == 1);
		CHECK(back[0].dropId == taken.dropId);
		// And a new drop doesn't take a number the old session used.
		const auto next = Drop(f, 1, Gun(3));
		CHECK(next.dropId > taken.dropId);
	}
}

TEST("server drops: a joiner's drops wait for their welcome, and a broken one is ignored")
{
	Fixture f;
	JoinAs(f, 1, "Alice", 0xA);
	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(Gun(1), MT::kReportDrop));  // before any hello
	auto broken = Gun(2);
	broken.count = 0;
	f.fake->Deliver(1, Protocol::Encode(broken, MT::kReportDrop));
	CHECK(Placed(Received(f, 1)).empty());
}
