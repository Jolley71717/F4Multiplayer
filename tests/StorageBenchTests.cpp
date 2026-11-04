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
		for (const auto bench : { BENCH, OTHER }) {
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

TEST("shared bench: a build the host's workbench no longer held is refused, and paid from the friend's own inventory")
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
	CHECK(s.guest.Carried(STEEL) == 0);                 // 2 of the 3 paid from their pockets
	CHECK(s.guest.storage.Stats().usedUp == 1);         // the third they didn't have: counted, the build stands
	CHECK(s.guest.In(BENCH, STEEL) == 0);               // shown as it is
	s.guest.CloseBench(s.f);
	s.Settle();
	CHECK(s.guest.In(BENCH, STEEL) == 4);
	// Nothing is made twice and nothing lost from anyone's inventory; the one component neither the workbench nor the
	// friend had was used all the same (a build can't be undone): the build stands, one steel short of paid.
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
