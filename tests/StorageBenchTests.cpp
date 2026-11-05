// The host's workbench as shared storage (Storage.h), against a real server (LootGame.h plays each game's loot and storage
// code): a settlement's workbench the host's game lists has one true copy, the host's, and a friend's workshop mode and
// crafting menus show it while open. A build or a craft takes its components out of the workbench, a scrap puts them in, as
// the engine does, and each goes through the server like any move at shared storage. Every test counts the components in
// the session's worlds before and after: nothing is made twice and nothing is lost, whatever is refused, reloaded or cut off.

#include "LootGame.h"

#include <deque>
#include <map>
#include <thread>

namespace
{
	using LootTest::Game;
	using StorageView::Keeper;
	using namespace TestServer;

	constexpr std::uint64_t HOST_ID = 0x0000'A11C'E000'0001;
	constexpr std::uint64_t FRIEND_ID = 0x0000'B0B0'0000'0002;
	constexpr PeerId        HOST_PEER = 1;
	constexpr PeerId        FRIEND_PEER = 2;
	constexpr std::uint32_t BENCH = 0x0002'50FE;   // Sanctuary's workbench, say
	constexpr std::uint32_t OTHER = 0x0002'50FF;   // a settlement's workbench the host's game never listed
	constexpr std::uint32_t LINKED = 0x0002'5101;  // another settlement's workbench the host owns, which a supply line may link
	constexpr std::uint32_t STEEL = 0x0001'FA8C;   // a component's scrap item (junk)
	constexpr std::uint32_t WOOD = 0x0001'FA91;
	constexpr std::uint32_t WATER = 0x0004'835D;   // purified water: a day's production
	constexpr std::uint32_t RIFLE = 0x0004'F46A;

	Server::Options Options()
	{
		Server::Options options;
		options.hostIdentity = HOST_ID;
		options.rules.sharedStorage = true;
		options.worldStateRefill = std::chrono::milliseconds{ 0 };  // each load settles before the next
		return options;
	}

	void Benches(Game& a_game)
	{
		for (const auto bench : { BENCH, OTHER, LINKED }) {
			a_game.holderFacts[bench] = { .kind = LootScope::Holder::kContainer, .workshop = true, .storage = true };
		}
		a_game.items[RIFLE] = { .kind = LootScope::Item::kWeapon };
		a_game.items[WATER] = { .kind = LootScope::Item::kAid };
	}

	// The server answers at most 10 heartbeats a second from a player (Sync is one): a game that would ask more waits.
	void Pace(PeerId a_peer)
	{
		using Steady = std::chrono::steady_clock;
		constexpr auto WINDOW = std::chrono::milliseconds{ 1100 };
		static std::map<PeerId, std::deque<Steady::time_point>> syncs;
		auto& times = syncs[a_peer];
		while (!times.empty() && Steady::now() - times.front() >= WINDOW) {
			times.pop_front();
		}
		if (times.size() >= 8) {
			std::this_thread::sleep_until(times.front() + WINDOW);
			times.pop_front();
		}
		times.push_back(Steady::now());
	}

	void Receive(Fixture& a_f, Game& a_game)
	{
		Pace(a_game.peer);
		a_game.Receive(a_f);
	}

	// A session at a settlement: the host's workbench holds 10 steel; the friend's copy of it holds 4 steel and a rifle of
	// their own (their save's settlement), and they carry 2 steel. The host's workbench is listed (the other one isn't).
	struct Session
	{
		Fixture f{ Options() };
		Game    host{ HOST_PEER, HOST_ID };
		Game    guest{ FRIEND_PEER, FRIEND_ID };
		// What each game's builds used up (from a workbench or their own inventory), and what a refused build used that
		// the friend no longer had (StorageView's usedUp): the one thing the true copy can't get back.
		std::int64_t built = 0;

		Session()
		{
			f.fake->SetLocal(HOST_PEER);
			host.host = true;
			Benches(host);
			Benches(guest);
			host.away.insert(OTHER);  // not loaded in the host's world: never listed
			host.holders[BENCH][STEEL] = 10;
			guest.holders[BENCH][STEEL] = 4;
			guest.holders[BENCH][RIFLE] = 1;
			guest.carried[STEEL] = 2;
			host.Join(f, "Host");
			guest.Join(f, "Guest");
			Settle();
		}

		void Settle()
		{
			for (int round = 0; round < 3; ++round) {
				Receive(f, host);
				Receive(f, guest);
			}
		}

