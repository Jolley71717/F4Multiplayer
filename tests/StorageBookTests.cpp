// The server's book of shared storage: who keeps a home's or a settlement's container, what it holds, and whether a
// friend's take or put at their copy may go through. Whatever the book can't check is refused.

#include "Test.h"

#include "StorageBook.h"

namespace
{
	using StorageBook::Book;
	using StorageBook::Listed;
	using StorageBook::Verdict;
	using StorageBook::Viewed;

	constexpr std::uint32_t HOST = 1;
	constexpr std::uint32_t FRIEND = 2;
	constexpr std::uint32_t OTHER = 3;
	constexpr std::uint32_t CHEST = 0x0001'2345;
	constexpr std::uint32_t BOX = 0x0001'2346;
	constexpr std::uint32_t STIMPAK = 0x0002'3736;
	constexpr std::uint32_t WATER = 0x0004'835D;

	Protocol::StorageListing Listing(std::uint32_t a_container, std::uint32_t a_asOf, std::vector<Protocol::StorageStack> a_stacks)
	{
		return { a_container, a_asOf, std::move(a_stacks) };
	}

	Protocol::ContainerChange Take(std::uint32_t a_item, std::int32_t a_count, std::uint32_t a_container = CHEST)
	{
		return { a_container, a_item, -a_count };
	}

	Protocol::ContainerChange Put(std::uint32_t a_item, std::int32_t a_count, std::uint32_t a_container = CHEST)
	{
		return { a_container, a_item, a_count };
	}

	// A session's numbered changes, as the server keeps them.
	struct Log
	{
		std::vector<Protocol::IndexedContainerChange> changes;

		Protocol::IndexedContainerChange Add(std::uint32_t a_player, const Protocol::ContainerChange& a_change)
		{
			changes.push_back({ static_cast<std::uint32_t>(changes.size()), a_player, a_change });
			return changes.back();
		}

		[[nodiscard]] std::uint32_t Next() const { return static_cast<std::uint32_t>(changes.size()); }
	};

	// The host lists a chest of 5 stimpaks, and a friend opens it.
	Book Opened(Log& a_log)
	{
		Book book;
		CHECK(book.List(HOST, true, Listing(CHEST, a_log.Next(), { { STIMPAK, 5 } }), a_log.changes) == Listed::kTaken);
		CHECK(book.Open(FRIEND, CHEST, a_log.Next()).kind == Viewed::Kind::kShown);
		return book;
	}
}

TEST("storage book: only the host's game claims a container, and a claim lists what it holds")
{
	Log  log;
	Book book;
	bool changed = false;
	CHECK(book.List(FRIEND, false, Listing(CHEST, 0, { { STIMPAK, 5 } }), log.changes, &changed) == Listed::kNotHost);
	CHECK(!changed);
	CHECK(!book.KeeperOf(CHEST));
	CHECK(book.List(HOST, true, Listing(CHEST, 0, { { STIMPAK, 5 }, { WATER, 2 } }), log.changes, &changed) == Listed::kTaken);
	CHECK(changed);
	CHECK(book.KeeperOf(CHEST) == HOST);
	CHECK(book.Held(CHEST, STIMPAK) == 5);
	CHECK(book.Held(CHEST, WATER) == 2);
	CHECK(book.List(HOST, true, Listing(CHEST, 0, { { STIMPAK, 4 } }), log.changes, &changed) == Listed::kTaken);
	CHECK(!changed);  // listed again: the same owner, nothing for the games to hear
	CHECK(book.Held(CHEST, STIMPAK) == 4);
	CHECK(book.Held(CHEST, WATER) == 0);  // a listing is the whole of it
}

TEST("storage book: a container nobody keeps is an ordinary one, and the keeper's own moves always go through")
{
	Log  log;
	Book book;
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kNotStorage);
	book.List(HOST, true, Listing(CHEST, 0, {}), log.changes);
	CHECK(book.Check(HOST, Take(STIMPAK, 99)) == Verdict::kKeeper);
	CHECK(book.Check(HOST, { CHEST, Protocol::CONTAINER_EMPTIED, 0 }) == Verdict::kKeeper);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1, BOX)) == Verdict::kNotStorage);
}

