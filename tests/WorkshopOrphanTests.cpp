// A built piece its builder's world no longer has: the server sends each player what it keeps as their own building
// in the world their game plays (kOwnWorkshopItems), once their record after the welcome or a load says whose world it is,
// and takes out for everyone what their game says it lacks (kReportWorkshopGone), keeping it as lost to come back if
// that world has it again. A real Server; the test plays the games. The game's side is WorkshopAudit
// (WorkshopAuditTests.cpp), and the Sim runs both together (SimScenarioTests.cpp).

#include "ServerFixture.h"

#include <set>

using namespace TestServer;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Key = std::pair<std::uint32_t, std::uint32_t>;

	constexpr std::uint64_t HOST = 0x0110000100000001;
	constexpr std::uint64_t BOB = 0x0110000100000002;
	constexpr std::uint64_t CAROL = 0x0110000100000003;
	// The worlds the players' games play (CharacterBinding::World's keys, said in each record): the player ID is the PC's.
	constexpr std::uint64_t HOST_A = 0x0000A00000000001;
	constexpr std::uint64_t BOB_A = 0x0000B00000000001;
	constexpr std::uint64_t BOB_B = 0x0000B00000000002;  // a second character of Bob's, on the same PC
	constexpr std::uint64_t CAROL_A = 0x0000C00000000001;
	constexpr std::uint32_t CHAIR = 0x0001F2A1;
	constexpr std::uint32_t BENCH = 0x000250FE;

	Protocol::WorkshopItem Chair(std::uint32_t a_ref, float a_x)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.base = CHAIR;
		item.workshop = BENCH;
		item.position[0] = a_x;
		item.position[1] = 1000.0f;
		return item;
	}

	std::vector<std::uint8_t> Built(const Protocol::WorkshopItem& a_item)
	{
		return Protocol::Encode(a_item, MT::kReportWorkshopItem);
	}

	// The builder's game says its world lacks these (what it was sent, each as a scrap).
	std::vector<std::uint8_t> GoneReport(std::vector<Protocol::WorkshopItem> a_items)
	{
		for (auto& item : a_items) {
			item.op = Protocol::WorkshopOp::kScrapped;
		}
		return Protocol::Encode(a_items, MT::kReportWorkshopGone);
	}

	// What the server keeps as the receiver's own building, by reference ID, from what it sent them: what stands (kPlaced),
	// or (a_lost) what their world said it lacked (kLost).
	std::set<std::uint32_t> Own(const Packets& a_packets, bool a_lost = false)
	{
		std::set<std::uint32_t> out;
		for (const auto& list : Decoded(a_packets, MT::kOwnWorkshopItems, Protocol::DecodeWorkshopItems)) {
			for (const auto& item : list) {
				if ((item.op == Protocol::WorkshopOp::kLost) == a_lost) {
					out.insert(item.refId);
				}
			}
		}
		return out;
	}

	// The others' building the receiver was sent: placed (true) or scrapped (false), the last word on each.
	std::map<Key, bool> Others(const Packets& a_packets)
	{
		std::map<Key, bool> out;
		for (const auto& packet : a_packets) {
			if (packet[0] == Type(MT::kWorkshopItem)) {
				const auto item = Protocol::DecodeWorkshopItem(packet);
				REQUIRE(item);
				out[{ item->playerId, item->refId }] = item->op == Protocol::WorkshopOp::kPlaced;
			} else if (packet[0] == Type(MT::kWorkshopItems)) {
				const auto items = Protocol::DecodeWorkshopItems(packet);
				REQUIRE(items);
				for (const auto& item : *items) {
					out[{ item.playerId, item.refId }] = item.op == Protocol::WorkshopOp::kPlaced;
				}
			}
		}
		return out;
	}

	Server::Options Hosted()
	{
		Server::Options options;
		options.hostIdentity = HOST;
		return options;
	}

	// A game's record of the world it plays (Drops, after each welcome and load): only whose world it is matters here.
	std::vector<std::uint8_t> Record(std::uint64_t a_character)
	{
		return Protocol::Encode(Protocol::DropRecord{ .character = a_character });
	}

	// Its record goes after its welcome, as the game's does; a_character 0: none yet (it is still loading, say).
	std::uint32_t JoinHost(Fixture& a_f, PeerId a_peer, std::uint64_t a_character = HOST_A)
	{
		a_f.fake->SetLocal(a_peer);
		const auto id = JoinAs(a_f, a_peer, "Host", HOST);
		if (a_character != 0) {
			a_f.fake->Deliver(a_peer, Record(a_character));
		}
		return id;
	}

	std::uint32_t JoinFriend(Fixture& a_f, PeerId a_peer, std::string a_name, std::uint64_t a_steam, std::uint64_t a_character)
	{
		a_f.fake->SetIdentity(a_peer, a_steam);
		const auto id = JoinAs(a_f, a_peer, std::move(a_name), a_steam);
		if (a_character != 0) {
			a_f.fake->Deliver(a_peer, Record(a_character));
		}
		return id;
	}

	// The game loaded a save of a_character's: it asks for the world again, then sends its record (Session::OnGameLoaded).
	// A record sooner than DROP_RECORD_INTERVAL after the last waits for the tick: the server's clock moves past it.
	void Load(Fixture& a_f, PeerId a_peer, std::uint64_t a_character)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
		a_f.Advance(std::chrono::milliseconds(300));
		a_f.fake->Deliver(a_peer, Record(a_character));
		Sync(a_f, a_peer);
	}
}