		// The friend's own components at the workbench: in their copy, or set aside while it shows the host's.
		std::int64_t Own(std::uint32_t a_item) const
		{
			const auto& aside = guest.storage.Aside();
			if (guest.storage.State().Viewing()) {
				return aside.contains(a_item) ? aside.at(a_item) : 0;
			}
			return guest.In(BENCH, a_item);
		}

		// Every one of a_item in the session's worlds, and those builds used up.
		std::int64_t Total(std::uint32_t a_item) const
		{
			return host.In(BENCH, a_item) + host.Carried(a_item) + guest.Carried(a_item) + Own(a_item) + (a_item == STEEL ? built : 0);
		}

		// A build that uses a_count of a_item, in a_game's world (workshop mode's or a crafting menu's).
		void Build(Game& a_game, std::int64_t a_count, bool a_frame = true)
		{
			built += a_count;
			a_game.Build(f, BENCH, STEEL, a_count, a_frame);
		}
	};

	constexpr std::int64_t STEEL_START = 10 + 4 + 2;
}

TEST("shared bench: the host's game lists its workbench; a friend's workshop mode shows the host's junk, and their own comes back after")
{
	Session s;
	CHECK(s.host.Keeper(BENCH) == Keeper::kUs);
	CHECK(s.guest.Keeper(BENCH) == Keeper::kFriend);
	CHECK(s.guest.Keeper(OTHER) == Keeper::kNone);
	CHECK(s.guest.Holder(BENCH).sharedBench);
	CHECK(!s.guest.Holder(OTHER).sharedBench);

	s.guest.OpenBench(s.f, BENCH);
	CHECK(s.guest.In(BENCH, STEEL) == 0);  // their own 4 set aside
	CHECK(s.guest.In(BENCH, RIFLE) == 1);  // weapons stay each player's: never set aside, never shared
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 10);  // the host's, to build with
	CHECK(s.Total(STEEL) == STEEL_START);

	s.guest.CloseBench(s.f);
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	CHECK(s.guest.In(BENCH, RIFLE) == 1);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 10);
	CHECK(s.host.In(BENCH, RIFLE) == 0);
	CHECK(s.Total(STEEL) == STEEL_START);
}

TEST("shared bench: a friend's build takes its components out of the host's workbench, once")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.guest, 3);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 7);
	CHECK(s.guest.In(BENCH, STEEL) == 7);
	CHECK(s.guest.Carried(STEEL) == 2);  // the workbench paid, not their pockets
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
	CHECK(s.Total(STEEL) == STEEL_START);

	// Built again later: the true copy is what was left.
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 7);
	s.Build(s.guest, 7);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	CHECK(s.Total(STEEL) == STEEL_START);
}

TEST("shared bench: a friend's scrap puts its components into the host's workbench")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.guest.Scrap(s.f, BENCH, WOOD, 5);
	s.guest.Scrap(s.f, BENCH, STEEL, 1);
	s.Settle();
	CHECK(s.host.In(BENCH, WOOD) == 5);
	CHECK(s.host.In(BENCH, STEEL) == 11);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, WOOD) == 0);  // in the one pile, not also in their own copy
	CHECK(s.Total(WOOD) == 5);
	CHECK(s.Total(STEEL) == STEEL_START + 1);

	// And the host builds with it.
	s.Build(s.host, 11);
	s.host.Build(s.f, BENCH, WOOD, 5);
	s.Settle();
	CHECK(s.host.In(BENCH, WOOD) == 0);
	CHECK(s.Total(STEEL) == STEEL_START + 1);
}

TEST("shared bench: the host's builds and scraps show in a friend's workshop mode as they happen; the friend's in the host's")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.host, 2);  // the keeper's own move: always numbered
	s.host.Scrap(s.f, BENCH, WOOD, 3);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 8);
	CHECK(s.guest.In(BENCH, WOOD) == 3);
	s.Build(s.guest, 1);
	s.guest.Scrap(s.f, BENCH, WOOD, 2);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 7);
	CHECK(s.host.In(BENCH, WOOD) == 5);
	CHECK(s.guest.In(BENCH, STEEL) == 7);
	CHECK(s.guest.In(BENCH, WOOD) == 5);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.Total(STEEL) == STEEL_START);
	CHECK(s.Total(WOOD) == 5);
	// With the view closed nothing comes into the friend's copy: it holds their own again.
	s.host.Scrap(s.f, BENCH, WOOD, 1);
	s.Settle();
	CHECK(s.guest.In(BENCH, WOOD) == 0);
	CHECK(s.host.In(BENCH, WOOD) == 6);
}

