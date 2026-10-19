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

	// Alice drops a_item; returns its drop as everyone was told it.
	Protocol::DroppedItem Drop(Fixture& a_f, PeerId a_peer, const Protocol::DroppedItem& a_item)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(a_item, MT::kReportDrop));
		const auto placed = Placed(Received(a_f, a_peer));
		REQUIRE(placed.size() == 1);
		return placed[0];
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

TEST("server drops: a drop goes to every game, the dropper's too, as the dropper's, with its mods and name")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice", 0xA);
	JoinAs(f, 2, "Bob", 0xB);
	Clear(f, { 1, 2 });
	const auto drop = Drop(f, 1, Gun(77));
	CHECK(drop.dropId != 0);
	CHECK(drop.ownerId == alice);
	CHECK(drop.nonce == 77);
	CHECK(drop.mods == Gun(77).mods);
	CHECK(drop.name == "Old Reliable");
	const auto bob = Placed(Received(f, 2));
	REQUIRE(bob.size() == 1);
	CHECK(bob[0] == drop);
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

TEST("server drops: records sent over and over are answered at most four times a second, the last one too")
{
	Fixture    f;
	const auto welcome = f.Join(1, Fixture::MakeHello("Alice", 0xA));
	REQUIRE(welcome);
	Clear(f, { 1 });
	for (int i = 0; i < 50; ++i) {
		SendRecord(f, 1, { welcome->sessionId, {}, {} });
	}
	const auto first = Received(f, 1);
	CHECK(Count(first, MT::kDropList) == 1);
	Packets later;
	CHECK(Await(f, 1, MT::kDropList, later, 2000ms));
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