TEST("orphan pieces: each player is sent what the session keeps as their own building at the welcome and after a load, and nobody else's")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	const auto bob = JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(1, Built(Chair(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Built(Chair(0x0001A2B3, 150.0f)));  // a piece of the world the host moved: every game has it
	f.fake->Deliver(2, Built(Chair(0xFF000B01, 200.0f)));
	Sync(f, 1);
	Sync(f, 2);
	Clear(f, { 1, 2 });

	Load(f, 1, HOST_A);
	Load(f, 2, BOB_A);
	CHECK(Own(Received(f, 1)) == std::set<std::uint32_t>{ 0xFF000001 });
	CHECK(Own(Received(f, 2)) == std::set<std::uint32_t>{ 0xFF000B01 });

	const auto carol = JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	CHECK(Own(Received(f, 3)).empty());  // a newcomer has built nothing here
	(void)host;
	(void)bob;
	(void)carol;
}

TEST("orphan pieces: the host reloads a save from before its second chair: the chair goes for everyone, the first stays, and a joiner gets only the first")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(1, Built(Chair(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Built(Chair(0xFF000002, 200.0f)));
	Sync(f, 1);
	CHECK((Others(Received(f, 2)) == std::map<Key, bool>{ { { host, 0xFF000001 }, true }, { { host, 0xFF000002 }, true } }));

	// The host's game loads its save, asks for the world again, and is told what it built; it lacks the second chair.
	Load(f, 1, HOST_A);
	const auto own = Decoded(Received(f, 1), MT::kOwnWorkshopItems, Protocol::DecodeWorkshopItems);
	REQUIRE(own.size() == 1);
	REQUIRE(own[0].size() == 2);
	const auto second = own[0][0].refId == 0xFF000002 ? own[0][0] : own[0][1];
	f.fake->Deliver(1, GoneReport({ second }));
	Sync(f, 1);
	CHECK(f.LoggedSoon("no longer has 1 of the pieces kept as theirs"));
	CHECK((Others(Received(f, 2)) == std::map<Key, bool>{ { { host, 0xFF000002 }, false } }));

	JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	CHECK((Others(Received(f, 3)) == std::map<Key, bool>{ { { host, 0xFF000001 }, true } }));

	// Said again (a second load): nothing more goes, nothing is relayed.
	f.fake->Deliver(1, GoneReport({ second }));
	Sync(f, 1);
	CHECK(Others(Received(f, 2)).empty());
	CHECK(f.LoggedTimes("no longer has") == 1);
}

TEST("orphan pieces: a joiner's lost piece goes the same way, and a gone report takes out only its sender's own building")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	const auto bob = JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(1, Built(Chair(0xFF000001, 100.0f)));
	f.fake->Deliver(2, Built(Chair(0xFF000001, 300.0f)));  // the same reference ID in Bob's game: his own chair
	f.fake->Deliver(2, Built(Chair(0xFF000002, 400.0f)));
	Sync(f, 1);
	Sync(f, 2);
	Clear(f, { 1, 2 });

	// Bob's game says it lacks 0xFF000001 and 0xFF000002, and (as no game would) a piece of the world the host scrapped.
	auto worldPiece = Chair(0x0001A2B4, 50.0f);
	worldPiece.op = Protocol::WorkshopOp::kScrapped;
	f.fake->Deliver(1, Built(worldPiece));
	Sync(f, 1);
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, GoneReport({ Chair(0xFF000001, 300.0f), Chair(0xFF000002, 400.0f), worldPiece }));
	Sync(f, 2);
	CHECK((Others(Received(f, 1)) == std::map<Key, bool>{ { { bob, 0xFF000001 }, false }, { { bob, 0xFF000002 }, false } }));

	JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	const auto carolSees = Others(Received(f, 3));
	CHECK((carolSees.contains({ host, 0xFF000001 })));   // the host's chair under the same ID stays
	CHECK((carolSees.contains({ host, 0x0001A2B4 })));   // and the world's piece the host scrapped stays scrapped
	CHECK((!carolSees.contains({ bob, 0xFF000001 })));
	CHECK((!carolSees.contains({ bob, 0xFF000002 })));
}

