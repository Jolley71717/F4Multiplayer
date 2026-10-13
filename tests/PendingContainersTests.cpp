// A friend's loot as a client keeps it until the container it changed is loaded (WorldSync's
// loot engine, LootSync::Loot), the slow sweep over what waits (RetryCursor), and the once-a-session notice of how
// full the session's history is. The first test drives it with what a real server sends; the rest pin
// down the promises it relies on.

#include "ChestWorld.h"
#include "FriendGame.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Change = Protocol::ContainerChange;

	constexpr std::uint32_t CHEST_A = 0x0002C001;  // where Bob is: loaded all along
	constexpr std::uint32_t CHEST_B = 0x0002C002;
	constexpr std::uint32_t CHEST_C = 0x0002C003;
	constexpr std::uint32_t CHEST_D = 0x0002C004;  // somewhere Bob never goes
	constexpr std::uint32_t ITEM = 0x0004A001;
	constexpr std::uint32_t ITEM_2 = 0x0004A002;
	// For the ledger on its own: the session, our player ID in it, and another player's.
	constexpr std::uint64_t SESSION = 0x5E55'0001'0000'0001;
	constexpr std::uint32_t BOB = 2;
	constexpr std::uint32_t ALICE = 1;

	Protocol::Hello HelloFrom(std::string a_name, std::uint64_t a_identity)
	{
		return Fixture::MakeHello(std::move(a_name), a_identity);
	}

	auto Loot(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count)
	{
		return Protocol::Encode(Change{ a_container, a_item, a_count }, MT::kReportContainer);
	}

	auto Request(std::uint64_t a_session, std::uint32_t a_from)
	{
		return Protocol::Encode(Protocol::WorldRequest{ a_session, a_from });
	}

	bool AwaitWorldState(Fixture& a_f, PeerId a_peer, Packets& a_got)
	{
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while (std::chrono::steady_clock::now() < deadline) {
			std::ranges::move(a_f.fake->Take(a_peer), std::back_inserter(a_got));
			if (Count(a_got, MT::kWorldState) > 0) {
				return true;
			}
			std::this_thread::sleep_for(20ms);
		}
		return false;
	}
}

// ---- with a real server ---------------------------------------------------------------------

// Bob is in one place, the host's friend Alice loots all over the map. Bob reloads a save while his
// world-state budget is spent (so the server owes it and holds back live changes meanwhile), and walks
// to the containers afterwards in his own order. Every container he reaches ends up as it is in Alice's
// game, with no item twice and none missing; the one he never reaches keeps its changes waiting.
TEST("pending loot: a friend's game ends up with every container as the session has it, through a reload and an owed world state")
{
	Server::Options options;
	options.worldStateRefill = 1500ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	const auto welcome = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(welcome);
	FriendGame bob;
	bob.Welcome(*welcome);
	bob.loaded = { CHEST_A };
	bob.Take(Received(f, 2));

	// Order matters in B: taken (from nothing) then put back leaves one.
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, 2));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, -1));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, 1));
	f.fake->Deliver(1, Loot(CHEST_C, ITEM, 4));
	f.fake->Deliver(1, Loot(CHEST_D, ITEM, 1));
	bob.Take(Received(f, 2));
	bob.Loot(f, 2, { CHEST_A, ITEM, -1 });  // one of the two Alice put there
	bob.Take(Received(f, 2));
	CHECK(bob.Waits(CHEST_B));
	CHECK(bob.Waits(CHEST_C));

	// Two quick reloads use up his budget; the world states change nothing in his game.
	f.fake->Deliver(2, Request(welcome->sessionId, bob.Next()));
	f.fake->Deliver(2, Request(welcome->sessionId, bob.Next()));
	bob.Take(Received(f, 2));
	CHECK(bob.worldStates == 3);

	// He saves.
	const auto savedWorld = bob.world;
	const auto savedNext = bob.Next();
	auto       savedEntries = bob.loot.Waiting();

	// After the save: more loot, some of it in containers Bob has, live.
	f.fake->Deliver(1, Loot(CHEST_B, ITEM_2, 1));
	f.fake->Deliver(1, Loot(CHEST_C, ITEM, -3));
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, -1));
	Sync(f, 1);
	bob.Take(Received(f, 2));

	// He loads the save (the co-save record comes back in whatever order) and asks for what it lacks:
	// one world state too many, so it is owed.
	std::ranges::shuffle(savedEntries, std::mt19937{ 7 });
	bob.world = savedWorld;
	bob.LoadRecord(savedNext, savedEntries);
	f.fake->Deliver(2, Request(welcome->sessionId, bob.Next()));
	Sync(f, 2);
	CHECK(f.Logged(std::format("player {} has asked for the world too often", welcome->playerId)));

	// Alice carries on meanwhile; none of it reaches Bob live while his world state is owed.
	f.fake->Deliver(1, Loot(CHEST_C, ITEM, -1));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM_2, -1));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, 3));
	Sync(f, 1);
	Packets toBob = Received(f, 2);
	CHECK(Count(toBob, MT::kContainerChanged) == 0);
	REQUIRE(AwaitWorldState(f, 2, toBob));
	bob.Take(toBob);

	// After it, live again.
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, 5));
	f.fake->Deliver(1, Loot(CHEST_C, ITEM_2, 2));
	Sync(f, 1);
	bob.Take(Received(f, 2));

	// He walks to C, then B.
	bob.Arrive(CHEST_C);
	bob.Arrive(CHEST_B);

	// The session as Alice's game was told it: every change once, in the server's order.
	auto echoes = Decoded(Received(f, 1), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	std::ranges::sort(echoes, {}, &Protocol::IndexedContainerChange::index);
	Inventories session;
	for (std::uint32_t i = 0; i < echoes.size(); ++i) {
		REQUIRE(echoes[i].index == i);
		session.Change(echoes[i].change);
	}
	for (const auto chest : { CHEST_A, CHEST_B, CHEST_C }) {
		CHECK(bob.world.Of(chest) == session.Of(chest));
	}
	CHECK((bob.world.Of(CHEST_B).at({ CHEST_B, ITEM }) == 4));  // 1 (taken from nothing, put back), then 3 more
	CHECK(bob.world.Of(CHEST_D).empty());
	CHECK(bob.loot.Waiting().size() == 1);
	CHECK(bob.Waits(CHEST_D));
	CHECK(bob.Next() == echoes.size());

	// And when he does get there, D too.
	bob.Arrive(CHEST_D);
	CHECK(bob.world.Of(CHEST_D) == session.Of(CHEST_D));
	CHECK(bob.loot.Waiting().size() == 0);
}