TEST("shared bench: a build the host's workbench no longer held is refused, and paid from the friend's own stash there")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	// The host builds with all 10 before the friend's game has heard; the friend's copy still shows 10 and builds with 3.
	s.Build(s.host, 10);
	Pace(HOST_PEER);
	Sync(s.f, HOST_PEER);
	s.Build(s.guest, 3);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 1);
	CHECK(s.guest.storage.Stats().stashPaid == 3);  // their own 4 at the workbench pay, as their game's build would have used them
	CHECK(s.guest.Carried(STEEL) == 2);             // their pockets untouched
	CHECK(s.guest.storage.Stats().usedUp == 0);
	CHECK(s.guest.In(BENCH, STEEL) == 0);           // shown as it is
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 1);  // their stash, less the 3
	CHECK(s.Total(STEEL) == STEEL_START);  // every component the two builds used was paid for, once
}

TEST("shared bench: a refused build their stash can't cover is paid from their pockets, and what neither holds is counted")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.host, 10);
	Pace(HOST_PEER);
	Sync(s.f, HOST_PEER);
	s.Build(s.guest, 8);  // their copy still shows the host's 10
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.guest.storage.Stats().stashPaid == 4);  // all of their own stash
	CHECK(s.guest.Carried(STEEL) == 0);             // then their 2 carried
	CHECK(s.guest.storage.Stats().usedUp == 2);     // the last 2 nobody had: the build stands, 2 short of paid
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 0);
	CHECK(s.Total(STEEL) == STEEL_START + s.guest.storage.Stats().usedUp);
}

TEST("shared bench: each game's production stays its own; a friend's scripts never add to the one pile")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	// The friend's settlement makes its day's water while they build: it goes to their own stash at the workbench.
	s.guest.Script(s.f, BENCH, WATER, 5);
	s.Settle();
	CHECK(s.host.In(BENCH, WATER) == 0);
	CHECK(s.guest.In(BENCH, WATER) == 0);  // not shown: the host's has none
	CHECK(s.guest.storage.Stats().localKept == 5);
	// The host's settlement makes its own: the true copy has it, and the friend sees it.
	s.host.Script(s.f, BENCH, WATER, 3);
	s.Settle();
	CHECK(s.host.In(BENCH, WATER) == 3);
	CHECK(s.guest.In(BENCH, WATER) == 3);
	// The friend's settlers eat 2 of the steel shown (a script's take): put back, paid from their own stash.
	s.guest.Script(s.f, BENCH, STEEL, -2);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 10);
	CHECK(s.guest.In(BENCH, STEEL) == 10);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, WATER) == 5);  // their day's water, theirs
	CHECK(s.guest.In(BENCH, STEEL) == 2);  // 4, less the 2 their settlers ate
	CHECK(s.host.In(BENCH, WATER) == 3);
	CHECK(s.guest.storage.Stats().localUnpaid == 0);
}

TEST("shared bench: weapons and armor at the workbench stay each player's")
{
	Session s;
	s.host.holders[BENCH][RIFLE] = 2;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	CHECK(s.guest.In(BENCH, RIFLE) == 1);  // their own, not the host's two
	s.guest.Take(s.f, BENCH, RIFLE, 1);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.Carried(RIFLE) == 1);
	CHECK(s.host.In(BENCH, RIFLE) == 2);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
}

TEST("shared bench: a workbench the host's game never listed stays each player's own, as with the rule off")
{
	Session s;
	s.guest.holders[OTHER][STEEL] = 6;
	s.host.holders[OTHER][STEEL] = 1;
	s.guest.OpenBench(s.f, OTHER);
	s.Settle();
	CHECK(!s.guest.storage.State().Viewing());
	CHECK(s.guest.In(OTHER, STEEL) == 6);
	s.guest.Build(s.f, OTHER, STEEL, 4);
	s.guest.Scrap(s.f, OTHER, WOOD, 2);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(OTHER, STEEL) == 2);
	CHECK(s.host.In(OTHER, STEEL) == 1);
	CHECK(s.host.In(OTHER, WOOD) == 0);
	CHECK(s.host.applied == 0);
}

