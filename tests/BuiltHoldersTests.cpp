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

	std::vector<Protocol::StorageRefusal> Refusals(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	}

	// A save whose session record holds a_bytes, read as WorldSync's LoadResyncPoint reads it (ResyncRecord::Decode): one
	// that doesn't read is no record at all, and our copies of built containers are unsure.
	Game::Save Reread(Game::Save a_save, std::uint32_t a_version, std::span<const std::uint8_t> a_bytes)
	{
		auto record = ResyncRecord::Decode(a_version, a_bytes);
		if (!record) {
			a_save.point = {};
			a_save.waiting.clear();
			a_save.book = {};
			a_save.book.MarkUnsure();
			return a_save;
		}
		a_save.point.sessionId = record->session;
		a_save.point.containerFrom = record->next;
		a_save.waiting = std::move(record->waiting);
		a_save.book = std::move(record->book);
		a_save.version = a_version;
		return a_save;
	}

	// The host has built a crate, both games are in the session, and the friend's game has its copy.
	struct Pair
	{
		Fixture       f;
		Game          host{ 1, 0xA11CE }, friendGame{ 2, 0xB0B };
		std::uint32_t hostId = 0;

		explicit Pair(Server::Options a_options = {}) :
			f{ a_options }
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

	// A copy never shown (another ID: one a save from before built containers were shared kept) holds only our player's
	// own: it is given all of the book, and what it held stays theirs.
	BuiltHolders::Book other;
	REQUIRE(other.Load(book.Record()));
	const auto deltas = other.Show(key, COPY_AGAIN, { { STIM, 1 } });
	using Delta = std::pair<std::uint32_t, std::int64_t>;
	CHECK((deltas == BuiltHolders::Book::Deltas{ Delta{ AMMO, 30 }, Delta{ STIM, 4 } }));
	CHECK((other.Going(COPY_AGAIN).items == BuiltHolders::Book::Items{ { AMMO, 30 }, { STIM, 4 } }));  // the one of its own stays

	// Unsure (a record that didn't read), the book can't tell a copy holding some of it already: what it holds of the book
	// is taken to be the book's, so nothing is given twice.
	BuiltHolders::Book unsure;
	REQUIRE(unsure.Load(book.Record()));
	unsure.MarkUnsure();
	CHECK((unsure.Show(key, COPY_AGAIN, { { STIM, 1 } }) == BuiltHolders::Book::Deltas{ Delta{ AMMO, 30 }, Delta{ STIM, 3 } }));

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
	CHECK((going.items == BuiltHolders::Book::Items{ { STIM, 3 } }));
	CHECK(book.Going(COPY).items.empty());
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

	// A piece the session doesn't know is nobody's to change: refused, as the friend named it, so their game undoes it.
	Clear(f, { 2 });
	f.fake->Deliver(2, Protocol::Encode(Protocol::ContainerChange{ 0xFF000777, STIM, 1, host }, MT::kReportContainer));
	CHECK(Changes(Received(f, 1)).empty());
	const auto refused = Refusals(Received(f, 2));
	REQUIRE(refused.size() == 1);
	CHECK((refused[0] == Protocol::StorageRefusal{ 0xFF000777, STIM, 1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed, host }));

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

TEST("built holders: a move at a built crate past the player's event budget is refused, each one, not dropped")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host");
	JoinAs(f, 2, "Friend");
	Build(f, 1, PIECE);
	Clear(f, { 1, 2 });
	constexpr std::size_t MOVES = 560;  // past the 500 a second
	for (std::size_t i = 0; i < MOVES; ++i) {
		f.fake->Deliver(2, Protocol::Encode(Protocol::ContainerChange{ PIECE, STIM, 1, host }, MT::kReportContainer));
	}
	const auto got = Received(f, 2);
	const auto numbered = Changes(got).size();
	const auto refused = Refusals(got);
	CHECK(numbered + refused.size() == MOVES);  // every move numbered (and echoed) or refused
	CHECK(numbered <= 500);
	CHECK(!refused.empty());
	for (const auto& refusal : refused) {
		CHECK((refusal == Protocol::StorageRefusal{ PIECE, STIM, 1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kCantHold, host }));
	}
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

// ---- a copy's removal, every way it goes: only our player's own share comes to them -----------------------------

TEST("built holders: the book keeps each copy's share through the piece's emptying, an ID used again and the save's record")
{
	BuiltHolders::Book      book;
	const BuiltHolders::Key key{ 1, PIECE };
	book.Apply(key, STIM, 5);
	CHECK(book.Show(key, COPY, {}).size() == 1);
	book.Forget(key);  // scrapped while the copy waits to go
	BuiltHolders::Book saved;
	REQUIRE(saved.Load(book.Record()));
	CHECK(!saved.Unsure());

	// A new piece under the same ID, and a new copy of it: the old copy's share stays its own.
	saved.Apply(key, STIM, 2);
	CHECK(saved.Show(key, COPY_AGAIN, {}).size() == 1);
	const auto old = saved.Going(COPY);
	CHECK(old.known);
	CHECK((old.items == BuiltHolders::Book::Items{ { STIM, 5 } }));
	const auto fresh = saved.Going(COPY_AGAIN);
	CHECK((fresh.items == BuiltHolders::Book::Items{ { STIM, 2 } }));

	// A copy the book knows nothing of is our player's alone, unless the book is unsure.
	CHECK(saved.Going(0xFF000B00).known);
	saved.MarkUnsure();
	CHECK(!saved.Going(0xFF000B00).known);
	BuiltHolders::Book reread;
	REQUIRE(reread.Load(saved.Record()));
	CHECK(reread.Unsure());
	CHECK(!reread.Load({ 7 }));
	CHECK(reread.Unsure());  // a record that doesn't read: unsure
}

TEST("built holders: a copy the book has no room for leaves it unsure, so no copy it can't follow gives anything")
{
	BuiltHolders::Book      book;
	const BuiltHolders::Key key{ 1, PIECE };
	book.Apply(key, STIM, 1);
	for (std::uint32_t i = 0; i < BuiltHolders::Book::MAX_COPIES; ++i) {
		(void)book.Show(key, 0xFF100000 + i, {});
	}
	CHECK(!book.Unsure());
	CHECK(book.Show(key, 0xFF200000, {}).empty());  // not followed: shows nothing
	CHECK(book.Unsure());
	CHECK(!book.Going(0xFF200000).known);
}

TEST("built holders: at the session's end the friend's copy goes and only the friend's own things come to them")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	p.friendGame.holders[COPY][AMMO] = 4;  // the friend's own, the session never saw
	p.friendGame.Disconnect(p.f);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
	CHECK(p.friendGame.Carried(AMMO) == 4);
	CHECK(p.friendGame.loot.Stats().builtRescued == 1);
}