// A rejoin with a save from before the session's latest loot: the hello's world state carries what the
// save lacks and nothing it has, so the friend's containers come out right without counting anything twice.
TEST("pending loot: rejoining from an older save, only what the save lacks is applied, once")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	auto welcome = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(welcome);
	FriendGame bob;
	bob.Welcome(*welcome);
	bob.loaded = { CHEST_A, CHEST_B };
	bob.Take(Received(f, 2));
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, 3));
	Sync(f, 1);
	bob.Take(Received(f, 2));
	const auto savedWorld = bob.world;
	const auto savedNext = bob.Next();
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, -2));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, 1));
	Sync(f, 1);
	bob.Take(Received(f, 2));

	// His game crashes; he starts it again from the save and rejoins.
	f.fake->Drop(2);
	bob.world = savedWorld;
	bob.LoadRecord(savedNext, {});
	f.fake->Connect(3);
	auto hello = HelloFrom("Bob", 0xB0B);
	hello.world = { welcome->sessionId, bob.Next() };
	f.fake->Deliver(3, Protocol::Encode(hello));
	Packets toBob;
	REQUIRE(AwaitWorldState(f, 3, toBob));
	const auto worlds = WorldStates(toBob);
	REQUIRE(worlds.size() == 1);
	CHECK(worlds[0].containerChanges.size() == 2);
	bob.Take(toBob);

	CHECK((bob.world.Of(CHEST_A) == std::map<std::pair<std::uint32_t, std::uint32_t>, int>{ { { CHEST_A, ITEM }, 1 } }));
	CHECK((bob.world.Of(CHEST_B) == std::map<std::pair<std::uint32_t, std::uint32_t>, int>{ { { CHEST_B, ITEM }, 1 } }));
	CHECK(bob.loot.Waiting().size() == 0);
}