TEST("orphan pieces: an offline player's building stays: nobody else's report takes it out, and their own list waits for their return")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	const auto bob = JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(2, Built(Chair(0xFF000B01, 200.0f)));
	f.fake->Deliver(2, Built(Chair(0xFF000B02, 300.0f)));
	Sync(f, 2);
	f.fake->Drop(2);
	REQUIRE(f.LoggedSoon(std::format("player {} left", bob)));
	Clear(f, { 1 });

	// The host's game, reloading, says it lacks Bob's IDs (it never had them): Bob's pieces are not the host's to take out.
	Load(f, 1, HOST_A);
	CHECK(Own(Received(f, 1)).empty());
	f.fake->Deliver(1, GoneReport({ Chair(0xFF000B01, 200.0f) }));
	Sync(f, 1);
	CHECK(!f.Logged("no longer has"));

	JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	const auto carolSees = Others(Received(f, 3));
	CHECK((carolSees.contains({ bob, 0xFF000B01 })));
	CHECK((carolSees.contains({ bob, 0xFF000B02 })));

	// Bob comes back: his welcome brings his list, for his game to look over.
	CHECK(JoinFriend(f, 4, "Bob", BOB, BOB_A) == bob);
	CHECK((Own(Received(f, 4)) == std::set<std::uint32_t>{ 0xFF000B01, 0xFF000B02 }));
	(void)host;
}

TEST("orphan pieces: a builder who leaves halfway through saying what they lack keeps the rest until their next welcome")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	JoinFriend(f, 2, "Bob", BOB, BOB_A);
	for (std::uint32_t i = 1; i <= 3; ++i) {
		f.fake->Deliver(1, Built(Chair(0xFF000000 + i, 100.0f * static_cast<float>(i))));
	}
	Sync(f, 1);
	Clear(f, { 1, 2 });

	// Its game lacks all three, but only one report gets out before the host's game drops.
	f.fake->Deliver(1, GoneReport({ Chair(0xFF000003, 300.0f) }));
	f.fake->Drop(1);
	REQUIRE(f.LoggedSoon(std::format("player {} left", host)));
	CHECK((Others(Received(f, 2)) == std::map<Key, bool>{ { { host, 0xFF000003 }, false } }));

	JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	const auto carolSees = Others(Received(f, 3));
	CHECK((carolSees.contains({ host, 0xFF000001 })));
	CHECK((carolSees.contains({ host, 0xFF000002 })));
	CHECK((!carolSees.contains({ host, 0xFF000003 })));

	// Back, its game is told of the two again and can say so then.
	CHECK(JoinHost(f, 4) == host);
	const auto back = Received(f, 4);
	CHECK((Own(back) == std::set<std::uint32_t>{ 0xFF000001, 0xFF000002 }));
	CHECK((Own(back, true) == std::set<std::uint32_t>{ 0xFF000003 }));  // and the one it lacked, as lost: it may have it again
}

