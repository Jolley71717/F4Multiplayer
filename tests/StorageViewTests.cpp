// Shared storage, a game's side: who keeps a container, what a friend's copy shows while its menu is open, what a
// refused or unanswered move undoes, and what a load does again.

#include "Test.h"

#include "game/StorageView.h"

namespace
{
	using StorageView::Adjust;
	using StorageView::Clock;
	using StorageView::Keeper;
	using StorageView::Side;

	constexpr std::uint32_t US = 7;
	constexpr std::uint32_t FRIEND = 2;
	constexpr std::uint64_t SESSION = 0x5E55'1011;
	constexpr std::uint32_t CHEST = 0x0001'2345;
	constexpr std::uint32_t BOX = 0x0001'2346;
	constexpr std::uint32_t STIMPAK = 0x0002'3736;
	constexpr std::uint32_t WATER = 0x0004'835D;
	const Clock::time_point T0{ std::chrono::hours{ 1 } };

	const auto ours = [](std::uint32_t a_id) { return a_id == US; };

	Protocol::StorageOwners Owned(std::uint32_t a_container, std::uint32_t a_owner)
	{
		return { { { a_container, a_owner } } };
	}

	Protocol::StorageRefusal Refusal(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count, Protocol::StorageReason a_reason = Protocol::StorageReason::kNotHeld)
	{
		return { a_container, a_item, a_count, Protocol::STORAGE_NOT_NUMBERED, a_reason };
	}

	Side Friend()
	{
		Side side;
		side.OnWelcome(true, false, SESSION, T0);
		side.Owners(Owned(CHEST, FRIEND));
		return side;
	}

	// An engine on two maps, as the tests' game has (tests/LootGame.h), for the Client's own rules.
	struct Engine final : StorageView::Engine
	{
		std::map<std::uint32_t, std::int64_t> copy;    // our copy of CHEST: all of it shared
		std::map<std::uint32_t, std::int64_t> player;
		std::vector<std::vector<std::uint8_t>> sent;
		std::vector<StorageView::Notice>       notices;
		std::uint32_t                          flushes = 0;
		std::uint32_t                          hides = 0;
		Clock::time_point                      now = T0;

		std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t) override
		{
			auto out = copy;
			std::erase_if(out, [](const auto& a_entry) { return a_entry.second <= 0; });
			return out;
		}

		std::int64_t ChangeCopy(std::uint32_t, std::uint32_t a_item, std::int64_t a_count) override { return Change(copy[a_item], a_count); }
		std::int64_t ChangePlayer(const StorageView::PlayerChange& a_change) override { return Change(player[a_change.Item()], a_change.Count()); }
		void         FlushLoot() override { ++flushes; }
		void         Send(std::vector<std::uint8_t> a_packet) override { sent.push_back(std::move(a_packet)); }
		void         Notify(StorageView::Notice a_notice) override { notices.push_back(a_notice); }
		void         HideMenu() override { ++hides; }
		Clock::time_point Now() override { return now; }

		static std::int64_t Change(std::int64_t& a_have, std::int64_t a_count)
		{
			const auto done = a_count < 0 ? -(std::min)(a_have, -a_count) : a_count;
			a_have += done;
			return done;
		}
	};
}

TEST("storage view: who keeps a container: nobody without the rule, else the owner's game; unclaimed storage is ordinary loot")
{
	Side side;
	side.OnWelcome(false, false, SESSION, T0);
	side.Owners(Owned(CHEST, FRIEND));
	CHECK(side.KeeperOf(CHEST, ours) == Keeper::kNone);

	side.OnWelcome(true, false, SESSION, T0);
	side.Owners(Owned(CHEST, FRIEND));
	side.Owners(Owned(BOX, US));
	CHECK(side.KeeperOf(CHEST, ours) == Keeper::kFriend);
	CHECK(side.KeeperOf(BOX, ours) == Keeper::kUs);
	CHECK(side.KeeperOf(0x99, ours) == Keeper::kNone);

	side.OnDisconnected();
	CHECK(side.KeeperOf(CHEST, ours) == Keeper::kNone);
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
	CHECK(!side.Refresh(CHEST));  // asked already: the answer to come covers it
	CHECK(!side.Refresh(BOX));

	const auto ended = side.Close();
	REQUIRE(ended);
	CHECK(ended->container == CHEST);
	CHECK(!ended->closed);
	CHECK(!side.Close());
	CHECK(!side.Viewing());
	CHECK(!side.Contents({ CHEST, 41, { { STIMPAK, 5 } } }));  // the answer to a view since closed
}