TEST("built holders: a copy that goes out of a session as its cell loads, after the game started again, gives nothing of the host's")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	p.friendGame.Put(p.f, COPY, STIM, 3);  // the friend's own put: the session's too (it is in the host's crate)
	p.Settle();
	REQUIRE(p.host.In(PIECE, STIM) == 9);
	p.friendGame.holders[COPY][AMMO] = 2;
	const auto save = p.friendGame.SaveGame(p.f);
	p.friendGame.Disconnect(p.f);
	p.friendGame.Restart(save);  // the game started again; no session
	// Its cell loads: the graveyard's sweep removes it.
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 7);  // 10 less the 3 put into the host's crate
	CHECK(p.friendGame.Carried(AMMO) == 2);
}

TEST("built holders: a save's leftover copy of a piece the host scrapped since goes after the load with none of the host's things for the friend")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 5);
	p.Settle();
	REQUIRE(p.friendGame.In(COPY, STIM) == 5);
	const auto save = p.friendGame.SaveGame(p.f);

	// The host scraps the crate (its scrap puts what it held into the host's workbench) while the friend is elsewhere.
	p.friendGame.away.insert(COPY);
	p.host.built.erase(PIECE);
	Scrap(p.f, 1, PIECE);
	p.Settle();

	// The friend loads the save: its copy stands again, holding the five, and no report claims it (SavedCopies' quiet
	// window), so it goes.
	p.friendGame.LoadGame(p.f, save);
	p.Settle();
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
}