TEST("shared bench: a friend's load after a build gives the components back to the one pile; after a scrap takes them out")
{
	Session s;
	const auto before = s.guest.SaveGame(s.f);
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.guest, 3);
	s.guest.Scrap(s.f, BENCH, WOOD, 4);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 7);
	CHECK(s.host.In(BENCH, WOOD) == 4);

	// The load's world has neither the built piece nor the scrap: the true copy is put back as it was.
	s.guest.LoadGame(s.f, before);
	s.built -= 3;
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 10);
	CHECK(s.host.In(BENCH, WOOD) == 0);
	CHECK(s.guest.storage.State().Stats().lacking == 2);
	CHECK(s.guest.Carried(STEEL) == 2);
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	CHECK(s.Total(STEEL) == STEEL_START);
	CHECK(s.Total(WOOD) == 0);
}

TEST("shared bench: a save in workshop mode keeps none of the host's things in the friend's copy")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.guest, 1);
	const auto save = s.guest.SaveGame(s.f);  // the view ends before the save (Storage::BeforeSaveOrLoad)
	CHECK(save.holders.at(BENCH).at(STEEL) == 4);
	CHECK(!s.guest.storage.State().Viewing());
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 9);
	// Workshop mode goes on: the view opens again (Storage's BenchFrame).
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 9);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.Total(STEEL) == STEEL_START);
}

TEST("shared bench: the host leaving closes the view at once, and the friend builds with their own")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.f.fake->Drop(HOST_PEER);
	std::vector<std::vector<std::uint8_t>> got;
	CHECK(Await(s.f, FRIEND_PEER, MT::kStorageRefused, got));
	s.guest.Handle(s.f, got);
	CHECK(!s.guest.storage.State().Viewing());
	CHECK(s.guest.In(BENCH, STEEL) == 4);  // their own back, workshop mode still on
	s.guest.Build(s.f, BENCH, STEEL, 3);    // with their own: kept to their world
	Receive(s.f, s.guest);
	CHECK(s.guest.In(BENCH, STEEL) == 1);
	CHECK(s.host.In(BENCH, STEEL) == 10);
	CHECK(s.guest.loot.Stats().storageKept >= 1);
}

TEST("shared bench: a friend leaving mid-build: the build on its way goes through late when they rejoin")
{
	Session s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	s.Build(s.guest, 2, false);
	s.guest.Disconnect(s.f);
	CHECK(s.guest.storage.State().InFlight() == 1);
	CHECK(s.guest.In(BENCH, STEEL) == 4);  // their own, back in the copy
	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().late == 1);
	CHECK(s.host.In(BENCH, STEEL) == 8);
	CHECK(s.Total(STEEL) == STEEL_START);
}

// ---- the host's supply lines (SupplyLines.h) -----------------------------------------------------------------------------
//
// The host's settlements a supply line links to the one built at: the host's game tells the session which workbenches its
// supply lines link (from its own engine's gather), and a friend's game viewing the host's workbench views those too, so its
// engine's gather takes from them in the host's order, then from its own world's, then from the player.

namespace
{
	// The host's workbench at the settlement built at, and one a supply line links to it: the host's holds 5 steel there, the
	// friend's copy 3 of their own.
	struct Lines : Session
	{
		Lines()
		{
			host.holders[LINKED][STEEL] = 5;
			guest.holders[LINKED][STEEL] = 3;
			host.storage.State().Unlist(LINKED);  // listed again, as it is now (the session's first listing was empty)
			Settle();
			host.ReportLinks(f, BENCH, { LINKED });
			Settle();
		}

		std::int64_t OwnAt(std::uint32_t a_bench, std::uint32_t a_item) const
		{
			const auto& aside = guest.storage.Aside(a_bench);
			if (guest.storage.State().Views(a_bench)) {
				return aside.contains(a_item) ? aside.at(a_item) : 0;
			}
			return guest.In(a_bench, a_item);
		}

		// Every steel in the session's worlds at the two workbenches and on the players, and what builds used up.
		std::int64_t Steel() const
		{
			return host.In(BENCH, STEEL) + host.In(LINKED, STEEL) + host.Carried(STEEL) + guest.Carried(STEEL) + OwnAt(BENCH, STEEL) + OwnAt(LINKED, STEEL) + built;
		}
	};

	constexpr std::int64_t LINES_START = 10 + 5 + 4 + 3 + 2;

	bool Any(std::uint32_t)
	{
		return true;
	}
}