TEST("storage book: a friend's move counts only while their menu is open on it")
{
	Log  log;
	Book book;
	book.List(HOST, true, Listing(CHEST, 0, { { STIMPAK, 5 } }), log.changes);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kNotViewing);  // their copy's own old contents: theirs, not the chest's
	CHECK(book.Check(FRIEND, Put(WATER, 1)) == Verdict::kNotViewing);
	const auto shown = book.Open(FRIEND, CHEST, 7);
	CHECK(shown.kind == Viewed::Kind::kShown);
	CHECK(shown.contents == Listing(CHEST, 7, { { STIMPAK, 5 } }));
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kAccept);
	CHECK(book.Check(OTHER, Take(STIMPAK, 1)) == Verdict::kNotViewing);
	book.Close(FRIEND, CHEST);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kNotViewing);
}

TEST("storage book: a take of more than the chest holds, or of what it lacks, is refused, and an emptied marker never goes")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 5)) == Verdict::kAccept);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 6)) == Verdict::kNotHeld);
	CHECK(book.Check(FRIEND, Take(WATER, 1)) == Verdict::kNotHeld);
	CHECK(book.Check(FRIEND, { CHEST, Protocol::CONTAINER_EMPTIED, 0 }) == Verdict::kNotHeld);
}

TEST("storage book: two friends taking the last stimpaks: the first numbered gets them, the second is refused")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Open(OTHER, CHEST, log.Next()).kind == Viewed::Kind::kShown);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 4)) == Verdict::kAccept);
	CHECK(book.Numbered(log.Add(FRIEND, Take(STIMPAK, 4))));
	CHECK(book.Held(CHEST, STIMPAK) == 1);
	CHECK(book.Check(OTHER, Take(STIMPAK, 4)) == Verdict::kNotHeld);
	CHECK(book.Check(OTHER, Take(STIMPAK, 1)) == Verdict::kAccept);
}

TEST("storage book: puts add to what the chest holds, and a put the keeper's game can't hold is refused")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Numbered(log.Add(FRIEND, Put(WATER, 3))));
	CHECK(book.Held(CHEST, WATER) == 3);
	CHECK(book.Check(FRIEND, Take(WATER, 3)) == Verdict::kAccept);
	const auto lacksWater = [](std::uint32_t a_keeper, std::uint32_t a_item) { return !(a_keeper == HOST && a_item == WATER); };
	CHECK(book.Check(FRIEND, Put(WATER, 1), lacksWater) == Verdict::kCantHold);
	CHECK(book.Check(FRIEND, Put(STIMPAK, 1), lacksWater) == Verdict::kAccept);
	CHECK(book.Check(FRIEND, Take(WATER, 1), lacksWater) == Verdict::kAccept);  // a take needs nothing of the keeper's game
}

TEST("storage book: a listing counts the friends' moves its keeper's world hadn't had yet, and not the keeper's own")
{
	Log  log;
	auto book = Opened(log);
	const auto asOf = log.Next();
	log.Add(FRIEND, Take(STIMPAK, 2));          // numbered, but the host's world lists before applying it
	log.Add(HOST, Take(STIMPAK, 1));            // the host's own: in its listing already
	log.Add(FRIEND, Put(WATER, 1, BOX));        // another container
	log.Add(FRIEND, { CHEST, Protocol::CONTAINER_EMPTIED, 0 });
	CHECK(book.List(HOST, true, Listing(CHEST, asOf, { { STIMPAK, 4 } }), log.changes) == Listed::kTaken);
	CHECK(book.Held(CHEST, STIMPAK) == 2);
	CHECK(book.Held(CHEST, WATER) == 0);
	// a listing as of a change the server never numbered counts nothing it doesn't have
	CHECK(book.List(HOST, true, Listing(CHEST, 9999, { { STIMPAK, 4 } }), log.changes) == Listed::kTaken);
	CHECK(book.Held(CHEST, STIMPAK) == 4);
}