TEST("built holders: the host's scrap empties the friend's copy, here or not, and a new piece under the same ID starts empty")
{
	for (const bool here : { true, false }) {
		Pair p;
		p.host.Put(p.f, PIECE, STIM, 6);
		p.Settle();
		p.friendGame.holders[COPY][AMMO] = 1;  // the friend's own
		if (!here) {
			p.friendGame.away.insert(COPY);
		}

		p.host.built.erase(PIECE);
		Scrap(p.f, 1, PIECE);
		p.Settle();
		CHECK(p.friendGame.loot.Built().Of({ p.hostId, PIECE }).empty());  // the book starts again
		if (here) {
			CHECK(p.friendGame.In(COPY, STIM) == 0);  // the scrap's emptying took the host's things out of the live copy
			p.friendGame.RemoveCopy(COPY);              // then the relayed scrap removes it
			CHECK(p.friendGame.Carried(STIM) == 10);
			CHECK(p.friendGame.Carried(AMMO) == 1);
		} else {
			CHECK(p.friendGame.In(COPY, STIM) == 6);  // not loaded: what it shows stays until it goes
		}

		// The host's game gives a new crate the same ID; the friend's game makes its copy. Nothing of the old one's is in either.
		p.host.holders.erase(PIECE);
		p.host.built[PIECE] = { 0, PIECE };
		Build(p.f, 1, PIECE);
		p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
		p.friendGame.OpenMenu(p.f, COPY_AGAIN);
		p.Settle();
		CHECK(p.host.In(PIECE, STIM) == 0);
		CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 0);
		p.host.Put(p.f, PIECE, STIM, 2);
		p.Settle();
		CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 2);

		// A joiner's game is told the old piece's loot and its emptying, then the new one's: its book holds the new one's alone.
		Game joiner{ 3, 0xCA201 };
		joiner.Join(p.f, "Joiner");
		joiner.Receive(p.f);
		CHECK((joiner.loot.Built().Of({ p.hostId, PIECE }) == BuiltHolders::Book::Items{ { STIM, 2 } }));

		if (!here) {
			// The old copy's cell loads and it goes: the host's six come out of it, the friend's own ammo comes to them.
			p.friendGame.away.erase(COPY);
			p.friendGame.RemoveCopy(COPY);
			CHECK(p.friendGame.Carried(STIM) == 10);
			CHECK(p.friendGame.Carried(AMMO) == 1);
			CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 2);
		}
	}
}

TEST("built holders: the builder's game lets go of a change that waited for its piece once the piece's emptying comes")
{
	Pair p;
	p.host.away.insert(PIECE);  // the host's crate isn't loaded in the host's game
	p.friendGame.Put(p.f, COPY, STIM, 3);
	p.Settle();
	REQUIRE(p.host.In(PIECE, STIM) == 0);  // waits for the crate to load

	// The crate goes (scrapped, or the host's world lacks it), and the host's game gives a new one the same ID.
	p.host.built.erase(PIECE);
	Scrap(p.f, 1, PIECE);
	p.Settle();
	p.host.holders.erase(PIECE);
	p.host.built[PIECE] = { 0, PIECE };
	p.host.away.erase(PIECE);
	Build(p.f, 1, PIECE);
	p.host.Frame(p.f);
	p.Settle();
	CHECK(p.host.In(PIECE, STIM) == 0);  // the friend's put was into the old crate, not this one
	CHECK(p.host.loot.Stats().builtStale == 1);
	CHECK(p.host.loot.Waiting().empty());
}

