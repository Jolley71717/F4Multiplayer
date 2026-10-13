// A friend's loot as a client keeps it until the container it changed is loaded (WorldSync's
// PendingContainers), the slow sweep over what waits (RetryCursor), and the once-a-session notice of how
// full the session's history is. The first test drives it with what a real server sends; the rest pin
// down the promises it relies on.

#include "ServerFixture.h"

#include "game/PendingContainers.h"

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

	// Containers as the game keeps them: taking what isn't there takes nothing, so the order of a
	// container's changes matters (take one, then put one back, leaves one; the other way round, none).
	struct Inventories
	{
		std::map<std::pair<std::uint32_t, std::uint32_t>, int> items;

		void Change(const Protocol::ContainerChange& a_change)
		{
			auto& count = items[{ a_change.container, a_change.item }];
			count = (std::max)(0, count + a_change.count);
		}

		[[nodiscard]] std::map<std::pair<std::uint32_t, std::uint32_t>, int> Of(std::uint32_t a_container) const
		{
			std::map<std::pair<std::uint32_t, std::uint32_t>, int> out;
			for (const auto& [key, count] : items) {
				if (key.first == a_container && count != 0) {
					out[key] = count;
				}
			}
			return out;
		}
	};

	// A friend's game, as far as loot goes: what WorldSync does with each packet, against a world in which
	// only some containers are loaded.
	struct FriendGame
	{
		std::uint32_t            me = 0;
		std::set<std::uint32_t>  loaded;
		Inventories              world;
		PendingContainers        pending;
		std::size_t              worldStates = 0;

		bool TryApply(const Change& a_change)
		{
			if (!loaded.contains(a_change.container)) {
				return false;
			}
			world.Change(a_change);
			return true;
		}

		void Offer(const Protocol::IndexedContainerChange& a_change)
		{
			pending.Offer(a_change.index, a_change.change, a_change.playerId == me, [&](const Change& a_next) { return TryApply(a_next); });
		}

		// Everything the server sent, in the order it came (Session hands each packet to WorldSync).
		void Take(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kWorldState)) {
					const auto state = Protocol::DecodeWorldState(packet);
					REQUIRE(state);
					++worldStates;
					for (const auto& change : state->containerChanges) {
						Offer(change);
					}
				} else if (packet[0] == Type(MT::kContainerChanged)) {
					const auto change = Protocol::DecodeIndexedContainerChange(packet);
					REQUIRE(change);
					Offer(*change);
				}
			}
		}

		// The game says the container's 3D loaded (WorldSync's refLoadInbox).
		void Arrive(std::uint32_t a_container)
		{
			loaded.insert(a_container);
			pending.Retry(a_container, [&](const Change& a_next) { return TryApply(a_next); });
		}

		// The player takes or puts something themselves: in their world at once, and reported.
		void Loot(Fixture& a_f, PeerId a_peer, const Change& a_change)
		{
			world.Change(a_change);
			a_f.fake->Deliver(a_peer, Protocol::Encode(a_change, MT::kReportContainer));
		}
	};

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

	PendingContainers::Entry EntryOf(std::uint32_t a_index, std::uint32_t a_container, std::int32_t a_count)
	{
		return { a_index, { a_container, ITEM, a_count } };
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
	bob.me = welcome->playerId;
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
	CHECK(bob.pending.Waiting(CHEST_B));
	CHECK(bob.pending.Waiting(CHEST_C));

	// Two quick reloads use up his budget; the world states change nothing in his game.
	f.fake->Deliver(2, Request(welcome->sessionId, bob.pending.Next()));
	f.fake->Deliver(2, Request(welcome->sessionId, bob.pending.Next()));
	bob.Take(Received(f, 2));
	CHECK(bob.worldStates == 3);

	// He saves.
	const auto savedWorld = bob.world;
	const auto savedNext = bob.pending.Next();
	auto       savedEntries = bob.pending.Sorted();

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
	bob.pending.Load(savedNext, savedEntries);
	f.fake->Deliver(2, Request(welcome->sessionId, bob.pending.Next()));
	Sync(f, 2);
	CHECK(f.Logged("'Bob' has asked for the world too often"));

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
	CHECK(bob.pending.Size() == 1);
	CHECK(bob.pending.Waiting(CHEST_D));
	CHECK(bob.pending.Next() == echoes.size());

	// And when he does get there, D too.
	bob.Arrive(CHEST_D);
	CHECK(bob.world.Of(CHEST_D) == session.Of(CHEST_D));
	CHECK(bob.pending.Empty());
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
	bob.me = welcome->playerId;
	bob.loaded = { CHEST_A, CHEST_B };
	bob.Take(Received(f, 2));
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, 3));
	Sync(f, 1);
	bob.Take(Received(f, 2));
	const auto savedWorld = bob.world;
	const auto savedNext = bob.pending.Next();
	f.fake->Deliver(1, Loot(CHEST_A, ITEM, -2));
	f.fake->Deliver(1, Loot(CHEST_B, ITEM, 1));
	Sync(f, 1);
	bob.Take(Received(f, 2));

	// His game crashes; he starts it again from the save and rejoins.
	f.fake->Drop(2);
	bob.world = savedWorld;
	bob.pending.Load(savedNext, {});
	f.fake->Connect(3);
	auto hello = HelloFrom("Bob", 0xB0B);
	hello.world = { welcome->sessionId, bob.pending.Next() };
	f.fake->Deliver(3, Protocol::Encode(hello));
	Packets toBob;
	REQUIRE(AwaitWorldState(f, 3, toBob));
	const auto worlds = WorldStates(toBob);
	REQUIRE(worlds.size() == 1);
	CHECK(worlds[0].containerChanges.size() == 2);
	bob.Take(toBob);

	CHECK((bob.world.Of(CHEST_A) == std::map<std::pair<std::uint32_t, std::uint32_t>, int>{ { { CHEST_A, ITEM }, 1 } }));
	CHECK((bob.world.Of(CHEST_B) == std::map<std::pair<std::uint32_t, std::uint32_t>, int>{ { { CHEST_B, ITEM }, 1 } }));
	CHECK(bob.pending.Empty());
}