TEST("orphan pieces: what a gone report takes out comes off its builder's share, and is no longer saved with the session")
{
	TempSessionFile file;
	auto            options = Hosted();
	options.sessionFile = file.path.string();
	options.maxWorkshopItemsPerPlayer = 2;
	std::uint32_t bob = 0;
	{
		Fixture f{ options };
		JoinHost(f, 1);
		bob = JoinFriend(f, 2, "Bob", BOB, BOB_A);
		f.fake->Deliver(2, Built(Chair(0xFF000001, 100.0f)));
		f.fake->Deliver(2, Built(Chair(0xFF000002, 200.0f)));
		f.fake->Deliver(2, Built(Chair(0xFF000003, 300.0f)));  // past his share: not kept
		Sync(f, 2);
		f.fake->Deliver(2, GoneReport({ Chair(0xFF000002, 200.0f) }));
		f.fake->Deliver(2, Built(Chair(0xFF000004, 400.0f)));  // within his share again
		Sync(f, 2);
		Clear(f, { 2 });
		Load(f, 2, BOB_A);
		CHECK((Own(Received(f, 2)) == std::set<std::uint32_t>{ 0xFF000001, 0xFF000004 }));
	}
	Fixture f{ options };
	CHECK(f.LoggedSoon("2 built"));
	JoinHost(f, 1);
	CHECK((Others(Received(f, 1)) == std::map<Key, bool>{ { { bob, 0xFF000001 }, true }, { { bob, 0xFF000004 }, true } }));
	// Whose world built them, and what it lacked, are kept too: Bob's world is asked about his own again, his lost chair
	// among them, and a second character of his about none.
	CHECK(f.Logged("1 lost by their builders"));
	CHECK(JoinFriend(f, 2, "Bob", BOB, BOB_A) == bob);
	const auto own = Received(f, 2);
	CHECK((Own(own) == std::set<std::uint32_t>{ 0xFF000001, 0xFF000004 }));
	CHECK((Own(own, true) == std::set<std::uint32_t>{ 0xFF000002 }));
	f.fake->Drop(2);
	REQUIRE(f.LoggedSoon(std::format("player {} left", bob)));
	CHECK(JoinFriend(f, 3, "Bob", BOB, BOB_B) == bob);
	CHECK(Own(Received(f, 3)).empty());
}

// The player ID is the PC's, not the character's: a friend who built with one character and joins with another (or a new
// game, or an alt on a dedicated server) had the first's list sent to a world that has none of it, and every piece went
// for everyone, for good.
TEST("orphan pieces: a second character of the same player is sent none of the first's pieces, and its game can take none of them out")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	const auto bob = JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(2, Built(Chair(0xFF000B01, 200.0f)));
	f.fake->Deliver(2, Built(Chair(0xFF000B02, 300.0f)));
	Sync(f, 2);
	f.fake->Drop(2);
	REQUIRE(f.LoggedSoon(std::format("player {} left", bob)));

	// Back with his second character: the same player ID, nothing of his to look for.
	CHECK(JoinFriend(f, 3, "Bob", BOB, BOB_B) == bob);
	CHECK(Own(Received(f, 3)).empty());
	CHECK(Own(Received(f, 3), true).empty());
	Clear(f, { 1 });
	// Even a game that said the first's pieces were gone (as the build before this did) takes nothing out.
	f.fake->Deliver(3, GoneReport({ Chair(0xFF000B01, 200.0f), Chair(0xFF000B02, 300.0f) }));
	Sync(f, 3);
	CHECK(!f.Logged("no longer has"));
	CHECK(Others(Received(f, 1)).empty());
	// What he builds as the second is that world's.
	f.fake->Deliver(3, Built(Chair(0xFF000C01, 400.0f)));
	Sync(f, 3);
	Load(f, 3, BOB_B);
	CHECK((Own(Received(f, 3)) == std::set<std::uint32_t>{ 0xFF000C01 }));

	// And back as the first, his first's pieces, and only them.
	f.fake->Drop(3);
	for (int i = 0; i < 1500 && f.LoggedTimes(std::format("player {} left", bob)) < 2; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	REQUIRE(f.LoggedTimes(std::format("player {} left", bob)) == 2);
	CHECK(JoinFriend(f, 4, "Bob", BOB, BOB_A) == bob);
	CHECK((Own(Received(f, 4)) == std::set<std::uint32_t>{ 0xFF000B01, 0xFF000B02 }));
	JoinFriend(f, 5, "Carol", CAROL, CAROL_A);
	const auto carolSees = Others(Received(f, 5));
	CHECK((carolSees.contains({ bob, 0xFF000B01 })));
	CHECK((carolSees.contains({ bob, 0xFF000B02 })));
	CHECK((carolSees.contains({ bob, 0xFF000C01 })));
	(void)host;
}

