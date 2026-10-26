// Shared storage, a game's side: who keeps a container, what a friend's copy shows while its menu is open, and what a
// refused move undoes.

#include "Test.h"

#include "game/StorageView.h"

namespace
{
	using StorageView::Adjust;
	using StorageView::Keeper;
	using StorageView::Side;

	constexpr std::uint32_t US = 7;
	constexpr std::uint32_t FRIEND = 2;
	constexpr std::uint32_t CHEST = 0x0001'2345;
	constexpr std::uint32_t BOX = 0x0001'2346;
	constexpr std::uint32_t STIMPAK = 0x0002'3736;
	constexpr std::uint32_t WATER = 0x0004'835D;

	const auto ours = [](std::uint32_t a_id) { return a_id == US; };

	Protocol::StorageOwners Owned(std::uint32_t a_container, std::uint32_t a_owner)
	{
		return { { { a_container, a_owner } } };
	}

	Side Friend()
	{
		Side side;
		side.OnWelcome(true, false);
		side.Owners(Owned(CHEST, FRIEND));
		return side;
	}
}

TEST("storage view: who keeps a container: nobody without the rule, the owner's game, or each player while unclaimed")
{
	Side side;
	side.OnWelcome(false, false);
	side.Owners(Owned(CHEST, FRIEND));
	CHECK(side.KeeperOf(CHEST, true, ours) == Keeper::kNone);

	side.OnWelcome(true, false);
	side.Owners(Owned(CHEST, FRIEND));
	side.Owners(Owned(BOX, US));
	CHECK(side.KeeperOf(CHEST, true, ours) == Keeper::kFriend);
	CHECK(side.KeeperOf(BOX, true, ours) == Keeper::kUs);
	CHECK(side.KeeperOf(0x99, true, ours) == Keeper::kLocal);
	CHECK(side.KeeperOf(0x99, false, ours) == Keeper::kNone);

	Side host;
	host.OnWelcome(true, true);
	CHECK(host.KeeperOf(0x99, true, ours) == Keeper::kNone);  // the host's own, until its game claims it

	side.OnDisconnected();
	CHECK(side.KeeperOf(CHEST, true, ours) == Keeper::kNone);
	CHECK(side.OwnerCount() == 0);
}

TEST("storage view: a new owner takes over the entry")
{
	Side side = Friend();
	side.Owners(Owned(CHEST, US));
	CHECK(side.OwnerOf(CHEST) == US);
	CHECK(side.OwnerCount() == 1);
}

TEST("storage view: reconcile gives the changes that make the copy show what it should")
{
	CHECK(StorageView::Reconcile({}, {}).empty());
	CHECK((StorageView::Reconcile({ { STIMPAK, 5 } }, { { STIMPAK, 2 }, { WATER, 3 } }) == std::vector<Adjust>{ { STIMPAK, 3 }, { WATER, -3 } }));
	CHECK((StorageView::Reconcile({ { WATER, 1 } }, { { WATER, 1 } }).empty()));
	CHECK((StorageView::Reconcile({ { WATER, 0 } }, { { WATER, 4 } }) == std::vector<Adjust>{ { WATER, -4 } }));
	CHECK((StorageView::Reconcile({ { WATER, 1'000'000'000'000 } }, {}) == std::vector<Adjust>{ { WATER, Protocol::MAX_ITEM_COUNT } }));
}

TEST("storage view: one view at a time; its changes wait for the contents, then count from where they left off")
{
	Side side = Friend();
	CHECK(!side.ViewFrom(CHEST));
	CHECK(side.Open(CHEST));
	CHECK(!side.Open(CHEST));
	CHECK(side.ViewFrom(CHEST) == StorageView::NOT_YET);
	CHECK(!side.Contents({ BOX, 3, {} }));  // an answer for another container

	const auto want = side.Contents({ CHEST, 40, { { STIMPAK, 5 } } });
	REQUIRE(want);
	CHECK((*want == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 5 } }));
	CHECK(side.ViewFrom(CHEST) == 40u);
	CHECK(side.Stats().views == 1);

	CHECK(side.Refresh(CHEST));
	CHECK(side.ViewFrom(CHEST) == StorageView::NOT_YET);
	CHECK(!side.Refresh(BOX));

	CHECK(side.Close() == CHEST);
	CHECK(!side.Close());
	CHECK(!side.Viewing());
	CHECK(!side.Contents({ CHEST, 41, { { STIMPAK, 5 } } }));  // the answer to a view since closed
}

TEST("storage view: contents count our moves on their way, so a redraw doesn't undo them")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, STIMPAK, -2 });
	side.Reported({ CHEST, WATER, 1 });
	side.Reported({ BOX, WATER, 9 });
	const auto want = side.Contents({ CHEST, 10, { { STIMPAK, 5 } } });
	REQUIRE(want);
	CHECK((*want == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 3 }, { WATER, 1 } }));

	// Once numbered, the server's contents count them, and our copy no longer does.
	side.OwnNumbered({ 10, FRIEND, { CHEST, STIMPAK, -2 } });
	side.OwnNumbered({ 11, FRIEND, { CHEST, WATER, 1 } });
	CHECK(side.InFlight() == 1);
	const auto after = side.Contents({ CHEST, 12, { { STIMPAK, 3 }, { WATER, 1 } } });
	REQUIRE(after);
	CHECK((*after == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 3 }, { WATER, 1 } }));
}

