// Containers players built, in the session's loot (game/BuiltHolders.h, Protocol::ContainerChange::builder): the key a
// change names a piece by on each side, the server numbering and replaying such changes, each game's book of the pieces
// others built and its copies showing it, and a copy's removal handing over nothing twice. A plugin's container stays as
// it was.

#include "LootGame.h"

#include "SessionFile.h"

#include <cstring>

using LootTest::Game;
using namespace TestServer;

namespace
{
	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t CRATE_BASE = 0x0010C3C3;
	constexpr std::uint32_t CHEST = 0x0001B2C4;      // a plugin's container
	constexpr std::uint32_t PIECE = 0xFF000800;      // the host's crate, in the host's game
	constexpr std::uint32_t COPY = 0xFF000A01;       // the friend's copy of it
	constexpr std::uint32_t COPY_AGAIN = 0xFF000A02;  // ...made again (a load, a move to another cell)
	constexpr std::uint32_t STIM = 0x00023736;
	constexpr std::uint32_t AMMO = 0x0001F276;

	Protocol::WorkshopItem Crate(std::uint32_t a_ref)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.base = CRATE_BASE;
		item.workshop = BENCH;
		item.position[0] = 100.0f;
		return item;
	}

	void Build(Fixture& a_f, PeerId a_peer, std::uint32_t a_ref)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Crate(a_ref), MT::kReportWorkshopItem));
	}

	void Scrap(Fixture& a_f, PeerId a_peer, std::uint32_t a_ref)
	{
		auto item = Crate(a_ref);
		item.op = Protocol::WorkshopOp::kScrapped;
		a_f.fake->Deliver(a_peer, Protocol::Encode(item, MT::kReportWorkshopItem));
	}

	std::vector<Protocol::IndexedContainerChange> Changes(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	}

	// The host has built a crate, both games are in the session, and the friend's game has its copy.
	struct Pair
	{
		Fixture       f;
		Game          host{ 1, 0xA11CE }, friendGame{ 2, 0xB0B };
		std::uint32_t hostId = 0;

		Pair()
		{
			for (auto* game : { &host, &friendGame }) {
				game->holderFacts[PIECE] = game->holderFacts[COPY] = game->holderFacts[COPY_AGAIN] = { .kind = LootScope::Holder::kContainer, .storage = true };
			}
			hostId = host.Join(f, "Host");
			friendGame.Join(f, "Friend");
			host.built[PIECE] = { 0, PIECE };
			Build(f, 1, PIECE);
			friendGame.MakeCopy(COPY, { hostId, PIECE });
			host.carried[STIM] = 10;
			friendGame.carried[STIM] = 10;
		}

		void Settle()
		{
			for (int i = 0; i < 2; ++i) {
				host.Receive(f);
				friendGame.Receive(f);
			}
		}
	};
}

// ---- the wire -------------------------------------------------------------------------------------------------------

TEST("built holders: a plugin's container's change is the same bytes as before, a built one's carries its builder")
{
	const Protocol::ContainerChange plain{ CHEST, STIM, 3 };
	const auto                      bytes = Protocol::Encode(plain, MT::kReportContainer);
	CHECK(bytes.size() == 13);  // type, container, item, count
	const auto back = Protocol::DecodeContainerChange(bytes);
	REQUIRE(back);
	CHECK(*back == plain);
	CHECK(!Protocol::IsBuiltHolder(*back));

	const Protocol::ContainerChange built{ PIECE, STIM, -2, 7 };
	const auto                      builtBytes = Protocol::Encode(built, MT::kReportContainer);
	CHECK(builtBytes.size() == 17);
	const auto builtBack = Protocol::DecodeContainerChange(builtBytes);
	REQUIRE(builtBack);
	CHECK(*builtBack == built);
	CHECK(Protocol::IsValid(*builtBack));

	const Protocol::IndexedContainerChange numbered{ 4, 2, built };
	const auto                             numberedBack = Protocol::DecodeIndexedContainerChange(Protocol::Encode(numbered));
	REQUIRE(numberedBack);
	CHECK(numberedBack->change == built);
	CHECK(numberedBack->index == 4);
}

TEST("built holders: a runtime reference without a builder, or a plugin's with one, is no valid change")
{
	CHECK((!Protocol::IsValid(Protocol::ContainerChange{ PIECE, STIM, 1 })));
	CHECK((!Protocol::IsValid(Protocol::ContainerChange{ CHEST, STIM, 1, 7 })));
	CHECK((!Protocol::IsValid(Protocol::ContainerChange{ 0xFF000000, STIM, 1, 7 })));
	CHECK((Protocol::IsValid(Protocol::ContainerChange{ CHEST, STIM, 1 })));
	CHECK((Protocol::IsValid(Protocol::ContainerChange{ PIECE, STIM, 1, Protocol::BUILT_ELSEWHERE })));
}

// ---- the key, both ways ---------------------------------------------------------------------------------------------