TEST("orphan pieces: the list waits for the record that says whose world it is, and what was built before it takes that world")
{
	Fixture    f{ Hosted() };
	JoinHost(f, 1);
	const auto bob = JoinFriend(f, 2, "Bob", BOB, 0);  // welcomed, no record yet
	f.fake->Deliver(2, Built(Chair(0xFF000B01, 200.0f)));
	Sync(f, 2);
	CHECK(Own(Received(f, 2)).empty());
	// Nor is a gone report taken before it: the answer to a list sent for the world before a load.
	f.fake->Deliver(2, GoneReport({ Chair(0xFF000B01, 200.0f) }));
	Sync(f, 2);
	CHECK(!f.Logged("no longer has"));
	f.fake->Deliver(2, Record(BOB_A));
	CHECK((Own(Received(f, 2)) == std::set<std::uint32_t>{ 0xFF000B01 }));

	// A load: until its record comes, its world isn't known; its gone reports wait for that.
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	f.fake->Deliver(2, GoneReport({ Chair(0xFF000B01, 200.0f) }));
	Sync(f, 2);
	CHECK(!f.Logged("no longer has"));
	f.Advance(std::chrono::milliseconds(300));
	f.fake->Deliver(2, Record(BOB_B));  // another character's save: none of it is that world's
	Sync(f, 2);
	CHECK(Own(Received(f, 2)).empty());
	(void)bob;
}

TEST("orphan pieces: a lost piece its builder's world has again (a later save) comes back for everyone")
{
	Fixture    f{ Hosted() };
	const auto host = JoinHost(f, 1);
	JoinFriend(f, 2, "Bob", BOB, BOB_A);
	f.fake->Deliver(1, Built(Chair(0xFF000001, 100.0f)));
	f.fake->Deliver(1, Built(Chair(0xFF000002, 200.0f)));
	Sync(f, 1);
	f.fake->Deliver(1, GoneReport({ Chair(0xFF000002, 200.0f) }));  // an older save loaded
	Sync(f, 1);
	REQUIRE(f.LoggedSoon("no longer has 1 of the pieces"));
	Clear(f, { 1, 2 });

	// The newer save loaded again: the server's list has the chair as lost, and the host's game finds it standing.
	Load(f, 1, HOST_A);
	const auto own = Received(f, 1);
	CHECK((Own(own) == std::set<std::uint32_t>{ 0xFF000001 }));
	CHECK((Own(own, true) == std::set<std::uint32_t>{ 0xFF000002 }));
	f.fake->Deliver(1, Built(Chair(0xFF000002, 250.0f)));  // reported placed again, where it stands
	Sync(f, 1);
	CHECK(f.LoggedSoon("has FF000002 again"));
	CHECK((Others(Received(f, 2)) == std::map<Key, bool>{ { { host, 0xFF000002 }, true } }));
	JoinFriend(f, 3, "Carol", CAROL, CAROL_A);
	const auto carolSees = Others(Received(f, 3));
	CHECK((carolSees.contains({ host, 0xFF000001 })));
	CHECK((carolSees.contains({ host, 0xFF000002 })));
	Load(f, 1, HOST_A);
	CHECK(Own(Received(f, 1), true).empty());
}