TEST("built holders: a move at a crate the host scrapped a moment before is refused, and the friend's game undoes it")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	REQUIRE(p.friendGame.In(COPY, STIM) == 6);

	// The host scraps the crate; before the friend's game hears of it, the friend takes 5 from the copy.
	p.host.built.erase(PIECE);
	Scrap(p.f, 1, PIECE);
	Sync(p.f, 1);
	p.friendGame.Take(p.f, COPY, STIM, 5);
	REQUIRE(p.friendGame.Carried(STIM) == 15);
	p.Settle();
	CHECK(p.friendGame.builtRefusals == 1);
	CHECK(p.friendGame.loot.Stats().builtRefused == 1);
	CHECK(p.friendGame.Carried(STIM) == 10);  // the five went with the crate, to the host's workbench: not to the friend too
	CHECK(p.friendGame.In(COPY, STIM) == 0);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
}

TEST("built holders: a friend's put past their share is refused and comes back to them, and the copy shows the crate again")
{
	Server::Options options;
	options.maxContainerChangesPerPlayer = 1;
	Pair p{ options };
	p.host.Put(p.f, PIECE, STIM, 6);  // the host's one
	p.Settle();
	p.friendGame.Take(p.f, COPY, STIM, 1);  // the friend's one
	p.Settle();
	REQUIRE(p.host.In(PIECE, STIM) == 5);
	REQUIRE(p.friendGame.Carried(STIM) == 11);

	p.friendGame.Put(p.f, COPY, STIM, 4);
	p.Settle();
	CHECK(p.friendGame.builtRefusals == 1);
	CHECK(p.friendGame.Carried(STIM) == 11);
	CHECK(p.friendGame.In(COPY, STIM) == 5);
	CHECK(p.host.In(PIECE, STIM) == 5);
	CHECK((p.friendGame.loot.Built().Of({ p.hostId, PIECE }) == BuiltHolders::Book::Items{ { STIM, 5 } }));
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 11);  // the copy shows only the host's five: nothing of theirs is left in it
}

TEST("built holders: the builder's own move the session can't keep (its table full) is refused and undone in the crate")
{
	Server::Options options;
	options.maxWorldEntries = 1;
	Pair p{ options };
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	p.host.Put(p.f, PIECE, STIM, 2);
	p.Settle();
	CHECK(p.host.builtRefusals == 1);
	CHECK(p.host.In(PIECE, STIM) == 6);
	CHECK(p.host.Carried(STIM) == 4);
	CHECK(p.friendGame.In(COPY, STIM) == 6);

	// A take refused the same way: the items go back into the crate.
	p.host.Take(p.f, PIECE, STIM, 3);
	p.Settle();
	CHECK(p.host.builtRefusals == 2);
	CHECK(p.host.In(PIECE, STIM) == 6);
	CHECK(p.host.Carried(STIM) == 4);
}

TEST("built holders: a move at a copy the session can't name (an item of a plugin it lacks) is undone as it never went")
{
	Pair p;
	p.friendGame.untranslatable.insert(AMMO);
	p.friendGame.carried[AMMO] = 5;
	p.friendGame.Put(p.f, COPY, AMMO, 5);
	p.Settle();
	CHECK(p.friendGame.Carried(AMMO) == 5);
	CHECK(p.friendGame.In(COPY, AMMO) == 0);
	CHECK(p.friendGame.loot.Built().Of({ p.hostId, PIECE }).empty());
	CHECK(p.friendGame.loot.Stats().builtRefused == 1);
}

TEST("built holders: a change at a piece of another of our characters' worlds goes nowhere and is counted")
{
	Game game{ 2, 0xB0B };
	game.holderFacts[PIECE] = { .kind = LootScope::Holder::kContainer, .storage = true };
	game.built[PIECE] = { 0, PIECE };  // this world's own piece under the same ID
	game.loot.OnWelcome(0x5E55, 2);
	game.loot.Live({ 0, 1, { PIECE, STIM, 5, Protocol::BUILT_ELSEWHERE } }, game.now, game);
	CHECK(game.loot.Stats().builtElsewhere == 1);
	CHECK(game.In(PIECE, STIM) == 0);
	CHECK(game.loot.Built().Pieces() == 0);
	CHECK(game.loot.Waiting().empty());
}