// Bob fast travels while a chest he is heading to has loot waiting. The chest loads during the loading
// screen, where nothing can be applied and its load is not tried, and Alice loots it again meanwhile. On the
// first frame back (the same cell: a reload looks just like this) all that waits is tried, and the chest is
// as the session has it, without walking away and back.
TEST("pending loot: a chest that loads during a loading screen gets its loot on the first frame back")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	const auto welcome = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(welcome);
	FriendGame bob;
	bob.Welcome(*welcome);
	bob.loaded = { CHEST_A };
	bob.Take(Received(f, 2));
	bob.Frame();
	Packets toAlice = Received(f, 1);

	f.fake->Deliver(1, Loot(CHEST_B, ITEM, 2));
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, 1));
	Sync(f, 1);
	bob.Take(Received(f, 2));
	bob.Frame();
	CHECK(bob.Waits(CHEST_B));
	CHECK(!bob.Waits(CHEST_A));

	bob.inWorld = false;
	bob.Frame();
	bob.Load(CHEST_B);
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, -1));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM_2, 1));
	Sync(f, 1);
	bob.Take(Received(f, 2));
	for (int frame = 0; frame < 10; ++frame) {
		const auto plan = bob.Frame(200ms);  // two seconds of loading screen: no sweep either
		CHECK(plan.loaded.empty());
		CHECK(plan.others.empty());
	}
	CHECK(bob.world.Of(CHEST_B).empty());

	bob.inWorld = true;
	const auto back = bob.Frame();
	CHECK(back.full);
	std::ranges::move(Received(f, 1), std::back_inserter(toAlice));
	const auto session = Inventories::FromEchoes(Decoded(toAlice, MT::kContainerChanged, Protocol::DecodeIndexedContainerChange));
	CHECK(bob.world.Of(CHEST_B) == session.Of(CHEST_B));
	CHECK(bob.world.Of(CHEST_A) == session.Of(CHEST_A));
	CHECK(bob.loot.Waiting().size() == 0);
}

// A chest that loaded before its change came (the game said so then, and won't again) isn't tried every
// frame, but the slow sweep gets to it.
TEST("retry schedule: a chest loaded without a word from the game is applied by the slow sweep, not tried every frame")
{
	FriendGame bob;
	bob.Frame();
	bob.Offer({ .index = 0, .playerId = 9, .change = { CHEST_A, ITEM, 1 } });
	CHECK(bob.Waits(CHEST_A));
	CHECK(bob.Frame().others.size() == 1);  // a sweep, while it isn't loaded
	bob.loaded.insert(CHEST_A);

	const auto from = bob.now;
	std::size_t tried = 0;
	for (int frame = 0; frame < 1000 && bob.Waits(CHEST_A); ++frame) {
		const auto plan = bob.Frame(50ms);
		tried += plan.others.size() + plan.loaded.size();
	}
	CHECK(!bob.Waits(CHEST_A));
	CHECK(tried == 1);
	CHECK(bob.now - from >= 1900ms);  // the sweep's interval in FriendGame: two seconds
	CHECK(bob.now - from <= 2100ms);
	CHECK((bob.world.Of(CHEST_A).at({ CHEST_A, ITEM }) == 1));
}

TEST("retry schedule: everything waiting is tried on arriving in the world or in another cell; loads are tried as they come; nothing while loading")
{
	using namespace std::chrono;
	RetrySchedule                    schedule{ 2s, 256 };
	const std::vector<std::uint32_t> waiting{ CHEST_A, CHEST_B, CHEST_C };
	int                              listed = 0;
	const auto                       all = [&] { return ++listed, waiting; };
	auto                             now = steady_clock::now();
	constexpr std::uint32_t          SANCTUARY = 0x0000003C, DIAMOND_CITY = 0x0001F000;

	auto plan = schedule.Frame(now, true, SANCTUARY, {}, true, all);
	CHECK(plan.full);  // the first frame in the world
	CHECK(plan.others == waiting);

	plan = schedule.Frame(now += 16ms, true, SANCTUARY, { CHEST_B }, true, all);
	CHECK(!plan.full);
	CHECK((plan.loaded == std::vector<std::uint32_t>{ CHEST_B }));
	CHECK(plan.others.empty());  // the sweep isn't due

	plan = schedule.Frame(now += 16ms, true, DIAMOND_CITY, {}, true, all);
	CHECK(plan.full);

	plan = schedule.Frame(now += 16ms, false, 0, { CHEST_C }, true, all);
	CHECK(plan.loaded.empty());
	CHECK(plan.others.empty());
	plan = schedule.Frame(now += 16ms, true, DIAMOND_CITY, {}, true, all);
	CHECK(plan.full);  // out of the loading screen, though in the same cell

	plan = schedule.Frame(now += 2100ms, true, DIAMOND_CITY, {}, true, all);
	CHECK(!plan.full);
	CHECK(plan.others == waiting);  // the sweep, a few at a time (all three, here)

	// Nothing waits: nothing is tried, and the list isn't even made.
	listed = 0;
	plan = schedule.Frame(now += 3s, true, DIAMOND_CITY, { CHEST_A }, false, all);
	CHECK(plan.loaded.empty());
	CHECK(plan.others.empty());
	CHECK(listed == 0);
}

// ---- what the client relies on --------------------------------------------------------------

