// Shared storage, a game's side: who keeps a container, what a friend's copy shows while its menu is open, what a
// refused move undoes, what our loaded world lacks, and how our moves settle after a load or a rejoin.

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

	// Welcomed, and settled at once (nothing of ours to put right): the settle sent is left out of what the tests count.
	void Welcome(StorageView::Client& a_client, Engine& a_engine)
	{
		a_client.OnWelcome(true, false, SESSION);
		a_client.OnCaughtUp(1, 1, 0, false);
		a_engine.sent.clear();
	}
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

TEST("storage view: a numbered move of ours our loaded world lacks is left to the true copy: our player keeps what the save has")
{
	Side side = Friend();
	side.OnLoad(SESSION, {}, {}, 0, T0);
	CHECK(!side.Own({ 20, US, { CHEST, WATER, 3 } }, true));     // nothing taken from our player: the server undoes the put
	CHECK(!side.Own({ 21, US, { CHEST, STIMPAK, -2 } }, true));  // nothing given: the server puts the take back
	CHECK(!side.Own({ 22, US, { CHEST, STIMPAK, -1 } }, false));  // in our world already
	CHECK(side.Stats().lacking == 2);
	CHECK((side.Lacking() == std::vector<std::uint32_t>{ 20, 21 }));
}

TEST("storage view: no move of ours numbered before the container was kept is lacking: the claim took the puts back")
{
	Side side = Friend();
	side.Owners({ { { CHEST, FRIEND, 30 } } });
	side.OnLoad(SESSION, {}, {}, 0, T0);
	side.Own({ 10, US, { CHEST, STIMPAK, -1 } }, true);
	side.Own({ 11, US, { CHEST, WATER, 2 } }, true);
	side.Own({ 31, US, { CHEST, STIMPAK, -1 } }, true);
	CHECK((side.Lacking() == std::vector<std::uint32_t>{ 31 }));
}

TEST("storage view: a catch-up the ledger still waits after settles from the same marker later; lacks past a rewound log go")
{
	Side side = Friend();
	side.OnLoad(SESSION, {}, { { 5, 20 } }, 0, T0);  // 20: numbered in a log the server lost (its host's game quit unsaved)
	CHECK(!side.CaughtUp(1, 1, 10, true));            // a gap: not yet
	REQUIRE(side.Marker());
	CHECK(*side.Marker() == 1);
	CHECK(side.Settling());
	// The gap was given up on (no more world states come): the game tries the marker again.
	const auto settle = side.CaughtUp(*side.Marker(), 1, 10, false);
	REQUIRE(settle);
	CHECK((settle->lacking == std::vector<std::uint32_t>{ 5 }));
	CHECK(side.Lacks() == 1);  // 20 forgotten: that number will be someone else's move
	CHECK(!side.Settling());
	side.OnDisconnected();
	CHECK(!side.Marker());
}

TEST("storage view: a load keeps the save's unanswered moves: their numbers find them, so they aren't lacking")
{
	Side side = Friend();
	side.Reported({ CHEST, WATER, 3 }, T0);
	side.Reported({ CHEST, STIMPAK, -1 }, T0);
	const auto unanswered = side.Unanswered();
	CHECK(unanswered.size() == 2);
	CHECK(side.MovesSession() == SESSION);

	side.Reported({ CHEST, WATER, 5 }, T0);  // after the save
	side.OnLoad(SESSION, unanswered, {}, 0, T0);
	CHECK(side.InFlight() == 2);
	CHECK(!side.Own({ 30, US, { CHEST, WATER, 3 } }, true));   // the save's: in our world
	CHECK(!side.Own({ 31, US, { CHEST, WATER, 5 } }, true));   // after it: lacking
	CHECK((side.Lacking() == std::vector<std::uint32_t>{ 31 }));
	// The save's take never came back numbered: it goes late with the settle, never undone here.
	side.Expired(T0 + StorageView::ANSWER_WITHIN);
	CHECK(side.InFlight() == 1);
	CHECK(side.Stats().unanswered == 1);
	const auto settle = side.CaughtUp(1, 1, 40, false);
	REQUIRE(settle);
	CHECK((settle->lacking == std::vector<std::uint32_t>{ 31 }));
	CHECK((settle->late == std::vector<Protocol::ContainerChange>{ { CHEST, STIMPAK, -1 } }));
	CHECK(settle->next == 40);
	CHECK(settle->lacksFrom == 0);
	CHECK(side.InFlight() == 1);  // on its way still: its number, or a refusal, answers it
}