// ---- what the client relies on --------------------------------------------------------------

TEST("pending loot: a container that never loads doesn't hold up another's changes")
{
	PendingContainers pending;
	std::vector<Change> applied;
	const auto loadedIsB = [&](const Change& a_change) {
		if (a_change.container != CHEST_B) {
			return false;
		}
		applied.push_back(a_change);
		return true;
	};
	pending.Offer(0, { CHEST_A, ITEM, -1 }, false, loadedIsB);
	pending.Offer(1, { CHEST_B, ITEM, 2 }, false, loadedIsB);
	pending.Offer(2, { CHEST_A, ITEM, 1 }, false, loadedIsB);
	pending.Offer(3, { CHEST_B, ITEM, -1 }, false, loadedIsB);
	CHECK(applied.size() == 2);
	CHECK(pending.Size() == 2);
	CHECK(pending.Waiting(CHEST_A));
	CHECK(!pending.Waiting(CHEST_B));
	CHECK(pending.Next() == 4);
}

TEST("pending loot: a change behind another to the same container waits for it, even if it could be applied now")
{
	PendingContainers pending;
	std::vector<std::int32_t> applied;
	bool                      loaded = false;
	const auto tryApply = [&](const Change& a_change) {
		if (!loaded) {
			return false;
		}
		applied.push_back(a_change.count);
		return true;
	};
	pending.Offer(0, { CHEST_A, ITEM, -1 }, false, tryApply);
	loaded = true;
	pending.Offer(1, { CHEST_A, ITEM, 2 }, false, tryApply);
	CHECK(applied.empty());
	CHECK(pending.Retry(CHEST_A, tryApply) == 2);
	CHECK((applied == std::vector<std::int32_t>{ -1, 2 }));
	CHECK(pending.Empty());
	CHECK(!pending.Waiting(CHEST_A));
}

TEST("pending loot: a change that comes twice, or one already in our world, isn't applied (again)")
{
	PendingContainers pending;
	int        applied = 0;
	const auto tryApply = [&](const Change&) { return ++applied, true; };
	pending.Offer(0, { CHEST_A, ITEM, 1 }, false, tryApply);
	pending.Offer(0, { CHEST_A, ITEM, 1 }, false, tryApply);  // live, then in a world state
	pending.Offer(1, { CHEST_A, ITEM, 1 }, true, tryApply);   // ours, before the save we loaded
	pending.Offer(2, { 0, ITEM, 1 }, false, tryApply);        // nothing a game could apply
	CHECK(applied == 1);
	CHECK(pending.Next() == 3);
	CHECK(pending.Empty());
}

// A save made with loot waiting must give the same loot back, in the same order per container, whatever
// order the record holds it in.
TEST("pending loot: a save keeps what waits, and loading it waits for the same containers in the same order")
{
	PendingContainers pending;
	const auto never = [](const Change&) { return false; };
	for (std::uint32_t i = 0; i < 30; ++i) {
		pending.Offer(i, { CHEST_A + i % 4, ITEM, static_cast<std::int32_t>(i) + 1 }, false, never);
	}
	auto saved = pending.Sorted();
	REQUIRE(saved.size() == 30);
	CHECK(std::ranges::is_sorted(saved, {}, &PendingContainers::Entry::index));
	std::ranges::shuffle(saved, std::mt19937{ 11 });

	PendingContainers loaded;
	loaded.Load(pending.Next(), saved);
	CHECK(loaded.Next() == 30);
	CHECK(loaded.Size() == 30);
	CHECK(loaded.Sorted() == pending.Sorted());
	for (std::uint32_t chest = CHEST_A; chest < CHEST_A + 4; ++chest) {
		std::vector<std::int32_t> order;
		loaded.Retry(chest, [&](const Change& a_change) { return order.push_back(a_change.count), true; });
		CHECK(std::ranges::is_sorted(order));
		CHECK(order.size() == 30 / 4 + (chest - CHEST_A < 30 % 4 ? 1 : 0));
	}
	CHECK(loaded.Empty());
	loaded.Load(0, {});
	CHECK(loaded.Next() == 0);
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