TEST("supply lines: the host's game tells the session its supply lines; a friend's game hears them, and only changes go out again")
{
	Lines s;
	CHECK(s.guest.Keeper(LINKED) == Keeper::kFriend);
	CHECK(s.guest.heard.With(BENCH, Any) == std::vector<std::uint32_t>{ LINKED });
	CHECK(s.host.reporter.Reports() == 1);
	s.host.ReportLinks(s.f, BENCH, { LINKED, BENCH, 0 });  // the same lines
	CHECK(s.host.reporter.Reports() == 1);
	CHECK(s.host.heard.Benches() == 0);  // the keeper isn't told its own
}

TEST("supply lines: a friend's build at the host's workbench takes from the workbenches the host's supply lines link, once")
{
	Lines s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.supply == std::vector<std::uint32_t>{ LINKED });
	CHECK(s.guest.storage.State().Shows(LINKED));
	CHECK(s.guest.In(BENCH, STEEL) == 10);
	CHECK(s.guest.In(LINKED, STEEL) == 5);  // the host's, their own 3 set aside
	CHECK(s.guest.storage.Aside(LINKED).at(STEEL) == 3);
	CHECK((s.guest.Gather() == std::vector<std::uint32_t>{ BENCH, LINKED }));
	CHECK(s.Steel() == LINES_START);

	// 13 steel: the workbench's 10, then 3 of the linked workbench's, as the host's game would take them.
	s.built += 13;
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 13);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.host.In(LINKED, STEEL) == 2);
	CHECK(s.guest.Carried(STEEL) == 2);  // their pockets untouched
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
	CHECK(s.Steel() == LINES_START);

	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(!s.guest.storage.State().Viewing());
	CHECK(s.guest.In(BENCH, STEEL) == 4);  // their own back at both
	CHECK(s.guest.In(LINKED, STEEL) == 3);
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: a take the linked workbench no longer held is refused and paid from the friend's own stash there")
{
	Lines s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	// The host builds with the linked workbench's 5 before the friend's game hears; the friend's build takes 2 there.
	s.built += 5;
	s.host.Build(s.f, LINKED, STEEL, 5);
	Pace(HOST_PEER);
	Sync(s.f, HOST_PEER);
	s.built += 12;
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 12);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.host.In(LINKED, STEEL) == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 1);
	CHECK(s.guest.storage.Stats().stashPaid == 2);  // their own 3 at the linked workbench pay
	CHECK(s.guest.Carried(STEEL) == 2);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(LINKED, STEEL) == 1);
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: a friend's take at a linked workbench neither game has loaded comes out of the host's pile at once")
{
	Lines s;
	s.host.away.insert(LINKED);   // the host far from that settlement (listed before, or from afar)
	s.guest.away.insert(LINKED);  // the friend never went there
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	CHECK(s.guest.In(LINKED, STEEL) == 5);  // the host's, their own 3 set aside

	s.built += 13;
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 13);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.host.In(LINKED, STEEL) == 2);  // the engine changes a workbench from afar: so does the friend's take
	CHECK(s.host.loot.Waiting().empty());
	CHECK(s.host.loot.Stats().fromAfar == 1);
	CHECK(s.guest.In(LINKED, STEEL) == 2);
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
	CHECK(s.Steel() == LINES_START);

	// The host's game adds there from afar (the day's production): the friend's view shows it as it comes, not only once
	// they look again.
	s.host.Script(s.f, LINKED, STEEL, 4);
	s.Settle();
	CHECK(s.host.In(LINKED, STEEL) == 6);
	CHECK(s.guest.In(LINKED, STEEL) == 6);
	CHECK(s.guest.loot.Waiting().empty());

	// All 6 used: every one comes off the host's pile, none refused, none used twice.
	s.built += 6;
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 6);
	s.Settle();
	CHECK(s.host.In(LINKED, STEEL) == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
	CHECK(s.guest.Carried(STEEL) == 2);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(LINKED, STEEL) == 3);  // their own back
	CHECK(s.guest.loot.Waiting().empty());
	CHECK(s.Steel() == LINES_START + 4);
}