TEST("built holders: our own piece goes out under our player id and comes back as ours; a copy under its piece's key")
{
	using BuiltHolders::Whose;
	const auto own = BuiltHolders::ToWire({ 0, PIECE }, 3, STIM, 2);
	REQUIRE(own);
	CHECK(own->container == PIECE);
	CHECK(own->builder == 3);
	CHECK(BuiltHolders::WhoseIs(*own, 3) == Whose::kOurs);
	CHECK(BuiltHolders::WhoseIs(*own, 4) == Whose::kCopy);
	CHECK((BuiltHolders::KeyOf(*own) == BuiltHolders::Key{ 3, PIECE }));

	const auto copy = BuiltHolders::ToWire({ 5, PIECE }, 3, STIM, -1);
	REQUIRE(copy);
	CHECK(copy->builder == 5);
	CHECK(BuiltHolders::WhoseIs(*copy, 3) == Whose::kCopy);
	CHECK(BuiltHolders::WhoseIs(*copy, 5) == Whose::kOurs);

	CHECK((!BuiltHolders::ToWire({ 0, PIECE }, 0, STIM, 1)));  // no player id yet
	CHECK((BuiltHolders::WhoseIs({ PIECE, STIM, 1, Protocol::BUILT_ELSEWHERE }, 3) == Whose::kElsewhere));
	CHECK((BuiltHolders::WhoseIs({ CHEST, STIM, 1 }, 3) == Whose::kPlugin));
	CHECK((BuiltHolders::WhoseIs({ PIECE, STIM, 1, 3 }, 0) == Whose::kUnknown));
}

// ---- the book -------------------------------------------------------------------------------------------------------

TEST("built holders: the book takes no more than a piece holds, and a new copy is given all of it, one a save kept none")
{
	BuiltHolders::Book book;
	const BuiltHolders::Key key{ 1, PIECE };
	book.Apply(key, STIM, 5);
	book.Apply(key, STIM, -8);  // removeitem takes what there is
	CHECK(book.Pieces() == 0);
	book.Apply(key, STIM, 4);
	book.Apply(key, AMMO, 30);

	const auto fresh = book.Show(key, COPY, {});
	CHECK(fresh.size() == 2);
	CHECK((book.Show(key, COPY, { { STIM, 4 }, { AMMO, 30 } }).empty()));  // shown already: nothing more

	BuiltHolders::Book saved;
	REQUIRE(saved.Load(book.Record()));
	CHECK(saved.Of(key) == book.Of(key));
	CHECK((saved.Show(key, COPY, { { STIM, 4 }, { AMMO, 30 }, { 0x1234, 1 } }).empty()));  // the save's copy holds it already

	// A copy holding less than the book is given the rest; what the book can't explain stays.
	BuiltHolders::Book other;
	REQUIRE(other.Load(book.Record()));
	const auto deltas = other.Show(key, COPY, { { STIM, 1 } });
	using Delta = std::pair<std::uint32_t, std::int64_t>;
	CHECK((deltas == BuiltHolders::Book::Deltas{ Delta{ AMMO, 30 }, Delta{ STIM, 3 } }));

	CHECK((!saved.Load({ 1, 2 })));  // cut short
	CHECK(saved.Pieces() == 0);
}

TEST("built holders: a copy going hands back what it shows of the book, only that, and only once")
{
	BuiltHolders::Book book;
	const BuiltHolders::Key key{ 1, PIECE };
	book.Apply(key, STIM, 5);
	CHECK((book.Show(key, COPY, {}).size() == 1));
	book.Own(key, COPY, STIM, -2, 3);  // our player takes 2: the copy holds 3
	const auto going = book.Going(COPY);
	CHECK((going == BuiltHolders::Book::Items{ { STIM, 3 } }));
	CHECK(book.Going(COPY).empty());
	CHECK((book.Of(key) == BuiltHolders::Book::Items{ { STIM, 3 } }));  // the piece still holds them
}

// ---- the server -----------------------------------------------------------------------------------------------------

TEST("built holders: the server numbers a move at a built crate, names it to each game as theirs or the builder's, and replays it to a joiner")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host");
	const auto friendId = JoinAs(f, 2, "Friend");
	Build(f, 1, PIECE);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Protocol::Encode(Protocol::ContainerChange{ PIECE, STIM, 5, host }, MT::kReportContainer));
	const auto atFriend = Changes(Received(f, 2));
	REQUIRE(atFriend.size() == 1);
	CHECK((atFriend[0].change == Protocol::ContainerChange{ PIECE, STIM, 5, host }));
	const auto atHost = Changes(Received(f, 1));
	REQUIRE(atHost.size() == 1);
	CHECK(atHost[0].change.builder == host);  // their own

	// The friend's take at their copy names the host's piece.
	f.fake->Deliver(2, Protocol::Encode(Protocol::ContainerChange{ PIECE, STIM, -2, host }, MT::kReportContainer));
	const auto back = Changes(Received(f, 1));
	REQUIRE(back.size() == 1);
	CHECK(back[0].playerId == friendId);
	CHECK((back[0].change == Protocol::ContainerChange{ PIECE, STIM, -2, host }));

	// A piece the session doesn't know is nobody's to change.
	f.fake->Deliver(2, Protocol::Encode(Protocol::ContainerChange{ 0xFF000777, STIM, 1, host }, MT::kReportContainer));
	CHECK(Changes(Received(f, 1)).empty());

	JoinAs(f, 3, "Joiner");
	std::vector<Protocol::IndexedContainerChange> replayed;
	for (const auto& world : WorldStates(Received(f, 3))) {
		replayed.insert(replayed.end(), world.containerChanges.begin(), world.containerChanges.end());
	}
	REQUIRE(replayed.size() == 2);
	CHECK((replayed[0].change == Protocol::ContainerChange{ PIECE, STIM, 5, host }));
	CHECK((replayed[1].change == Protocol::ContainerChange{ PIECE, STIM, -2, host }));

	// Scrapped: its emptying is numbered, so no game's book keeps the old piece's loot under an ID used again.
	Clear(f, { 1, 2, 3 });
	Scrap(f, 1, PIECE);
	const auto emptied = Changes(Received(f, 3));
	REQUIRE(emptied.size() == 1);
	CHECK(emptied[0].change.item == Protocol::CONTAINER_EMPTIED);
	CHECK(emptied[0].change.builder == host);
	CHECK(emptied[0].playerId == 0);
}