TEST("pending loot: a container that never loads doesn't hold up another's changes")
{
	LootSync::Loot      loot;
	std::vector<Change> applied;
	ChestWorld          world{ [](std::uint32_t a_container) { return a_container == CHEST_B; }, [&](const Change& a_change) { applied.push_back(a_change); } };
	const auto          now = LootSync::Clock::now();
	loot.OnWelcome(SESSION, BOB);
	loot.Live(Numbered(0, ALICE, { CHEST_A, ITEM, -1 }), now, world);
	loot.Live(Numbered(1, ALICE, { CHEST_B, ITEM, 2 }), now, world);
	loot.Live(Numbered(2, ALICE, { CHEST_A, ITEM, 1 }), now, world);
	loot.Live(Numbered(3, ALICE, { CHEST_B, ITEM, -1 }), now, world);
	CHECK(applied.size() == 2);
	CHECK(loot.Waiting().size() == 2);
	CHECK((loot.Containers() == std::vector<std::uint32_t>{ CHEST_A }));
	CHECK(loot.Point().containerFrom == 4);
}

TEST("pending loot: a change behind another to the same container waits for it, even if it could be applied now")
{
	LootSync::Loot            loot;
	std::vector<std::int32_t> applied;
	bool                      loaded = false;
	ChestWorld                world{ [&](std::uint32_t) { return loaded; }, [&](const Change& a_change) { applied.push_back(a_change.count); } };
	const auto                now = LootSync::Clock::now();
	loot.OnWelcome(SESSION, BOB);
	loot.Live(Numbered(0, ALICE, { CHEST_A, ITEM, -1 }), now, world);
	CHECK(loot.Waiting().size() == 1);
	loaded = true;
	loot.Live(Numbered(1, ALICE, { CHEST_A, ITEM, 2 }), now, world);
	CHECK(loot.Waiting().size() == 2);
	CHECK(applied.empty());
	CHECK(loot.Retry(CHEST_A, world) == 2);
	CHECK((applied == std::vector<std::int32_t>{ -1, 2 }));
	CHECK(loot.Waiting().empty());
	CHECK(loot.Containers().empty());
}

TEST("pending loot: a change that comes twice, or one already in our world, isn't applied (again)")
{
	LootSync::Loot loot;
	int            applied = 0;
	ChestWorld     world{ [](std::uint32_t) { return true; }, [&](const Change&) { ++applied; } };
	const auto     now = LootSync::Clock::now();
	loot.OnWelcome(SESSION, BOB);
	loot.Live(Numbered(0, ALICE, { CHEST_A, ITEM, 1 }), now, world);
	loot.CatchUp(0, { Numbered(0, ALICE, { CHEST_A, ITEM, 1 }) }, now, world);  // live, then in a world state
	loot.Live(Numbered(1, BOB, { CHEST_A, ITEM, 1 }), now, world);              // our own: in our world already
	loot.Live(Numbered(2, ALICE, { 0, ITEM, 1 }), now, world);                  // nothing a game could apply
	CHECK(applied == 1);
	CHECK(loot.Point().containerFrom == 3);
	CHECK(loot.Waiting().empty());
}

// A change from a gap (one the world state we asked for after a load carries, behind a live one that came
// first) goes in its container's queue in session order, not at the back.
TEST("pending loot: a change that fills a gap waits in session order behind its container's earlier ones, ahead of later ones")
{
	LootSync::Loot            loot;
	bool                      loaded = false;
	std::vector<std::int32_t> order;
	ChestWorld                world{ [&](std::uint32_t) { return loaded; }, [&](const Change& a_change) { order.push_back(a_change.count); } };
	const auto                now = LootSync::Clock::now();
	loot.OnWelcome(SESSION, BOB);
	loot.Live(Numbered(0, ALICE, { CHEST_A, ITEM, 1 }), now, world);
	loot.Live(Numbered(3, ALICE, { CHEST_A, ITEM, 4 }), now, world);  // live, ahead of the world state's #1 and #2
	loot.CatchUp(1, { Numbered(1, ALICE, { CHEST_A, ITEM, 2 }), Numbered(2, ALICE, { CHEST_A, ITEM, 3 }) }, now, world);
	loaded = true;
	CHECK(loot.Retry(CHEST_A, world) == 4);
	CHECK((order == std::vector<std::int32_t>{ 1, 2, 3, 4 }));
}