TEST("storage view: a take kept with a save that comes back smaller takes back the rest; a move of now never matches an older number")
{
	Side side = Friend();
	side.Reported({ CHEST, STIMPAK, -4 }, T0);
	const auto unanswered = side.Unanswered();
	side.OnLoad(SESSION, unanswered, {}, 0, T0);
	side.Reported({ CHEST, STIMPAK, -10 }, T0);  // made in the loaded world
	const auto corrected = side.Own({ 50, US, { CHEST, STIMPAK, -1 } }, true);
	REQUIRE(corrected);
	CHECK(corrected->player == -3);
	CHECK(side.Stats().corrected == 1);
	// Another older take of ours, lacking: it doesn't pair with the take of 10 on its way.
	CHECK(!side.Own({ 51, US, { CHEST, STIMPAK, -3 } }, true));
	CHECK(side.InFlight() == 1);
	CHECK((side.Lacking() == std::vector<std::uint32_t>{ 51 }));
}

TEST("storage view: a move with no answer stays on its way, is matched when its number comes, and a late refusal undoes it")
{
	Side side = Friend();
	side.Open(CHEST);
	side.Contents({ CHEST, 1, {} });
	side.Reported({ CHEST, WATER, 4 }, T0);
	side.Reported({ CHEST, STIMPAK, -1 }, T0 + std::chrono::seconds{ 10 });
	side.Expired(T0 + StorageView::ANSWER_WITHIN);
	side.Expired(T0 + StorageView::ANSWER_WITHIN + std::chrono::seconds{ 10 });
	side.Expired(T0 + StorageView::ANSWER_WITHIN + std::chrono::seconds{ 20 });
	CHECK(side.Stats().unanswered == 2);  // once each
	CHECK(side.InFlight() == 2);

	CHECK(!side.Own({ 40, US, { CHEST, WATER, 4 } }, false));
	CHECK(side.InFlight() == 1);
	CHECK(side.Lacking().empty());
	const auto undo = side.Refused(Refusal(CHEST, STIMPAK, -1));
	CHECK(undo.matched);
	CHECK(undo.player == -1);
	CHECK(side.InFlight() == 0);
}

TEST("storage view: as the connection goes our unanswered moves stay; the rejoin settles them, and another session drops them")
{
	Side side = Friend();
	side.Reported({ CHEST, WATER, 2 }, T0);
	side.Reported({ CHEST, STIMPAK, -3 }, T0);
	side.OnDisconnected();
	CHECK(side.InFlight() == 2);
	CHECK(!side.Rule());

	side.OnWelcome(true, false, SESSION, T0);
	side.Owners(Owned(CHEST, FRIEND));
	CHECK(side.Settling());
	CHECK(!side.Own({ 60, US, { CHEST, WATER, 2 } }, true));  // the catch-up numbered the put: in our world
	CHECK(!side.CaughtUp(1, 2, 61, false));  // a world state for an older request: not settled yet
	CHECK(!side.CaughtUp(1, 1, 61, true));   // changes still wait for an earlier one: the gap is asked for, and settles
	const auto settle = side.CaughtUp(1, 1, 61, false);
	REQUIRE(settle);
	CHECK((settle->late == std::vector<Protocol::ContainerChange>{ { CHEST, STIMPAK, -3 } }));
	CHECK(settle->lacking.empty());
	CHECK(!side.Settling());
	CHECK(!side.CaughtUp(1, 1, 61, false));  // once
	CHECK(side.Stats().late == 1);

	// Another session: nothing of the last one's is ours to settle.
	side.OnDisconnected();
	side.OnWelcome(true, false, SESSION + 1, T0);
	CHECK(side.InFlight() == 0);
	CHECK(side.Lacks() == 0);
}