TEST("storage view: contents count our moves on their way, so a redraw doesn't undo them")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, STIMPAK, -2 }, T0);
	side.Reported({ CHEST, WATER, 1 }, T0);
	side.Reported({ BOX, WATER, 9 }, T0);
	const auto want = side.Contents({ CHEST, 10, { { STIMPAK, 5 } } });
	REQUIRE(want);
	CHECK((*want == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 3 }, { WATER, 1 } }));

	// Once numbered, the server's contents count them, and our copy no longer does.
	CHECK(!side.Own({ 10, US, { CHEST, STIMPAK, -2 } }, false));
	CHECK(!side.Own({ 11, US, { CHEST, WATER, 1 } }, true));  // on its way from this world: never done twice
	CHECK(side.InFlight() == 1);
	const auto after = side.Contents({ CHEST, 12, { { STIMPAK, 3 }, { WATER, 1 } } });
	REQUIRE(after);
	CHECK((*after == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 3 }, { WATER, 1 } }));
}

TEST("storage view: a refused take comes back out of our player, and into our copy while we still view it")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Contents({ CHEST, 1, {} });
	side.Reported({ CHEST, STIMPAK, -2 }, T0);
	const auto undo = side.Refused(Refusal(CHEST, STIMPAK, -2));
	CHECK(undo.matched);
	CHECK(!undo.closeView);
	CHECK(undo.refresh);
	CHECK(undo.player == -2);
	CHECK(undo.copy == 2);
	CHECK(side.Stats().refusedTakes == 1);
	CHECK(side.InFlight() == 0);

	// The same refusal again matches nothing: undone once.
	CHECK(!side.Refused(Refusal(CHEST, STIMPAK, -2)).matched);
	CHECK(side.Stats().unmatched == 1);
}

TEST("storage view: a refused put goes back to our player; after the menu closed our copy is left alone")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Reported({ CHEST, WATER, 3 }, T0);
	side.Close();
	const auto undo = side.Refused(Refusal(CHEST, WATER, 3, Protocol::StorageReason::kCantHold));
	CHECK(undo.matched);
	CHECK(undo.player == 3);
	CHECK(undo.copy == 0);
	CHECK(!undo.refresh);
	CHECK(side.Stats().refusedPuts == 1);
}

TEST("storage view: the server closing our view leaves it closed until the menu closes; for another container nothing")
{
	Side side = Friend();
	side.Open(CHEST);
	CHECK(!side.Refused({ BOX, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }).closeView);
	const auto undo = side.Refused({ CHEST, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed });
	CHECK(undo.closeView);
	CHECK(undo.player == 0);
	CHECK(side.Stats().closedByHost == 1);
	CHECK(side.Viewing() == CHEST);  // the menu is still open, on our own copy
	CHECK(!side.Shows(CHEST));
	CHECK(!side.ViewFrom(CHEST));  // nothing goes into it, and our moves there stay ours
	CHECK(!side.Refused({ CHEST, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }).closeView);
	const auto ended = side.Close();
	REQUIRE(ended);
	CHECK(ended->closed);
}

TEST("storage view: a shortfall takes back only what a numbered take of ours never found, once")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Contents({ CHEST, 1, {} });
	side.Reported({ CHEST, STIMPAK, -4 }, T0);
	CHECK(!side.Own({ 50, US, { CHEST, STIMPAK, -4 } }, false));
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

TEST("storage view: a numbered move of ours our loaded world lacks is done again on our player: a put taken, a take given")
{
	Side side = Friend();
	side.OnLoad(SESSION, {}, T0);
	const auto put = side.Own({ 20, US, { CHEST, WATER, 3 } }, true);
	REQUIRE(put);
	CHECK(put->player == -3);  // our loaded player still has the waters the chest holds now
	CHECK(put->copy == 0);
	const auto take = side.Own({ 21, US, { CHEST, STIMPAK, -2 } }, true);
	REQUIRE(take);
	CHECK(take->player == 2);  // the chest gave them up: our loaded player has them again
	CHECK(!side.Own({ 22, US, { CHEST, STIMPAK, -1 } }, false));  // in our world already
	CHECK(side.Stats().redone == 2);
}

TEST("storage view: a load keeps the save's unanswered moves: their numbers find them, so they aren't done twice")
{
	Side side = Friend();
	side.Reported({ CHEST, WATER, 3 }, T0);
	side.Reported({ CHEST, STIMPAK, -1 }, T0);
	const auto unanswered = side.Unanswered();
	CHECK(unanswered.size() == 2);
	CHECK(side.MovesSession() == SESSION);

	side.Reported({ CHEST, WATER, 5 }, T0);  // after the save
	side.OnLoad(SESSION, unanswered, T0);
	CHECK(side.InFlight() == 2);
	CHECK(!side.Own({ 30, US, { CHEST, WATER, 3 } }, true));   // the save's: in our world
	const auto later = side.Own({ 31, US, { CHEST, WATER, 5 } }, true);  // after it: done again
	REQUIRE(later);
	CHECK(later->player == -5);
	// The save's take was refused before the load: it never comes, and is undone in time.
	CHECK(side.Expired(T0 + StorageView::ANSWER_WITHIN - std::chrono::seconds{ 1 }).empty());
	const auto undone = side.Expired(T0 + StorageView::ANSWER_WITHIN);
	REQUIRE(undone.size() == 1);
	CHECK(undone[0].player == -1);  // the take comes back out of our player
	CHECK(side.InFlight() == 0);
}