TEST("storage book: when the keeper's world reloads or they leave, the chest closes to friends until it is listed again")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Open(OTHER, CHEST, log.Next()).kind == Viewed::Kind::kShown);
	const auto closed = book.Stale(HOST);
	CHECK(closed.size() == 2);
	CHECK(book.Viewers(CHEST).empty());
	CHECK(!book.Fresh(CHEST));
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kClosed);
	CHECK(book.Check(FRIEND, Put(STIMPAK, 1)) == Verdict::kClosed);
	CHECK(book.Open(FRIEND, CHEST, log.Next()).kind == Viewed::Kind::kClosed);
	CHECK(book.Viewers(CHEST).empty());
	CHECK(book.KeeperOf(CHEST) == HOST);  // still the host's: a friend's copy never becomes the true one
	book.List(HOST, true, Listing(CHEST, log.Next(), { { STIMPAK, 3 } }), log.changes);
	CHECK((book.Open(FRIEND, CHEST, log.Next()).contents.stacks == std::vector<Protocol::StorageStack>{ { STIMPAK, 3 } }));

	const auto gone = book.Left(HOST);
	CHECK(gone.size() == 1);
	CHECK(gone[0].container == CHEST);
	CHECK(gone[0].viewer == FRIEND);
	CHECK(book.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kClosed);
}

TEST("storage book: a friend leaving stops viewing, and their own containers' listings go stale")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Left(FRIEND).empty());
	CHECK(book.Viewers(CHEST).empty());
	CHECK(book.Fresh(CHEST));
}

TEST("storage book: the keeper never views its own chest")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Open(HOST, CHEST, 0).kind == Viewed::Kind::kOwn);
	CHECK(book.Open(FRIEND, BOX, 0).kind == Viewed::Kind::kNotStorage);
	CHECK(book.Viewers(CHEST) == std::vector<std::uint32_t>{ FRIEND });
}

TEST("storage book: a numbered take finding the chest short tells the taker once, and nothing else passes for a shortfall")
{
	Log  log;
	auto book = Opened(log);
	const auto take = log.Add(FRIEND, Take(STIMPAK, 3));
	const auto own = log.Add(HOST, Take(STIMPAK, 1));
	const auto put = log.Add(FRIEND, Put(STIMPAK, 1));
	const auto refusal = book.Short(HOST, { take.index, 2 }, log.changes);
	REQUIRE(refusal);
	CHECK(refusal->first == FRIEND);
	CHECK((refusal->second == Protocol::StorageRefusal{ CHEST, STIMPAK, -2, take.index, Protocol::StorageReason::kShort }));
	CHECK(!book.Short(HOST, { take.index, 2 }, log.changes));   // once
	CHECK(!book.Short(HOST, { own.index, 1 }, log.changes));    // the keeper's own
	CHECK(!book.Short(HOST, { put.index, 1 }, log.changes));    // a put
	CHECK(!book.Short(FRIEND, { take.index, 1 }, log.changes)); // not the keeper
	CHECK(!book.Short(HOST, { 999, 1 }, log.changes));          // never numbered
	const auto small = log.Add(FRIEND, Take(STIMPAK, 1));
	CHECK(!book.Short(HOST, { small.index, 2 }, log.changes));  // more than was taken
}

TEST("storage book: what it holds never goes below nothing, and says so when a take found less")
{
	Log  log;
	auto book = Opened(log);
	CHECK(!book.Numbered(log.Add(HOST, Take(STIMPAK, 7))));
	CHECK(book.Held(CHEST, STIMPAK) == 0);
	CHECK(book.Numbered(log.Add(HOST, Put(STIMPAK, 1))));
	CHECK(book.Held(CHEST, STIMPAK) == 1);
}

TEST("storage book: a viewer is sent what it holds after each listing")
{
	Log  log;
	auto book = Opened(log);
	book.Numbered(log.Add(FRIEND, Put(WATER, 2)));
	const auto contents = book.ContentsOf(CHEST, log.Next());
	CHECK(contents == Listing(CHEST, log.Next(), { { STIMPAK, 5 }, { WATER, 2 } }));
	CHECK(book.ContentsOf(BOX, 3) == Listing(BOX, 3, {}));
}