TEST("built holders: a copy away at the host's scrap gives up the old piece's things when it goes, though a new piece took the ID")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	p.friendGame.away.insert(COPY);
	p.host.built.erase(PIECE);
	Scrap(p.f, 1, PIECE);
	p.Settle();

	// The host's game gives a new crate the same ID and puts 2 in; the friend's game makes its copy.
	p.host.holders.erase(PIECE);
	p.host.built[PIECE] = { 0, PIECE };
	Build(p.f, 1, PIECE);
	p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
	p.host.Put(p.f, PIECE, STIM, 2);
	p.Settle();
	CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 2);

	// The old copy's cell loads and the graveyard removes it.
	p.friendGame.away.erase(COPY);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
	CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 2);
}

TEST("built holders: when the save's record of built containers doesn't read, a copy that goes gives the friend nothing, and says so")
{
	for (const bool cutInBook : { true, false }) {
		Pair p;
		p.host.Put(p.f, PIECE, STIM, 5);
		p.Settle();
		p.friendGame.holders[COPY][AMMO] = 2;
		auto       save = p.friendGame.SaveGame(p.f);
		const auto& loot = p.friendGame.loot;
		REQUIRE(save.version == ResyncRecord::VERSION_2);
		// The co-save's bytes, cut short: in the book (the rest reads), or in the header (none of it does).
		auto bytes = ResyncRecord::Encode(loot.Point(), loot.Waiting(), loot.Built(), ResyncRecord::VERSION_2);
		bytes.resize(cutInBook ? bytes.size() - 4 : 10);
		const auto read = Reread(save, ResyncRecord::VERSION_2, bytes);
		CHECK(read.book.Unsure());
		CHECK((read.point.sessionId == p.friendGame.sessionId) == cutInBook);
		p.friendGame.Disconnect(p.f);
		p.friendGame.Restart(read);
		p.friendGame.RemoveCopy(COPY);
		CHECK(p.friendGame.Carried(STIM) == 10);
		CHECK(p.friendGame.Carried(AMMO) == 0);
		CHECK(p.friendGame.loot.Stats().builtUnknown == 1);
	}
}

TEST("built holders: the session record is written as version 1, the old layout, while nothing in it needs version 2")
{
	Pair p;
	p.host.holders[CHEST][AMMO] = 10;
	p.friendGame.holders[CHEST][AMMO] = 10;
	p.friendGame.away.insert(CHEST);
	p.host.Take(p.f, CHEST, AMMO, 4);  // waits in the friend's game for the chest to load
	p.Settle();
	REQUIRE(p.friendGame.loot.Waiting().size() == 1);
	const auto& loot = p.friendGame.loot;
	CHECK(ResyncRecord::VersionFor(loot.Waiting(), loot.Built()) == ResyncRecord::VERSION_1);
	const auto save = p.friendGame.SaveGame(p.f);
	CHECK(save.version == ResyncRecord::VERSION_1);

	// Byte for byte what every build before wrote (and reads): the session, the first change lacked, and each waiting
	// change's index, container, item and count.
	std::vector<std::uint8_t> old;
	const auto put = [&](auto a_value) {
		const auto at = old.size();
		old.resize(at + sizeof(a_value));
		std::memcpy(old.data() + at, &a_value, sizeof(a_value));
	};
	const auto& waiting = loot.Waiting()[0];
	put(loot.Point().sessionId);
	put(loot.Point().containerFrom);
	put(std::uint32_t{ 1 });
	put(waiting.index);
	put(CHEST);
	put(AMMO);
	put(std::int32_t{ -4 });
	CHECK(ResyncRecord::Encode(loot.Point(), loot.Waiting(), loot.Built(), ResyncRecord::VERSION_1) == old);
	const auto read = ResyncRecord::Decode(ResyncRecord::VERSION_1, old);
	REQUIRE(read);
	CHECK(read->session == p.friendGame.sessionId);
	REQUIRE(read->waiting.size() == 1);
	CHECK((read->waiting[0].change == Protocol::ContainerChange{ CHEST, AMMO, -4 }));
	CHECK(read->bookRead);
	CHECK(read->book.Pieces() == 0);
	CHECK(!read->book.Unsure());
	CHECK(!ResyncRecord::Decode(3, old));  // a version from after this build: not read

	// The crate's loot, booked: now it needs version 2.
	p.host.Put(p.f, PIECE, STIM, 1);
	p.Settle();
	CHECK(p.friendGame.SaveGame(p.f).version == ResyncRecord::VERSION_2);
}