TEST("storage view: an unanswered move is undone in time, and done again should its number come after all")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Contents({ CHEST, 1, {} });
	side.Reported({ CHEST, WATER, 4 }, T0);
	side.Reported({ CHEST, STIMPAK, -1 }, T0 + std::chrono::seconds{ 10 });
	auto undone = side.Expired(T0 + StorageView::ANSWER_WITHIN);
	REQUIRE(undone.size() == 1);
	CHECK(undone[0].player == 4);   // the put given back
	CHECK(undone[0].copy == -4);    // and out of the copy that shows it
	CHECK(side.Orphans() == 1);
	CHECK(side.Stats().unanswered == 1);

	const auto redo = side.Own({ 40, US, { CHEST, WATER, 4 } }, false);
	REQUIRE(redo);
	CHECK(redo->player == -4);
	CHECK(redo->refresh);
	CHECK(side.Orphans() == 0);

	// The take is refused late, after it was undone for want of an answer: undone once.
	undone = side.Expired(T0 + StorageView::ANSWER_WITHIN + std::chrono::seconds{ 10 });
	REQUIRE(undone.size() == 1);
	CHECK(!side.Refused(Refusal(CHEST, STIMPAK, -1)).matched);
	CHECK(side.Stats().lateRefusals == 1);
}

TEST("storage view: as the connection goes our unanswered moves are undone; the same session's catch-up does the numbered again")
{
	Side side = Friend();
	side.Reported({ CHEST, WATER, 2 }, T0);
	side.Reported({ CHEST, STIMPAK, -3 }, T0);
	const auto undone = side.OnDisconnected();
	REQUIRE(undone.size() == 2);
	CHECK(undone[0].player == 2);
	CHECK(undone[1].player == -3);
	CHECK(side.InFlight() == 0);

	side.OnWelcome(true, false, SESSION, T0);
	side.Owners(Owned(CHEST, FRIEND));
	const auto redo = side.Own({ 60, US, { CHEST, WATER, 2 } }, false);
	REQUIRE(redo);
	CHECK(redo->player == -2);

	// Another session: nothing of the last one's is ours to answer.
	side.OnDisconnected();
	side.OnWelcome(true, false, SESSION + 1, T0);
	CHECK(side.Orphans() == 0);
}

TEST("storage view: a move that couldn't be sent is undone at once")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Contents({ CHEST, 1, {} });
	side.Reported({ CHEST, WATER, 2 }, T0);
	const auto undo = side.Unsent({ CHEST, WATER, 2 });
	REQUIRE(undo);
	CHECK(undo->player == 2);
	CHECK(undo->copy == -2);
	CHECK(!side.Unsent({ CHEST, WATER, 2 }));
	CHECK(side.Stats().unsent == 1);
}