TEST("supply lines: a friend's take at the host's far workbench goes in even after the host's reload, and its reload's listing counts it once")
{
	Lines s;
	const auto save = s.host.SaveGame(s.f);  // LINKED holds 5 in the save
	s.host.away.insert(LINKED);
	s.guest.away.insert(LINKED);
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	s.built += 12;
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 12);  // 10 at the workbench, 2 at LINKED
	s.Settle();
	CHECK(s.host.In(LINKED, STEEL) == 3);
	s.guest.CloseBench(s.f);
	s.Settle();
	// The host loads the save from before the take, still far from LINKED: the catch-up takes the friend's 2 again there,
	// so its world holds what the server's book does (3), and the 2 are used once.
	s.host.LoadGame(s.f, save);
	s.Settle();
	s.host.Wait(s.f, std::chrono::seconds{ 1 });
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.host.In(LINKED, STEEL) == 3);
	CHECK(s.host.loot.Waiting().empty());
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: a line the host's game takes back ends that view, and the friend's own things come back there")
{
	Lines s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	s.host.ReportLinks(s.f, BENCH, {});
	s.Settle();
	CHECK(s.guest.supply.empty());
	CHECK(!s.guest.storage.State().Views(LINKED));
	CHECK(s.guest.storage.State().Shows(BENCH));  // the workbench's own view stays
	CHECK(s.guest.In(LINKED, STEEL) == 3);
	CHECK(s.guest.Gather() == std::vector<std::uint32_t>{ BENCH });
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: a line to a workbench nobody keeps stays the friend's own, as their world's supply lines are")
{
	Lines s;
	s.host.ReportLinks(s.f, BENCH, { LINKED, OTHER });  // OTHER: never listed (not loaded in the host's world)
	s.Settle();
	s.guest.holders[OTHER][STEEL] = 6;
	s.host.holders[OTHER][STEEL] = 1;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	CHECK(s.guest.supply == std::vector<std::uint32_t>{ LINKED });
	CHECK(!s.guest.storage.State().Views(OTHER));
	// Their own world links OTHER too: it comes after the host's lines, their own.
	const auto order = s.guest.Gather({ OTHER, LINKED });
	CHECK((order == std::vector<std::uint32_t>{ BENCH, LINKED, OTHER }));
	s.built += 15;  // the two workbenches' 15; OTHER's 2 are their own world's (not in Steel())
	s.guest.BuildFrom(s.f, order, STEEL, 17);
	s.Settle();
	CHECK(s.host.In(BENCH, STEEL) == 0);
	CHECK(s.host.In(LINKED, STEEL) == 0);
	CHECK(s.guest.In(OTHER, STEEL) == 4);  // 2 of their own
	CHECK(s.host.In(OTHER, STEEL) == 1);   // the host's untouched
	CHECK(s.guest.Carried(STEEL) == 2);
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: lines sent for a workbench a game doesn't keep are dropped by the server")
{
	Lines s;
	Pace(FRIEND_PEER);
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::BenchLinks{ BENCH, { OTHER } }, MT::kReportBenchLinks));
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::BenchLinks{ OTHER, { LINKED } }, MT::kReportBenchLinks));
	s.Settle();
	CHECK(s.host.heard.Benches() == 0);
	// The friend rejoins: the table it is welcomed with is the host's.
	s.guest.Disconnect(s.f);
	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.heard.With(BENCH, Any) == std::vector<std::uint32_t>{ LINKED });
	CHECK(s.guest.heard.Benches() == 1);
}

TEST("supply lines: the host leaving closes every view at once, and the friend builds with their own")
{
	Lines s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	s.f.fake->Drop(HOST_PEER);
	std::vector<std::vector<std::uint8_t>> got;
	CHECK(Await(s.f, FRIEND_PEER, MT::kStorageRefused, got));
	Pace(FRIEND_PEER);
	for (auto& packet : s.guest.Fetch(s.f)) {
		got.push_back(std::move(packet));
	}
	s.guest.Handle(s.f, got);
	CHECK(!s.guest.storage.State().Viewing());
	CHECK(s.guest.storage.State().Viewed().empty());
	CHECK(s.guest.heard.Benches() == 0);  // the host's lines forgotten with its world
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	CHECK(s.guest.In(LINKED, STEEL) == 3);
	s.guest.BuildFrom(s.f, s.guest.Gather(), STEEL, 3);  // with their own: kept to their world
	Receive(s.f, s.guest);
	CHECK(s.guest.In(BENCH, STEEL) == 1);
	CHECK(s.host.In(LINKED, STEEL) == 5);
}