TEST("storage book: owners are kept with the session and come back closed until listed")
{
	Log  log;
	auto book = Opened(log);
	book.List(HOST, true, Listing(BOX, 0, {}), log.changes);
	const auto owners = book.Owners();
	CHECK((owners == std::vector<Protocol::StorageOwner>{ { CHEST, HOST }, { BOX, HOST } }));

	Book restored;
	for (const auto& owner : owners) {
		restored.Restore(owner);
	}
	restored.Restore({ 0xFF00'0001, HOST });  // a runtime reference: never shared
	restored.Restore({ 0x0001'0000, 0 });     // nobody
	CHECK(restored.Owners() == owners);
	CHECK(!restored.Fresh(CHEST));
	CHECK(restored.Check(FRIEND, Take(STIMPAK, 1)) == Verdict::kClosed);
	CHECK(restored.Open(FRIEND, CHEST, 0).kind == Viewed::Kind::kClosed);
	restored.Clear();
	CHECK(restored.Size() == 0);
}

TEST("storage book: a host's game on another PC takes over the old host's containers")
{
	Log  log;
	Book book;
	book.Restore({ CHEST, 41 });
	bool changed = false;
	CHECK(book.List(HOST, true, Listing(CHEST, 0, { { WATER, 1 } }), log.changes, &changed) == Listed::kTaken);
	CHECK(changed);
	CHECK(book.KeeperOf(CHEST) == HOST);
}

TEST("storage book: a full book claims no new container, but lists the ones it has")
{
	Log  log;
	Book book(1);
	CHECK(book.List(HOST, true, Listing(CHEST, 0, {}), log.changes) == Listed::kTaken);
	CHECK(book.List(HOST, true, Listing(BOX, 0, {}), log.changes) == Listed::kFull);
	CHECK(book.List(HOST, true, Listing(CHEST, 0, { { WATER, 1 } }), log.changes) == Listed::kTaken);
	book.Restore({ BOX, HOST });
	CHECK(book.Size() == 1);
}

TEST("storage book: a put of a new item into a chest of the most stacks a listing holds is refused")
{
	Log  log;
	Book book;
	std::vector<Protocol::StorageStack> stacks;
	for (std::uint32_t i = 0; i < Protocol::MAX_STORAGE_STACKS; ++i) {
		stacks.push_back({ 0x0010'0000 + i, 1 });
	}
	book.List(HOST, true, Listing(CHEST, 0, stacks), log.changes);
	book.Open(FRIEND, CHEST, 0);
	CHECK(book.Check(FRIEND, Put(0x0010'0000, 1)) == Verdict::kAccept);
	CHECK(book.Check(FRIEND, Put(WATER, 1)) == Verdict::kCantHold);
}

TEST("storage book: a forfeit gives up no more than the chest holds while listed, all of it while stale, and none of the keeper's own")
{
	Log  log;
	auto book = Opened(log);
	CHECK(book.Forfeit(FRIEND, Take(STIMPAK, 3)) == 3);
	CHECK(book.Forfeit(FRIEND, Take(STIMPAK, 9)) == 5);  // the rest was taken by others meanwhile
	CHECK(book.Forfeit(FRIEND, Take(WATER, 2)) == 0);    // none there to give up
	CHECK(book.Forfeit(FRIEND, Put(STIMPAK, 2)) == 0);   // a forfeit only ever takes
	CHECK(book.Forfeit(HOST, Take(STIMPAK, 1)) == 0);    // the keeper's game takes from its own chest itself
	CHECK(book.Forfeit(FRIEND, Take(STIMPAK, 1, BOX)) == 0);  // nobody keeps it
	CHECK(book.Forfeit(FRIEND, Take(Protocol::CONTAINER_EMPTIED, 1)) == 0);
	book.Stale(HOST);
	CHECK(book.Forfeit(FRIEND, Take(STIMPAK, 9)) == 9);  // the keeper's world applies what is there
}

TEST("storage book: a container is kept from the first change numbered after its claim, and that survives the session file")
{
	Log  log;
	log.Add(FRIEND, Take(WATER, 1, BOX));
	log.Add(FRIEND, Take(WATER, 1, BOX));
	Book book;
	CHECK(book.Since(BOX) == 0);
	CHECK(book.List(HOST, true, Listing(BOX, log.Next(), { { WATER, 3 } }), log.changes) == Listed::kTaken);
	CHECK(book.Since(BOX) == 2);
	log.Add(FRIEND, Take(WATER, 1, BOX));
	book.List(HOST, true, Listing(BOX, log.Next(), { { WATER, 2 } }), log.changes);
	CHECK(book.Since(BOX) == 2);  // listed again: still kept from the claim
	const auto owners = book.Owners();
	CHECK(owners.size() == 1);
	CHECK(owners[0].since == 2);
	Book restored;
	restored.Restore(owners[0]);
	CHECK(restored.Since(BOX) == 2);
}