TEST("storage view: the host's listings: each once, again after a shortfall or a world request")
{
	Side side;
	side.OnWelcome(true, true, SESSION, T0);
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

TEST("storage view: past MAX_IN_FLIGHT a move isn't sent; a Take All and a Store All of the fullest chest are within it")
{
	static_assert(StorageView::MAX_IN_FLIGHT >= 2 * Protocol::MAX_STORAGE_STACKS);
	Side side = Friend();
	for (std::uint32_t i = 0; i < StorageView::MAX_IN_FLIGHT; ++i) {
		CHECK(side.Reported({ CHEST, STIMPAK + i, -1 }, T0));
	}
	CHECK(!side.Reported({ CHEST, WATER, -1 }, T0));
	CHECK(side.InFlight() == StorageView::MAX_IN_FLIGHT);
	CHECK(side.Stats().overflow == 1);
	CHECK(side.Refused(Refusal(CHEST, STIMPAK, -1)).matched);  // the oldest is still followed

	Side many;
	many.OnWelcome(true, false, SESSION, T0);
	for (std::uint32_t i = 0; i < Side::MAX_OWNERS + 5; ++i) {
		many.Owners(Owned(0x10000 + i, FRIEND));
	}
	CHECK(many.OwnerCount() == Side::MAX_OWNERS);
}

TEST("storage client: the first open sets the copy's own things aside, never to the player, and puts them back at the close")
{
	Engine engine;
	StorageView::Client client{ engine };
	client.OnWelcome(true, false, SESSION);
	client.State().Owners(Owned(CHEST, FRIEND));
	engine.copy = { { WATER, 2 }, { STIMPAK, 5 } };  // their stash, or the host's things it mirrored in an older session
	client.Open(CHEST, true);
	CHECK(engine.SharedIn(CHEST).empty());
	CHECK(engine.player.empty());
	CHECK((client.Aside() == std::map<std::uint32_t, std::int64_t>{ { STIMPAK, 5 }, { WATER, 2 } }));
	CHECK(engine.sent.size() == 1);

	client.OnContents({ CHEST, 7, { { STIMPAK, 3 } } });
	CHECK(engine.copy[STIMPAK] == 3);
	CHECK(engine.copy[WATER] == 0);

	client.MenuClosed();
	CHECK(engine.copy[STIMPAK] == 5);  // the true copy's left; their own came back
	CHECK(engine.copy[WATER] == 2);
	CHECK(engine.player.empty());
	CHECK(client.Aside().empty());
	CHECK(engine.sent.size() == 2);  // the view, and its close
}

TEST("storage client: the server closing the view hides the menu; what was set aside comes back only as it closes")
{
	Engine engine;
	StorageView::Client client{ engine };
	client.OnWelcome(true, false, SESSION);
	client.State().Owners(Owned(CHEST, FRIEND));
	engine.copy = { { WATER, 2 } };
	client.Open(CHEST, true);
	client.OnContents({ CHEST, 7, { { STIMPAK, 3 } } });
	const auto flushes = engine.flushes;
	client.OnRefused({ CHEST, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed });
	CHECK(engine.flushes == flushes + 1);  // the moves made in the menu went first
	CHECK(engine.hides == 1);
	CHECK(engine.copy[STIMPAK] == 0);  // the session's things left
	CHECK(engine.copy[WATER] == 0);    // their own still aside: the menu may still be open
	CHECK(engine.notices == std::vector{ StorageView::Notice::kClosed });

	client.MenuClosed();
	CHECK(engine.copy[WATER] == 2);
	CHECK(engine.sent.size() == 1);  // the server closed it: no close to send
}

TEST("storage client: a save or a disconnect while viewing puts the copy back as it was, and undoes what is unanswered")
{
	Engine engine;
	StorageView::Client client{ engine };
	client.OnWelcome(true, false, SESSION);
	client.State().Owners(Owned(CHEST, FRIEND));
	engine.copy = { { WATER, 2 } };
	engine.player = { { STIMPAK, 0 } };
	client.Open(CHEST, true);
	client.OnContents({ CHEST, 7, { { STIMPAK, 3 } } });
	// The player takes 3 stimpaks: the engine moves them, WorldSync reports the move.
	engine.copy[STIMPAK] -= 3;
	engine.player[STIMPAK] += 3;
	CHECK(client.Reporting({ CHEST, STIMPAK, -3 }));
	client.BeforeSaveOrLoad();
	CHECK(engine.copy[WATER] == 2);
	CHECK(engine.player[STIMPAK] == 3);  // on its way: the answer decides
	client.OnDisconnected();
	CHECK(engine.player[STIMPAK] == 0);  // no answer will come: taken back
	CHECK(client.State().Orphans() == 1);
}

TEST("storage client: notices come at most every NOTICE_EVERY, however many moves are refused")
{
	Engine engine;
	StorageView::Client client{ engine };
	client.OnWelcome(true, false, SESSION);
	client.State().Owners(Owned(CHEST, FRIEND));
	client.Open(CHEST, true);
	client.OnContents({ CHEST, 7, { { STIMPAK, 300 } } });
	for (std::uint32_t i = 0; i < 300; ++i) {
		engine.copy[STIMPAK] -= 1;
		engine.player[STIMPAK] += 1;
		client.Reporting({ CHEST, STIMPAK, -1 });
	}
	for (std::uint32_t i = 0; i < 300; ++i) {
		client.OnRefused(Refusal(CHEST, STIMPAK, -1));
	}
	CHECK(engine.player[STIMPAK] == 0);
	CHECK(engine.copy[STIMPAK] == 300);
	CHECK(engine.notices.size() == 1);
	CHECK(engine.sent.size() == 2);  // the view, and one ask for it afresh
	engine.now += StorageView::NOTICE_EVERY;
	client.Reporting({ CHEST, STIMPAK, -1 });
	client.OnRefused(Refusal(CHEST, STIMPAK, -1));
	CHECK(engine.notices.size() == 2);
}