TEST("storage view: a refused take comes back out of our player, and into our copy while we still view it")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, STIMPAK, -2 });
	const auto undo = side.Refused({ CHEST, STIMPAK, -2, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kNotHeld });
	CHECK(undo.matched);
	CHECK(!undo.closeView);
	CHECK(undo.refresh);
	CHECK(undo.player == -2);
	CHECK(undo.copy == 2);
	CHECK(side.Stats().refusedTakes == 1);
	CHECK(side.InFlight() == 0);

	// The same refusal again matches nothing: undone once.
	CHECK(!side.Refused({ CHEST, STIMPAK, -2, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kNotHeld }).matched);
	CHECK(side.Stats().unmatched == 1);
}

TEST("storage view: a refused put goes back to our player; after the menu closed our copy is left alone")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, WATER, 3 });
	side.Close();
	const auto undo = side.Refused({ CHEST, WATER, 3, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kCantHold });
	CHECK(undo.matched);
	CHECK(undo.player == 3);
	CHECK(undo.copy == 0);
	CHECK(!undo.refresh);
	CHECK(side.Stats().refusedPuts == 1);
}

TEST("storage view: the server closing our view closes the menu; for another container it does nothing")
{
	Side side = Friend();
	side.Open(CHEST);
	CHECK(!side.Refused({ BOX, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }).closeView);
	const auto undo = side.Refused({ CHEST, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed });
	CHECK(undo.closeView);
	CHECK(undo.player == 0);
	CHECK(side.Stats().closedByHost == 1);
}

TEST("storage view: a shortfall takes back only what a numbered take of ours never found, once")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, STIMPAK, -4 });
	side.OwnNumbered({ 50, FRIEND, { CHEST, STIMPAK, -4 } });
	// Not ours (another index), another item, or more than we took: left alone.
	CHECK(!side.Refused({ CHEST, STIMPAK, -3, 51, Protocol::StorageReason::kShort }).matched);
	CHECK(!side.Refused({ CHEST, WATER, -3, 50, Protocol::StorageReason::kShort }).matched);
	CHECK(!side.Refused({ CHEST, STIMPAK, -5, 50, Protocol::StorageReason::kShort }).matched);
	const auto undo = side.Refused({ CHEST, STIMPAK, -3, 50, Protocol::StorageReason::kShort });
	CHECK(undo.matched);
	CHECK(undo.player == -3);
	CHECK(undo.copy == 0);  // the redraw shows our copy as it is
	CHECK(undo.refresh);
	CHECK(side.Stats().shortTakes == 1);
	CHECK(!side.Refused({ CHEST, STIMPAK, -3, 50, Protocol::StorageReason::kShort }).matched);
}

TEST("storage view: a load forgets our moves and view, and lists everything again; a welcome forgets the owners too")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, STIMPAK, -1 });
	side.MarkListed(BOX);
	side.OnLoad();
	CHECK(!side.Viewing());
	CHECK(side.InFlight() == 0);
	CHECK(!side.Listed(BOX));
	CHECK(side.OwnerOf(CHEST) == FRIEND);
	CHECK(!side.Refused({ CHEST, STIMPAK, -1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kNotHeld }).matched);

	side.OnWelcome(true, false);
	CHECK(!side.OwnerOf(CHEST));
}

TEST("storage view: the host's listings: each once, again after a shortfall or a world request")
{
	Side side;
	side.OnWelcome(true, true);
	CHECK(!side.Listed(CHEST));
	side.MarkListed(CHEST);
	side.MarkListed(BOX);
	CHECK(side.Listed(CHEST));
	side.Unlist(CHEST);
	CHECK(!side.Listed(CHEST));
	CHECK(side.Listed(BOX));
	side.UnlistAll();
	CHECK(!side.Listed(BOX));
	CHECK(side.Stats().listed == 2);
}

TEST("storage view: what it remembers is capped: the oldest move in flight goes first")
{
	Side side = Friend();
	for (std::uint32_t i = 0; i < Side::MAX_IN_FLIGHT + 3; ++i) {
		side.Reported({ CHEST, STIMPAK + i, -1 });
	}
	CHECK(side.InFlight() == Side::MAX_IN_FLIGHT);
	CHECK(side.Stats().inFlightLost == 3);
	CHECK(!side.Refused({ CHEST, STIMPAK, -1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kNotHeld }).matched);
	CHECK(side.Refused({ CHEST, STIMPAK + 3, -1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kNotHeld }).matched);

	Side many;
	many.OnWelcome(true, false);
	for (std::uint32_t i = 0; i < Side::MAX_OWNERS + 5; ++i) {
		many.Owners(Owned(0x10000 + i, FRIEND));
	}
	CHECK(many.OwnerCount() == Side::MAX_OWNERS);
}