TEST("built holders: the session file reaches the layout for built holders only when it holds one, and an older one with one is damaged")
{
	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 1;
	snapshot.containerChanges = { { 0, 1, { CHEST, STIM, 2 } } };
	auto plain = SessionFile::Encode(snapshot);
	std::uint16_t version = 0;
	std::memcpy(&version, plain.data() + 4, sizeof(version));
	CHECK(version < SessionFile::BUILT_HOLDERS);

	snapshot.containerChanges.push_back({ 1, 1, { PIECE, STIM, 2, 1 } });
	auto built = SessionFile::Encode(snapshot);
	std::memcpy(&version, built.data() + 4, sizeof(version));
	CHECK(version == SessionFile::BUILT_HOLDERS);
	const auto read = SessionFile::Decode(built);
	REQUIRE(read);
	REQUIRE(read->containerChanges.size() == 2);
	CHECK((read->containerChanges[1].change == Protocol::ContainerChange{ PIECE, STIM, 2, 1 }));

	const std::uint16_t old = SessionFile::BUILT_HOLDERS - 1;
	std::memcpy(built.data() + 4, &old, sizeof(old));
	CHECK(!SessionFile::Decode(built));
}

// ---- both games -----------------------------------------------------------------------------------------------------

TEST("built holders: the host's crate and the friend's copy hold the same loot, both ways")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 5);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 5);

	p.friendGame.Take(p.f, COPY, STIM, 2);
	p.Settle();
	CHECK(p.host.In(PIECE, STIM) == 3);
	CHECK(p.friendGame.In(COPY, STIM) == 3);
	CHECK(p.friendGame.Carried(STIM) == 12);

	p.friendGame.Put(p.f, COPY, STIM, 4);
	p.Settle();
	CHECK(p.host.In(PIECE, STIM) == 7);
	CHECK(p.friendGame.In(COPY, STIM) == 7);
	CHECK(p.host.loot.Stats().builtOurs == 2);
}

TEST("built holders: a copy made again shows what the piece holds, and one that goes hands its friend nothing of the host's")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	REQUIRE(p.friendGame.In(COPY, STIM) == 6);

	// The copy goes (a move to another cell, a scrap at the friend's end): only the friend's own things in it come out.
	p.friendGame.holders[COPY][AMMO] = 9;  // put in before the session shared built containers
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);  // none of the host's six
	CHECK(p.friendGame.Carried(AMMO) == 9);

	// The host takes some while no copy stands; a new copy shows what is left.
	p.host.Take(p.f, PIECE, STIM, 1);
	p.Settle();
	p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
	p.host.Take(p.f, PIECE, STIM, 1);
	p.Settle();
	CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 4);
	CHECK(p.host.In(PIECE, STIM) == 4);
}

TEST("built holders: a copy not here when the change comes is given it as its menu opens")
{
	Pair p;
	p.friendGame.away.insert(COPY);
	p.host.Put(p.f, PIECE, STIM, 3);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 0);
	p.friendGame.away.erase(COPY);
	p.friendGame.OpenMenu(p.f, COPY);
	CHECK(p.friendGame.In(COPY, STIM) == 3);
}

TEST("built holders: the friend's book survives a save and a load, and the copy the save kept is given nothing twice")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 5);
	p.Settle();
	const auto save = p.friendGame.SaveGame(p.f);
	p.friendGame.LoadGame(p.f, save);
	p.Settle();
	p.host.Put(p.f, PIECE, STIM, 1);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 6);
}

TEST("built holders: a plugin's container shares loot as it always did")
{
	Pair p;
	p.host.holders[CHEST][AMMO] = 50;
	p.friendGame.holders[CHEST][AMMO] = 50;
	p.host.Take(p.f, CHEST, AMMO, 20);
	p.Settle();
	CHECK(p.friendGame.In(CHEST, AMMO) == 30);
	CHECK(p.friendGame.loot.Stats().builtApplied == 0);
}