TEST("storage view: what our world lacks is kept with a save, and a settle names those from where it remembers")
{
	Side side = Friend();
	side.OnLoad(SESSION, {}, { { 3, 9, 12 } }, 5, T0);  // the save's record: 3 is before where it remembers from
	CHECK((side.Lacking() == std::vector<std::uint32_t>{ 9, 12 }));
	CHECK(side.LacksFrom() == 5);
	side.Own({ 20, US, { CHEST, WATER, 1 } }, true);
	const auto settle = side.CaughtUp(1, 1, 15, false);
	REQUIRE(settle);
	CHECK((settle->lacking == std::vector<std::uint32_t>{ 9, 12 }));  // 20 is past where the world state ended
	CHECK(settle->lacksFrom == 5);

	// Past MAX_LACKS the oldest is forgotten, and where it remembers from moves past it.
	Side full = Friend();
	full.OnLoad(SESSION, {}, {}, 0, T0);
	for (std::uint32_t i = 0; i <= Side::MAX_LACKS; ++i) {
		full.Own({ 100 + i, US, { CHEST, WATER, 1 } }, true);
	}
	CHECK(full.Lacks() == Side::MAX_LACKS);
	CHECK(full.LacksFrom() == 101);
	CHECK(full.Stats().lacksDropped == 1);
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
	Welcome(client, engine);
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
	Welcome(client, engine);
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

TEST("storage client: a save or a disconnect while viewing puts the copy back as it was, and leaves what is unanswered on its way")
{
	Engine engine;
	StorageView::Client client{ engine };
	Welcome(client, engine);
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
	CHECK(engine.player[STIMPAK] == 3);  // never taken back: the rejoin settles it
	CHECK(client.State().InFlight() == 1);
}

TEST("storage client: until our moves have settled, a friend's storage closes as it opens; the settle goes once caught up")
{
	Engine engine;
	StorageView::Client client{ engine };
	client.OnWelcome(true, false, SESSION);
	client.State().Owners(Owned(CHEST, FRIEND));
	engine.copy = { { WATER, 2 } };
	client.Open(CHEST, true);
	CHECK(engine.hides == 1);
	CHECK(!client.State().Viewing());
	CHECK(engine.copy[WATER] == 2);  // nothing set aside
	CHECK(engine.sent.empty());      // no view asked for
	CHECK(engine.notices == std::vector{ StorageView::Notice::kClosed });
	client.OnCaughtUp(1, 1, 5, false);
	REQUIRE(engine.sent.size() == 1);
	const auto settle = Protocol::DecodeStorageSettle(engine.sent[0]);
	REQUIRE(settle);
	CHECK(settle->asked == 1);
	CHECK(settle->next == 5);
	client.Open(CHEST, true);
	CHECK(client.State().Viewing() == CHEST);

	// A gap asked for again: settling again, and the menu closes as it opens until its answer has come.
	client.MenuClosed();
	client.Asked();
	client.Open(CHEST, true);
	CHECK(engine.hides == 2);
	client.OnCaughtUp(2, 2, 5, false);
	client.Open(CHEST, true);
	CHECK(client.State().Viewing() == CHEST);

	// With the rule off nothing settles.
	Engine off;
	StorageView::Client offClient{ off };
	offClient.OnWelcome(false, false, SESSION);
	offClient.OnCaughtUp(1, 1, 5, false);
	CHECK(off.sent.empty());
}

TEST("storage client: notices come at most every NOTICE_EVERY, however many moves are refused")
{
	Engine engine;
	StorageView::Client client{ engine };
	Welcome(client, engine);
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