TEST("built holders: a save from before built containers were shared still gives the friend what they kept in a copy")
{
	Pair p;
	p.friendGame.holders[COPY][STIM] = 2;  // the friend's own, put in before the session shared built containers
	p.friendGame.holders[COPY][AMMO] = 3;
	auto save = p.friendGame.SaveGame(p.f);
	REQUIRE(save.version == ResyncRecord::VERSION_1);

	// The record such a build wrote: the session, the first change lacked, nothing waiting, and no book.
	std::vector<std::uint8_t> old(16);
	const auto                session = save.point.sessionId;
	const auto                next = save.point.containerFrom;
	std::memcpy(old.data(), &session, 8);
	std::memcpy(old.data() + 8, &next, 4);
	const auto read = Reread(save, ResyncRecord::VERSION_1, old);
	CHECK(!read.book.Unsure());
	CHECK(read.point.sessionId == session);
	p.friendGame.LoadGame(p.f, read);
	p.Settle();

	// The host puts into the crate: the copy, new to the book, shows all of it besides the friend's own.
	p.host.Put(p.f, PIECE, STIM, 5);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 7);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 12);  // their own two, none of the host's five
	CHECK(p.friendGame.Carried(AMMO) == 3);
	CHECK(p.friendGame.loot.Stats().builtUnknown == 0);
}

// ---- a copy made again: shown the book with nobody touching the piece ----------------------------------------------------
// (The rig, 2026-11-28: the friend left and joined again, and their game's new copy of the host's crate held nothing against
// the host's 7 until the host put another in. The session's changes at the piece were in the friend's book already, so the
// join replayed none of them, and nothing showed the book to the new copy.)

TEST("built holders: a copy made again on a rejoin shows what the piece holds, though nobody changes it after")
{
	for (const bool madeBeforeTheJoin : { false, true }) {
		Pair p;
		p.host.Put(p.f, PIECE, STIM, 7);
		p.Settle();
		REQUIRE(p.friendGame.In(COPY, STIM) == 7);

		// The friend leaves: the session's end takes the copy, and only the friend's own things come to them.
		p.friendGame.Disconnect(p.f);
		p.friendGame.RemoveCopy(COPY);
		CHECK(p.friendGame.Carried(STIM) == 10);

		// The friend joins again (the same session: its changes are in the book already, none come again). Their game makes
		// its copy again, before the join's world state or after it.
		if (madeBeforeTheJoin) {
			p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
		}
		p.friendGame.Join(p.f, "Friend");
		p.Settle();
		if (!madeBeforeTheJoin) {
			p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
			p.Settle();
		}
		CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 7);
		CHECK(p.friendGame.loot.Stats().builtCaughtUp == 1);

		// Brought on once: the next change moves it by that change alone, and its going hands the friend none of the host's.
		p.host.Put(p.f, PIECE, STIM, 1);
		p.Settle();
		CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 8);
		CHECK(p.host.In(PIECE, STIM) == 8);
		p.friendGame.Disconnect(p.f);
		p.friendGame.RemoveCopy(COPY_AGAIN);
		CHECK(p.friendGame.Carried(STIM) == 10);
	}
}