TEST("supply lines: a container menu opened while viewing the workbench and its lines ends them all first")
{
	Lines s;
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	s.guest.benchView.reset();
	s.guest.supply.clear();
	s.guest.OpenMenu(s.f, LINKED);  // the linked workbench's own container menu, say
	CHECK(s.guest.storage.State().Viewed() == std::vector<std::uint32_t>{ LINKED });
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	s.Settle();
	CHECK(s.guest.In(LINKED, STEEL) == 5);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.In(LINKED, STEEL) == 3);
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: the host's reload forgets its lines for everyone; its game tells them again from its loaded world")
{
	Lines s;
	const auto save = s.host.SaveGame(s.f);
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	REQUIRE(s.guest.storage.State().Shows(LINKED));
	s.host.LoadGame(s.f, save);
	s.Settle();
	CHECK(s.guest.heard.Benches() == 0);
	CHECK(!s.guest.storage.State().Views(LINKED));
	CHECK(s.guest.In(LINKED, STEEL) == 3);  // their own back
	// The loaded world's lines go out again (Storage::ReportSupply after OnGameLoaded's Forget).
	s.host.ReportLinks(s.f, BENCH, { LINKED });
	s.Settle();
	CHECK(s.guest.heard.With(BENCH, Any) == std::vector<std::uint32_t>{ LINKED });
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.Steel() == LINES_START);
}

namespace
{
	// The host owns a_more settlements its lines link to Sanctuary's besides LINKED: each workbench holds 1 steel there.
	// The links, LINKED first.
	std::vector<std::uint32_t> Network(Lines& a_s, std::uint32_t a_more)
	{
		std::vector<std::uint32_t> network{ LINKED };
		for (std::uint32_t i = 0; i < a_more; ++i) {
			const auto bench = 0x0002'6000 + i;
			network.push_back(bench);
			a_s.host.holders[bench][STEEL] = 1;
			for (auto* game : { &a_s.host, &a_s.guest }) {
				game->holderFacts[bench] = { .kind = LootScope::Holder::kContainer, .workshop = true, .storage = true };
			}
		}
		a_s.Settle();
		a_s.host.ReportLinks(a_s.f, BENCH, network);
		a_s.Settle();
		return network;
	}
}

TEST("supply lines: a big supply network: the friend's game views the first 32, one at a time, and the server closes none")
{
	Lines      s;
	const auto network = Network(s, SupplyLines::Views::MAX + 3);  // 36 linked: more than a player builds
	REQUIRE(s.guest.heard.With(BENCH, Any).size() == network.size());
	s.guest.OpenBench(s.f, BENCH);
	s.Settle();
	CHECK(s.guest.storage.State().Viewed().size() <= 2);  // the workbench and the first link: not every one at once
	for (int i = 0; i < 36; ++i) {
		s.guest.now += std::chrono::milliseconds{ 250 };
		Receive(s.f, s.guest);
	}
	CHECK(s.guest.storage.State().Viewed().size() == 1 + SupplyLines::Views::MAX);
	CHECK((s.guest.supply == std::vector<std::uint32_t>{ network.begin(), network.begin() + SupplyLines::Views::MAX }));
	CHECK(s.guest.supplyClosed == 0);
	CHECK(s.guest.Noticed(StorageView::Notice::kClosed) == 0);
	CHECK(s.guest.In(network[1], STEEL) == 1);   // shown: the host's
	CHECK(s.guest.In(network[SupplyLines::Views::MAX - 1], STEEL) == 1);
	CHECK(s.guest.In(network.back(), STEEL) == 0);  // past the 32: their own copy (nothing there)
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.storage.State().Viewed().empty());
	CHECK(s.Steel() == LINES_START);
}

TEST("supply lines: the server sends a workbench's view and all 32 of its lines' at once, refusing none")
{
	Lines      s;
	const auto network = Network(s, SupplyLines::Views::MAX - 1);
	// The friend's game asks for every view in one go (faster than it ever opens them): each is answered with contents.
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ BENCH, true }));
	for (const auto link : network) {
		s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ link, true }));
	}
	Pace(FRIEND_PEER);
	const auto got = s.guest.Fetch(s.f);
	std::size_t contents = 0;
	std::size_t refused = 0;
	for (const auto& packet : got) {
		contents += LootTest::IsType(packet, MT::kStorageContents) ? 1 : 0;
		refused += LootTest::IsType(packet, MT::kStorageRefused) ? 1 : 0;
	}
	CHECK(contents == 1 + SupplyLines::Views::MAX);
	CHECK(refused == 0);
}