// A save made with loot waiting must give the same loot back, in the same order per container, whatever
// order the record holds it in.
TEST("pending loot: a save keeps what waits, and loading it waits for the same containers in the same order")
{
	LootSync::Loot pending;
	ChestWorld     never{ [](std::uint32_t) { return false; }, [](const Change&) {} };
	const auto     now = LootSync::Clock::now();
	pending.OnWelcome(SESSION, BOB);
	for (std::uint32_t i = 0; i < 30; ++i) {
		pending.Live(Numbered(i, ALICE, { CHEST_A + i % 4, ITEM, static_cast<std::int32_t>(i) + 1 }), now, never);
	}
	auto saved = pending.Waiting();
	REQUIRE(saved.size() == 30);
	CHECK(std::ranges::is_sorted(saved, {}, &LootSync::Pending::index));
	std::ranges::shuffle(saved, std::mt19937{ 11 });

	LootSync::Loot loaded;
	loaded.LoadRecord(SESSION, pending.Point().containerFrom, saved);
	CHECK(loaded.Point().sessionId == SESSION);
	CHECK(loaded.Point().containerFrom == 30);
	REQUIRE(loaded.Waiting().size() == 30);
	for (std::size_t i = 0; i < 30; ++i) {
		CHECK(loaded.Waiting()[i].index == pending.Waiting()[i].index);
		CHECK(loaded.Waiting()[i].change.container == pending.Waiting()[i].change.container);
		CHECK(loaded.Waiting()[i].change.count == pending.Waiting()[i].change.count);
	}
	CHECK(loaded.Containers() == pending.Containers());
	for (std::uint32_t chest = CHEST_A; chest < CHEST_A + 4; ++chest) {
		std::vector<std::int32_t> order;
		ChestWorld                here{ [&](std::uint32_t a_container) { return a_container == chest; }, [&](const Change& a_change) { order.push_back(a_change.count); } };
		loaded.Retry(chest, here);
		CHECK(std::ranges::is_sorted(order));
		CHECK(order.size() == 30 / 4 + (chest - CHEST_A < 30 % 4 ? 1 : 0));
	}
	CHECK(loaded.Waiting().empty());
	CHECK(loaded.Containers().empty());
	loaded.LoadRecord(0, 0, {});  // a save with no record
	CHECK(loaded.Point().containerFrom == 0);
}

// The safety net for what loaded before its change came: a few at a time, but nothing is left out for long.
TEST("sweep: everything that waits is tried within a few passes, while other entries come and go")
{
	RetryCursor<std::uint32_t> cursor{ 256 };
	std::set<std::uint32_t>    waiting;
	for (std::uint32_t id = 1; id <= 1000; ++id) {
		waiting.insert(id * 7);
	}
	const std::set<std::uint32_t> stays{ 7, 3500, 6993, 7000 };
	std::set<std::uint32_t>       seen;
	std::uint32_t                 next = 100000;
	const auto                    passes = (waiting.size() + 255) / 256 + 1;  // +1: entries added meanwhile
	for (std::size_t pass = 0; pass < passes; ++pass) {
		const auto visit = cursor.Pass({ waiting.begin(), waiting.end() });
		CHECK(visit.size() <= 256);
		for (const auto id : visit) {
			seen.insert(id);
			if (!stays.contains(id) && id % 2 == 0) {
				waiting.erase(id);  // applied
			}
		}
		// New arrivals, before and after the cursor.
		waiting.insert(next++);
		waiting.insert(static_cast<std::uint32_t>(pass + 1));
	}
	for (const auto id : stays) {
		CHECK(seen.contains(id));
	}
	CHECK(std::ranges::all_of(waiting, [&](std::uint32_t a_id) { return a_id >= 100000 || a_id <= passes || seen.contains(a_id); }));
}

TEST("sweep: a few entries are all tried every pass, and nothing waiting means nothing to try")
{
	RetryCursor<std::uint32_t> cursor{ 256 };
	CHECK(cursor.Pass({}).empty());
	for (int pass = 0; pass < 3; ++pass) {
		const auto visit = cursor.Pass({ 30, 10, 20, 10 });
		CHECK(visit.size() == 3);
		CHECK((std::set<std::uint32_t>{ visit.begin(), visit.end() } == std::set<std::uint32_t>{ 10, 20, 30 }));
	}
}

TEST("history notice: how full the session looks goes by its fullest table")
{
	CHECK(SharedHistoryPercent(0, 0, 0) == 0);
	CHECK(SharedHistoryPercent(1000, 2000, 300) < 80);
	CHECK(SharedHistoryPercent(48000, 10, 10) == 80);
	CHECK(SharedHistoryPercent(10, 10, 54000) == 90);
	CHECK(SharedHistoryPercent(10, Protocol::MAX_WORLD_STATE_ACTORS, 10) == 100);
}