TEST("built holders: a joiner's copy shows the host's count whether it is made before the replay comes or after")
{
	for (const bool madeBeforeTheReplay : { false, true }) {
		Pair p;
		p.host.Put(p.f, PIECE, STIM, 7);
		p.friendGame.Put(p.f, COPY, STIM, 3);
		p.Settle();
		REQUIRE(p.host.In(PIECE, STIM) == 10);

		Game joiner{ 3, 0xCA201 };
		joiner.holderFacts[COPY] = { .kind = LootScope::Holder::kContainer, .storage = true };
		if (madeBeforeTheReplay) {
			joiner.MakeCopy(COPY, { p.hostId, PIECE });
		}
		joiner.Join(p.f, "Joiner");
		joiner.Receive(p.f);
		REQUIRE((joiner.loot.Built().Of({ p.hostId, PIECE }) == BuiltHolders::Book::Items{ { STIM, 10 } }));
		if (!madeBeforeTheReplay) {
			joiner.MakeCopy(COPY, { p.hostId, PIECE });
			joiner.Receive(p.f);
		}
		CHECK(joiner.In(COPY, STIM) == 10);
		joiner.Receive(p.f);
		CHECK(joiner.In(COPY, STIM) == 10);  // shown once, not again
	}
}

TEST("built holders: a friend who loads a save out of the session and joins again sees the host's count in the copy made again")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 5);
	p.Settle();
	const auto save = p.friendGame.SaveGame(p.f);
	p.friendGame.Disconnect(p.f);
	p.friendGame.RemoveCopy(COPY);
	p.host.Put(p.f, PIECE, STIM, 2);  // while the friend is away
	p.host.Receive(p.f);

	// The game started again on that save; its cell loads out of the session and the copy the save kept goes.
	p.friendGame.Restart(save);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
	p.friendGame.Join(p.f, "Friend");
	p.Settle();
	p.friendGame.MakeCopy(COPY_AGAIN, { p.hostId, PIECE });
	p.Settle();
	CHECK(p.friendGame.In(COPY_AGAIN, STIM) == 7);
	CHECK(p.host.In(PIECE, STIM) == 7);
}

TEST("built holders: a copy away as the piece changes shows the change once it is back, its menu never opened")
{
	Pair p;
	p.host.Put(p.f, PIECE, STIM, 6);
	p.Settle();
	REQUIRE(p.friendGame.In(COPY, STIM) == 6);

	// Its cell unloads, the host takes some; it loads again.
	p.friendGame.away.insert(COPY);
	p.host.Take(p.f, PIECE, STIM, 2);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 6);  // not here: it can't be changed
	p.friendGame.away.erase(COPY);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 4);

	// Away again while the host takes the rest (the book has nothing of the piece then): back, it holds none of it.
	p.friendGame.away.insert(COPY);
	p.host.Take(p.f, PIECE, STIM, 4);
	p.Settle();
	REQUIRE(p.friendGame.loot.Built().Of({ p.hostId, PIECE }).empty());
	p.friendGame.away.erase(COPY);
	p.Settle();
	CHECK(p.friendGame.In(COPY, STIM) == 0);
	p.friendGame.RemoveCopy(COPY);
	CHECK(p.friendGame.Carried(STIM) == 10);
}

TEST("built holders: the book says which copies are behind it, a copy found anew only while the book holds anything of its piece")
{
	BuiltHolders::Book book;
	const BuiltHolders::Key key{ 1, PIECE };
	CHECK(!book.Behind(key, COPY));  // nothing to show
	book.Apply(key, STIM, 4);
	CHECK(book.Behind(key, COPY));  // found anew: none of the book in it
	CHECK((book.Keys() == std::vector<BuiltHolders::Key>{ key }));
	CHECK((book.Show(key, COPY, {}) == BuiltHolders::Book::Deltas{ { STIM, 4 } }));
	CHECK(!book.Behind(key, COPY));
	book.Apply(key, STIM, -4);  // the piece emptied while the copy shows 4
	CHECK(book.Pieces() == 0);
	CHECK(book.Behind(key, COPY));
	CHECK((book.Keys() == std::vector<BuiltHolders::Key>{ key }));  // known by the copy that still shows it
	CHECK((book.Show(key, COPY, { { STIM, 4 } }) == BuiltHolders::Book::Deltas{ { STIM, -4 } }));
	CHECK(!book.Behind(key, COPY));
	CHECK(book.Keys().empty());
}
