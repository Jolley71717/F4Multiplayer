// Shared storage between the host's game and friends' games, against a real server (LootGame.h plays each game's loot
// and storage code). The host's game keeps the one true copy of a home's or a settlement's container; a friend's copy
// shows it while their menu is open, and their moves go through only if the server can check them. Every test counts
// the items in the session's worlds: whatever is refused, reloaded or cut off, nothing is duplicated and nothing is lost.

#include "LootGame.h"

#include "SessionFile.h"

#include <deque>
#include <map>
#include <thread>

namespace
{
	using LootTest::Game;
	using StorageView::Keeper;
	using StorageView::Notice;
	using namespace TestServer;

	constexpr std::uint64_t HOST_ID = 0x0000'A11C'E000'0001;
	constexpr std::uint64_t FRIEND_ID = 0x0000'B0B0'0000'0002;
	constexpr std::uint64_t OTHER_ID = 0x0000'C0C0'0000'0003;
	constexpr PeerId        HOST_PEER = 1;
	constexpr PeerId        FRIEND_PEER = 2;
	constexpr PeerId        OTHER_PEER = 3;
	constexpr std::uint32_t CHEST = 0x0001'2345;  // Home Plate's chest, say
	constexpr std::uint32_t BOX = 0x0001'2346;    // a settlement box the host's game hasn't loaded
	constexpr std::uint32_t STIMPAK = 0x0002'3736;
	constexpr std::uint32_t WATER = 0x0004'835D;
	constexpr std::uint32_t RIFLE = 0x0004'F46A;
	constexpr std::uint32_t JUNK = 0x0003'0000;  // the first of many junk items (a Take All's)

	Server::Options Options(bool a_shared = true)
	{
		Server::Options options;
		options.hostIdentity = HOST_ID;
		options.rules.sharedStorage = a_shared;
		return options;
	}

	// A session whose friend loads more often than the world-state budget allows at once (a bucket of three, the hello's
	// one of them): every world state goes at once, so each load settles before the next.
	Server::Options Reloads()
	{
		auto options = Options();
		options.worldStateRefill = std::chrono::milliseconds{ 0 };
		return options;
	}

	void Storage(Game& a_game, std::uint32_t a_holder = CHEST)
	{
		a_game.holderFacts[a_holder] = { .kind = LootScope::Holder::kContainer, .storage = true };
		a_game.items[RIFLE] = { .kind = LootScope::Item::kWeapon };
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

	// Every game takes what the server sent it, a few times over, so what one sends reaches the others and back.
	void SettleAll(Fixture& a_f, std::initializer_list<Game*> a_games)
	{
		for (int round = 0; round < 3; ++round) {
			for (auto* game : a_games) {
				Receive(a_f, *game);
			}
		}
	}

	// A raw player's game answers its catch-up (kStorageCaughtUp a_asked) with nothing to settle: until it does, with
	// the rule on, the server shows it no storage and refuses its moves there.
	void SettleRaw(Fixture& a_f, PeerId a_peer, std::uint32_t a_asked = 1)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::StorageSettle{ a_asked, 0, 0, {}, {} }));
		Pace(a_peer);
		Sync(a_f, a_peer);
	}

	// A session: the host's game (its chest holds 5 stimpaks) and a friend's (their copy holds 2 waters of their own, and
	// they carry 3 more).
	struct Session
	{
		Fixture f;
		Game    host{ HOST_PEER, HOST_ID };
		Game    guest{ FRIEND_PEER, FRIEND_ID };

		explicit Session(bool a_shared = true) :
			Session(Options(a_shared))
		{
		}

		explicit Session(Server::Options a_options) :
			f(std::move(a_options))
		{
			f.fake->SetLocal(HOST_PEER);
			host.host = true;
			Storage(host);
			Storage(guest);
			host.holders[CHEST][STIMPAK] = 5;
			guest.holders[CHEST][WATER] = 2;
			guest.carried[WATER] = 3;
			host.Join(f, "Host");
			guest.Join(f, "Guest");
			SettleAll(f, { &host, &guest });
		}

		void Settle() { SettleAll(f, { &host, &guest }); }

		// The items of a_item in the session's worlds: the host's chest, both players' inventories, and the friend's copy's
		// own things (in their copy, or set aside while it shows the host's). Moves still on their way aren't anywhere: a
		// test settles first.
		std::int64_t Total(std::uint32_t a_item) const
		{
			const auto& aside = guest.storage.Aside();
			const auto  own = guest.storage.State().Viewing() ? (aside.contains(a_item) ? aside.at(a_item) : 0) : guest.In(CHEST, a_item);
			const auto  stashed = stash.contains(a_item) ? stash.at(a_item) : 0;
			return host.In(CHEST, a_item) + host.Carried(a_item) + guest.Carried(a_item) + own + stashed;
		}

		// What the friend's player put where the session never hears of it (their workbench, a companion): out of their
		// inventory, still in their world.
		std::map<std::uint32_t, std::int64_t> stash;

		void Stash(std::uint32_t a_item, std::int64_t a_count)
		{
			const auto n = (std::min)(guest.Carried(a_item), a_count);
			guest.carried[a_item] -= n;
			stash[a_item] += n;
		}
	};
}

TEST("shared storage: the host's game claims its storage, and a friend's copy shows it only while their menu is open")
{
	Session s;
	CHECK(s.host.Keeper(CHEST) == Keeper::kUs);
	CHECK(s.guest.Keeper(CHEST) == Keeper::kFriend);
	CHECK(s.host.storage.State().Stats().listed == 1);

	s.guest.OpenMenu(s.f, CHEST);
	CHECK(s.guest.In(CHEST, WATER) == 0);   // their copy's own waters are set aside: the copy shows the host's now
	CHECK(s.guest.Carried(WATER) == 3);     // and never come to them
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 5);

	s.guest.CloseMenu(s.f);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // the session's things leave the copy with the menu
	CHECK(s.guest.In(CHEST, WATER) == 2);    // and their own come back
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 5);
	CHECK(s.Total(STIMPAK) == 5);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a friend's take and put go through to the host's chest, and come back to nobody twice")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, STIMPAK, 3);
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 2);
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.guest.Carried(STIMPAK) == 3);
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.guest.In(CHEST, STIMPAK) == 2);
	CHECK(s.guest.In(CHEST, WATER) == 3);
	CHECK(s.guest.storage.State().InFlight() == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(STIMPAK) == 5);
	CHECK(s.Total(WATER) == 5);

	// The host takes what the friend put in, at its own chest: an ordinary move of the keeper's.
	s.host.Take(s.f, CHEST, WATER, 3);
	s.Settle();
	CHECK(s.host.Carried(WATER) == 3);
	CHECK(s.guest.In(CHEST, WATER) == 2);  // nothing comes into the closed copy: it holds their own again
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: two friends taking the last stimpaks at once: one gets them, the other's take is given back")
{
	Session s;
	Game other{ OTHER_PEER, OTHER_ID };
	Storage(other);
	other.Join(s.f, "Other");
	SettleAll(s.f, { &s.host, &s.guest, &other });
	s.guest.OpenMenu(s.f, CHEST);
	other.OpenMenu(s.f, CHEST);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	CHECK(other.In(CHEST, STIMPAK) == 5);

	// Both take 4 before either hears of the other's.
	s.guest.Take(s.f, CHEST, STIMPAK, 4);
	other.Take(s.f, CHEST, STIMPAK, 4);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	CHECK(s.host.In(CHEST, STIMPAK) == 1);
	CHECK(s.guest.Carried(STIMPAK) + other.Carried(STIMPAK) == 4);
	CHECK(s.guest.storage.State().Stats().refusedTakes + other.storage.State().Stats().refusedTakes == 1);
	// Each copy shows what the chest holds.
	CHECK(s.guest.In(CHEST, STIMPAK) == 1);
	CHECK(other.In(CHEST, STIMPAK) == 1);
	s.guest.CloseMenu(s.f);
	other.CloseMenu(s.f);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	CHECK(s.host.In(CHEST, STIMPAK) + s.guest.Carried(STIMPAK) + other.Carried(STIMPAK) == 5);
}

TEST("shared storage: with the menu closed, the friend's copy is theirs: their moves stay there, and nobody's come in")
{
	Session s;
	s.guest.Take(s.f, CHEST, WATER, 1);  // their own water, from their own copy
	s.host.Take(s.f, CHEST, STIMPAK, 2);
	s.Settle();
	CHECK(s.guest.loot.Stats().storageKept == 1);
	CHECK(s.guest.loot.Stats().storageSkipped >= 1);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.host.In(CHEST, STIMPAK) == 3);
	CHECK(s.Total(STIMPAK) == 5);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: what isn't shared (a weapon) stays in the friend's copy as their own")
{
	Session s;
	s.guest.holders[CHEST][RIFLE] = 1;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, RIFLE) == 1);
	CHECK(s.guest.Carried(RIFLE) == 0);
	s.guest.Take(s.f, CHEST, RIFLE, 1);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(RIFLE) == 1);
	CHECK(s.host.In(CHEST, RIFLE) == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
}

TEST("shared storage: a put onto a stack of their copy's that fills a quest alias stays theirs, in their copy")
{
	Session s;
	s.guest.questStacks.insert({ CHEST, WATER });
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 1);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.guest.loot.Stats().storageKept == 1);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: when the host leaves, the friend's view closes, their menu closes, and their copy is theirs again")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.f.fake->Drop(HOST_PEER);
	std::vector<std::vector<std::uint8_t>> got;
	CHECK(Await(s.f, FRIEND_PEER, MT::kStorageRefused, got));
	s.guest.Handle(s.f, got);
	CHECK(!s.guest.storage.State().Viewing());  // the menu Storage hid has closed
	CHECK(s.guest.storage.State().Stats().closedByHost == 1);
	CHECK(s.guest.Noticed(Notice::kClosed) == 1);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // the host's things left the copy
	CHECK(s.guest.Carried(STIMPAK) == 0);
	CHECK(s.guest.In(CHEST, WATER) == 2);
	CHECK(s.guest.Carried(WATER) == 3);
}

TEST("shared storage: the server closing a view with moves not yet sent: they go first, are refused, and undone")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3, false);    // made in the menu, the frame not come yet
	s.guest.Take(s.f, CHEST, STIMPAK, 2, false);
	s.f.fake->Drop(HOST_PEER);
	std::vector<std::vector<std::uint8_t>> got;
	CHECK(Await(s.f, FRIEND_PEER, MT::kStorageRefused, got));
	s.guest.Handle(s.f, got);
	CHECK(!s.guest.storage.State().Viewing());
	Receive(s.f, s.guest);  // the moves' refusals
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.Carried(WATER) == 3);    // the put returned
	CHECK(s.guest.Carried(STIMPAK) == 0);  // the take given back
	CHECK(s.guest.In(CHEST, WATER) == 2);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	CHECK(s.guest.storage.State().Stats().refusedPuts == 1);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 1);
}

TEST("shared storage: a friend's take in flight as the host's world reloads is refused, and the chest opens again once listed")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	const auto save = s.host.SaveGame(s.f);
	s.host.LoadGame(s.f, save);  // the server holds the host's listings stale from its request on
	Pace(HOST_PEER);
	Sync(s.f, HOST_PEER);
	s.guest.Take(s.f, CHEST, STIMPAK, 2);
	Receive(s.f, s.guest);
	Receive(s.f, s.guest);
	CHECK(s.guest.Carried(STIMPAK) == 0);  // refused, given back
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	CHECK(!s.guest.storage.State().Viewing());

	// The host's world has its answer and lists the chest again; the friend opens it again.
	s.Settle();
	CHECK(s.host.storage.State().Stats().listed == 2);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 5);
	s.guest.Take(s.f, CHEST, STIMPAK, 2);
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 3);
	CHECK(s.guest.Carried(STIMPAK) == 2);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(STIMPAK) == 5);
}

TEST("shared storage: a take the host's chest held less of than the server knew is given back by the taker")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.host.holders[CHEST][STIMPAK] = 1;  // a script took 4 in the host's world, unreported
	s.guest.Take(s.f, CHEST, STIMPAK, 4);
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 0);
	CHECK(s.host.storage.State().Stats().shorts == 1);
	CHECK(s.guest.storage.State().Stats().shortTakes == 1);
	CHECK(s.guest.Carried(STIMPAK) == 1);     // 4 taken, 3 of them never there
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);   // shown as it is: the host's game listed it again
	CHECK(s.host.storage.State().Stats().listed == 2);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(STIMPAK) == 1);
}

TEST("shared storage: a friend's reload after a put: the chest gives the put up, and the loaded player keeps what their save has")
{
	Session s{ Reloads() };
	const auto before = s.guest.SaveGame(s.f);  // carrying 3 waters
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);

	s.guest.LoadGame(s.f, before);
	CHECK(s.guest.Carried(WATER) == 3);  // the save's
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);  // never taken from them again: they could have moved it away first
	CHECK(s.guest.storage.State().Stats().lacking == 1);
	CHECK(s.guest.storage.State().Stats().settles == 2);  // the hello's, and the load's
	CHECK(s.host.In(CHEST, WATER) == 0);  // the true copy gave the put up
	CHECK(s.f.Logged("storage undid 1 and did again 0 of player"));
	CHECK(s.Total(WATER) == 5);

	// The loop that duped: put, quickload, take again. The chest holds none of them now.
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, WATER) == 0);
	s.guest.Take(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.Total(WATER) == 5);

	// And again: put, quickload. The chest gives up the second put too, once.
	const auto again = s.guest.SaveGame(s.f);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	s.guest.LoadGame(s.f, again);
	s.Settle();
	s.guest.LoadGame(s.f, again);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a friend's reload after a take: the chest has the take back, and the loaded player keeps what their save has")
{
	Session s;
	const auto before = s.guest.SaveGame(s.f);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, STIMPAK, 2);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 3);

	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 0);   // the save's
	CHECK(s.host.In(CHEST, STIMPAK) == 5);  // never lost: the true copy has them again
	CHECK(s.Total(STIMPAK) == 5);
}

TEST("shared storage: an older save then a newer one: the chest gives up the put for the first, and has it again for the second")
{
	Session s{ Reloads() };
	const auto older = s.guest.SaveGame(s.f);  // carrying 3 waters
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	const auto newer = s.guest.SaveGame(s.f);  // the put made: carrying none
	CHECK(newer.lacks.empty());

	s.guest.LoadGame(s.f, older);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.Total(WATER) == 5);
	const auto lacking = s.guest.SaveGame(s.f);  // the older world, with the put it lacks kept in its record
	CHECK(lacking.lacks.size() == 1);

	s.guest.LoadGame(s.f, newer);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.host.In(CHEST, WATER) == 3);  // done again: the newer world has the put
	CHECK(s.f.Logged("storage undid 0 and did again 1 of player"));
	CHECK(s.Total(WATER) == 5);

	s.guest.LoadGame(s.f, lacking);  // the save made in the older world: lacking it again
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.Total(WATER) == 5);

	s.guest.LoadGame(s.f, lacking);  // the same again: nothing more to undo
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.Total(WATER) == 5);
	CHECK(!s.f.Logged("has asked for the world too often"));  // each load settled before the next
}

TEST("shared storage: a save made with a put unanswered, loaded again: the put is in it, and the chest keeps it")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	const auto during = s.guest.SaveGame(s.f);  // before the put's number came back
	CHECK(during.unanswered.size() == 1);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);

	s.guest.LoadGame(s.f, during);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.guest.storage.State().Stats().lacking == 0);
	CHECK(s.guest.storage.State().Stats().late == 0);
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: moves cut off by a disconnect stay on their way: the rejoin's catch-up numbers one, the other goes late")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	// One put the server gets (its answer lost with the connection), one it never does (still in the frame as it goes).
	s.guest.Put(s.f, CHEST, WATER, 1);
	Pace(FRIEND_PEER);
	Sync(s.f, FRIEND_PEER);  // numbered, and its echo sent to the connection about to go
	s.guest.Put(s.f, CHEST, WATER, 2, false);
	s.guest.Disconnect(s.f);
	CHECK(s.guest.Carried(WATER) == 0);  // nothing given back: neither is known lost
	CHECK(s.guest.storage.State().InFlight() == 2);
	CHECK(s.guest.In(CHEST, WATER) == 2);  // their own, back in the copy
	s.host.Receive(s.f);
	CHECK(s.host.In(CHEST, WATER) == 1);  // the first got through

	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().late == 1);
	CHECK(s.guest.storage.State().Stats().lacking == 0);
	CHECK(s.host.In(CHEST, WATER) == 3);  // the second went through late
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a move with no answer stays on its way: nothing is given back, and the next settle sends it late")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3, false);
	s.guest.FlushLoot();
	s.guest.outbox.clear();  // lost on its way (as a server past its budgets answers nothing)
	CHECK(s.guest.storage.State().InFlight() == 1);
	s.guest.Wait(s.f, StorageView::ANSWER_WITHIN);
	CHECK(s.guest.Carried(WATER) == 0);   // not given back: a number may still come
	CHECK(s.guest.In(CHEST, WATER) == 3);  // shown on its way
	CHECK(s.guest.storage.State().Stats().unanswered == 1);
	CHECK(s.guest.Noticed(Notice::kPutRefused) == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);

	// The next settle (a rejoin here) sends it late, and the true copy has it.
	s.guest.Disconnect(s.f);
	s.host.Receive(s.f);
	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a put whose number comes after ANSWER_WITHIN is matched when it comes: nothing given back or taken twice")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);  // sent and numbered; its echo not read for a while
	s.guest.Wait(s.f, StorageView::ANSWER_WITHIN);
	CHECK(s.guest.Carried(WATER) == 0);
	s.Stash(WATER, 3);  // nothing to move away
	s.Settle();
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.guest.storage.State().Stats().lacking == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a put the session's numbering can't carry is given back at once")
{
	Session s;
	constexpr std::uint32_t MOD_ITEM = 0x0100'0800;
	s.guest.untranslatable.insert(MOD_ITEM);
	s.guest.carried[MOD_ITEM] = 1;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, MOD_ITEM, 1);
	CHECK(s.guest.Carried(MOD_ITEM) == 1);
	CHECK(s.guest.In(CHEST, MOD_ITEM) == 0);
	CHECK(s.guest.storage.State().Stats().unsent == 1);
	s.Settle();
	CHECK(s.host.In(CHEST, MOD_ITEM) == 0);
}

TEST("shared storage: a Take All of 400 stacks goes through whole, past the old in-flight cap and the event budget")
{
	Session s;
	constexpr std::uint32_t STACKS = 400;
	for (std::uint32_t i = 0; i < STACKS; ++i) {
		s.host.holders[CHEST][JUNK + i] = 1;
	}
	s.host.storage.State().Unlist(CHEST);  // listed again, with them
	s.Settle();
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	REQUIRE(s.guest.In(CHEST, JUNK + STACKS - 1) == 1);
	for (std::uint32_t i = 0; i < STACKS; ++i) {
		s.guest.Take(s.f, CHEST, JUNK + i, 1, false);
	}
	s.guest.Frame(s.f);
	s.Settle();
	s.Settle();
	std::int64_t carried = 0;
	std::int64_t left = 0;
	for (std::uint32_t i = 0; i < STACKS; ++i) {
		carried += s.guest.Carried(JUNK + i);
		left += s.host.In(CHEST, JUNK + i);
	}
	CHECK(carried == STACKS);
	CHECK(left == 0);
	CHECK(s.guest.storage.State().Stats().refusedTakes == 0);
	CHECK(s.guest.storage.State().Stats().overflow == 0);
	CHECK(s.guest.storage.State().InFlight() == 0);
}

TEST("shared storage: a friend's copy that mirrors the host's chest (an older session's) is set aside, never given")
{
	Session s;
	s.guest.holders[CHEST][STIMPAK] = 5;  // what 0.9.x's loot sync put in their copy: the host's stimpaks
	s.guest.OpenMenu(s.f, CHEST);
	CHECK(s.guest.Carried(STIMPAK) == 0);
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 5);  // the host's, shown
	s.guest.Take(s.f, CHEST, STIMPAK, 5);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 5);  // the host's five, once
	CHECK(s.host.In(CHEST, STIMPAK) == 0);
	CHECK(s.guest.In(CHEST, STIMPAK) == 5);  // the mirror is back where it was, out of reach while the host keeps the chest
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	s.guest.CloseMenu(s.f);
	CHECK(s.Total(STIMPAK) == 10);  // the host's 5 and the mirror's 5, as there were
}

TEST("shared storage: storage the host's game hasn't claimed is ordinary loot while the rule is on")
{
	Session s;
	Storage(s.guest, BOX);
	Storage(s.host, BOX);
	s.host.away.insert(BOX);
	s.guest.holders[BOX][WATER] = 3;
	s.guest.Take(s.f, BOX, WATER, 3);
	s.Settle();
	CHECK(s.guest.Keeper(BOX) == Keeper::kNone);
	CHECK(s.guest.loot.Stats().storageKept == 0);
	CHECK(s.host.Keeper(BOX) == Keeper::kNone);  // the host's own: claimed once its game has it loaded
	CHECK(s.host.storage.State().OwnerOf(BOX) == std::nullopt);
}

TEST("shared storage: off (the default), every game keeps its own copy and moves go as before")
{
	Session s{ false };
	CHECK(s.guest.caughtUp == 0);  // no settling with the rule off
	CHECK(s.host.Keeper(CHEST) == Keeper::kNone);
	CHECK(s.guest.Keeper(CHEST) == Keeper::kNone);
	CHECK(s.host.storage.State().Stats().listed == 0);
	s.guest.OpenMenu(s.f, CHEST);
	CHECK(s.guest.In(CHEST, WATER) == 2);
	s.guest.Take(s.f, CHEST, WATER, 1);
	s.host.Take(s.f, CHEST, STIMPAK, 1);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // removeitem takes what is there: their copy had none
	CHECK(s.guest.loot.Stats().storageKept == 0);
	CHECK(s.guest.storage.State().Stats().views == 0);
}

TEST("shared storage: only the host's game claims storage: a friend's listing is nobody's truth")
{
	Session s;
	Storage(s.guest, BOX);
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageListing{ BOX, 0, { { WATER, 9 } } }, MT::kReportStorage));
	s.Settle();
	CHECK(s.guest.storage.State().OwnerOf(BOX) == std::nullopt);
	CHECK(s.host.storage.State().OwnerOf(BOX) == std::nullopt);
}

TEST("shared storage: the host's game doesn't list a container whose inventory isn't made yet")
{
	Session s;
	Storage(s.host, BOX);
	s.host.unstocked.insert(BOX);
	s.Settle();
	CHECK(s.host.storage.State().OwnerOf(BOX) == std::nullopt);
	s.host.unstocked.clear();
	s.Settle();
	CHECK(s.host.storage.State().OwnerOf(BOX) == s.host.localId);
}

TEST("shared storage: a friend's put of an item from a mod the host's game lacks is refused, and given back")
{
	Fixture f{ Options() };
	f.fake->SetLocal(HOST_PEER);
	auto hostHello = Fixture::MakeHello("Host", HOST_ID);
	hostHello.plugins = { "Fallout4.esm" };
	REQUIRE(f.Join(HOST_PEER, hostHello));
	auto guestHello = Fixture::MakeHello("Guest", FRIEND_ID);
	guestHello.contentHash = 0x9999;
	guestHello.plugins = { "Fallout4.esm", "Mod.esp" };
	REQUIRE(f.Join(FRIEND_PEER, guestHello));
	SettleRaw(f, FRIEND_PEER);
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	Sync(f, FRIEND_PEER);
	constexpr std::uint32_t MOD_ITEM = 0x0100'0800;  // Mod.esp's, in the session's numbering
	for (int i = 0; i < 3; ++i) {  // the host's numbering is made once, and asked each time
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, MOD_ITEM, 1 }, MT::kReportContainer));
	}
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 1 }, MT::kReportContainer));
	const auto got = Received(f, FRIEND_PEER);
	const auto refused = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refused.size() == 3);
	CHECK((refused[0] == Protocol::StorageRefusal{ CHEST, MOD_ITEM, 1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kCantHold }));
	const auto changes = Decoded(got, MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].change.item == WATER);
}

TEST("shared storage: a flood of moves at a friend's storage is bounded: past the budgets nothing is checked or answered")
{
	Fixture f{ Options() };
	f.fake->SetLocal(HOST_PEER);
	REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	Received(f, FRIEND_PEER);
	SettleRaw(f, FRIEND_PEER);
	// Not viewing: every one is refused, up to the budgets (a burst of moves, then of refusals), then nothing.
	constexpr std::size_t FLOOD = 3 * Protocol::MAX_STORAGE_MOVES_UNANSWERED;
	for (std::size_t i = 0; i < FLOOD; ++i) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, -1 }, MT::kReportContainer));
	}
	const auto got = Received(f, FRIEND_PEER);
	const auto refusals = Count(got, MT::kStorageRefused);
	CHECK(refusals >= Protocol::MAX_STORAGE_MOVES_UNANSWERED);
	CHECK(refusals < FLOOD);
	CHECK(Count(got, MT::kContainerChanged) == 0);

	// Views: a few a second; one past that is answered closed.
	std::size_t shown = 0;
	std::size_t closed = 0;
	for (int i = 0; i < 40; ++i) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	}
	const auto views = Received(f, FRIEND_PEER);
	shown = Count(views, MT::kStorageContents);
	closed = Count(views, MT::kStorageRefused);
	CHECK(shown < 40);
	CHECK(shown >= 16);
	CHECK(shown + closed <= 40);
}

TEST("shared storage: who keeps each container outlasts a restart, closed to friends until the host's game lists it again")
{
	TempSessionFile file;
	auto            options = Options();
	options.sessionFile = file.path.string();
	{
		Fixture f{ options };
		f.fake->SetLocal(HOST_PEER);
		REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
		f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
		Sync(f, HOST_PEER);
	}  // stopping writes the session file
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	REQUIRE(saved->storageOwners.size() == 1);

	Fixture f{ options };
	f.fake->SetLocal(HOST_PEER);
	REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	const auto welcome = Received(f, FRIEND_PEER);
	SettleRaw(f, FRIEND_PEER);
	const auto owners = Decoded(welcome, MT::kStorageOwners, [](auto a_data) { return Protocol::DecodeStorageOwners(a_data); });
	REQUIRE(owners.size() == 1);
	REQUIRE(owners[0].entries.size() == 1);
	CHECK(owners[0].entries[0].container == CHEST);

	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, -1 }, MT::kReportContainer));
	const auto closed = Received(f, FRIEND_PEER);
	CHECK(Count(closed, MT::kStorageContents) == 0);
	CHECK(Count(closed, MT::kContainerChanged) == 0);
	const auto refusals = Decoded(closed, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refusals.size() == 2);
	CHECK(refusals[0].item == 0);
	CHECK(refusals[1].reason == Protocol::StorageReason::kClosed);

	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 4 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	const auto shown = Decoded(Received(f, FRIEND_PEER), MT::kStorageContents, [](auto a_data) { return Protocol::DecodeStorageListing(a_data); });
	REQUIRE(shown.size() == 1);
	CHECK((shown[0].stacks == std::vector<Protocol::StorageStack>{ { STIMPAK, 4 } }));
}

TEST("shared storage: a run with the rule off keeps who keeps each container, so turning it on again takes back nothing friends put in")
{
	TempSessionFile file;
	auto            on = Options();
	on.sessionFile = file.path.string();
	auto off = Options(false);
	off.sessionFile = file.path.string();
	std::int64_t hostChest = 0;     // the waters in the host's chest
	std::int64_t friendCarries = 20;
	{
		Fixture f{ on };
		f.fake->SetLocal(HOST_PEER);
		REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
		REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
		f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
		Sync(f, HOST_PEER);
		SettleRaw(f, FRIEND_PEER);
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 20 }, MT::kReportContainer));  // #0, in the host's copy alone
		Sync(f, FRIEND_PEER);
		const auto put = Decoded(Received(f, HOST_PEER), MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
		REQUIRE(put.size() == 1);
		hostChest += put[0].change.count;
		friendCarries -= 20;
	}
	REQUIRE(hostChest == 20);
	{
		Fixture f{ off };  // the host tries the session without the rule for a while
		f.fake->SetLocal(HOST_PEER);
		REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
		REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
		Received(f, FRIEND_PEER);
		// The book is kept, not asked: the friend's game is shown nothing of the host's chest.
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
		Sync(f, FRIEND_PEER);
		const auto got = Received(f, FRIEND_PEER);
		CHECK(Count(got, MT::kStorageContents) == 0);
		CHECK(Count(got, MT::kStorageRefused) == 0);
	}
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	CHECK(saved->storageOwners.size() == 1);

	Fixture f{ on };  // and turns it on again
	f.fake->SetLocal(HOST_PEER);
	REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	Received(f, HOST_PEER);
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 1, { { STIMPAK, 5 }, { WATER, 20 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	for (const auto& change : Decoded(Received(f, HOST_PEER), MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); })) {
		if (change.change.item == WATER) {
			hostChest += change.change.count;
		}
	}
	CHECK(!f.Logged("claimed:"));
	CHECK(hostChest == 20);  // the friend's 20 are still in the host's chest
	CHECK(hostChest + friendCarries == 20);
	SettleRaw(f, FRIEND_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	const auto shown = Decoded(Received(f, FRIEND_PEER), MT::kStorageContents, [](auto a_data) { return Protocol::DecodeStorageListing(a_data); });
	REQUIRE(shown.size() == 1);
	CHECK((shown[0].stacks == std::vector<Protocol::StorageStack>{ { STIMPAK, 5 }, { WATER, 20 } }));
}

// ---- the routes the reviews found: the true copy is put right, never the player, who could move the items away first ----

TEST("shared storage: put, leave, load an older save, move the items to a workbench, rejoin: the chest gives the put up")
{
	Session s;
	const auto before = s.guest.SaveGame(s.f);  // carrying 3 waters
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	REQUIRE(s.host.In(CHEST, WATER) == 3);

	s.guest.Disconnect(s.f);
	s.host.Receive(s.f);
	s.guest.Restart(before);
	CHECK(s.guest.Carried(WATER) == 3);
	s.Stash(WATER, 3);  // into their workbench, never reported
	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.storage.State().Stats().lacking == 1);
	CHECK(s.host.In(CHEST, WATER) == 0);  // the put undone in the true copy
	CHECK(s.Total(WATER) == 5);

	// Taking the chest's waters on top of the workbench's finds none.
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, WATER) == 0);
	s.guest.Take(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: quickloads whose catch-up is owed: the friend can't view or take until it has come and they have settled")
{
	auto options = Options();
	options.worldStateRefill = std::chrono::hours{ 1 };  // owed until the test moves the server clock past it, so a busy PC can't refill it early
	Session s{ options };
	const auto before = s.guest.SaveGame(s.f);  // carrying 3 waters
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	REQUIRE(s.host.In(CHEST, WATER) == 3);

	// Quickloads spend the world-state budget: the last one's catch-up is owed.
	for (int i = 0; i < 4; ++i) {
		s.guest.LoadGame(s.f, before);
	}
	Pace(FRIEND_PEER);
	Sync(s.f, FRIEND_PEER);
	REQUIRE(s.f.Logged("has asked for the world too often"));
	Receive(s.f, s.guest);
	CHECK(s.guest.storage.State().Settling());
	s.Stash(WATER, 3);  // the window a player could use

	// Their game won't open the host's chest meanwhile...
	s.guest.OpenMenu(s.f, CHEST);
	Receive(s.f, s.guest);
	CHECK(!s.guest.storage.State().Viewing());
	CHECK(!s.guest.menu);
	CHECK(s.guest.storage.State().Stats().settlingClosed == 1);
	CHECK(s.guest.Noticed(Notice::kClosed) == 1);
	// ...nor will the server, for a game that tries anyway (a modified one): no contents, and the Take All refused.
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, -3 }, MT::kReportContainer));
	Pace(FRIEND_PEER);
	const auto got = Received(s.f, FRIEND_PEER);
	CHECK(Count(got, MT::kStorageContents) == 0);
	CHECK(Count(got, MT::kContainerChanged) == 0);
	const auto refusals = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refusals.size() == 2);
	CHECK(refusals[0].item == 0);  // the view, closed
	CHECK(refusals[1].reason == Protocol::StorageReason::kClosed);
	s.host.Receive(s.f);
	CHECK(s.host.In(CHEST, WATER) == 3);

	// The owed catch-up comes: the put is undone in the true copy, and they can view it again.
	s.f.Advance(std::chrono::hours{ 1 } + std::chrono::seconds{ 1 });
	s.Settle();
	CHECK(!s.guest.storage.State().Settling());
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.Total(WATER) == 5);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.storage.State().Viewing() == CHEST);
	CHECK(s.guest.In(CHEST, WATER) == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a Take All numbered, its answer lost as the connection goes, the items moved away: the rejoin keeps it")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, STIMPAK, 5);
	Pace(FRIEND_PEER);
	Sync(s.f, FRIEND_PEER);  // numbered; the echo is lost with the connection
	s.Stash(STIMPAK, 5);
	s.guest.Disconnect(s.f);
	CHECK(s.guest.storage.State().InFlight() == 1);  // not undone: nothing is taken back from the player
	s.host.Receive(s.f);
	CHECK(s.host.In(CHEST, STIMPAK) == 0);

	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.storage.State().InFlight() == 0);  // the catch-up numbered it: in their world
	CHECK(s.guest.storage.State().Stats().late == 0);
	CHECK(s.host.In(CHEST, STIMPAK) == 0);
	CHECK(s.Total(STIMPAK) == 5);
}

TEST("shared storage: a Take All never sent as the connection goes, the items moved away: the rejoin sends it late")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, STIMPAK, 5, false);  // still in the frame as the connection goes
	s.Stash(STIMPAK, 5);
	s.guest.Disconnect(s.f);
	s.host.Receive(s.f);
	CHECK(s.host.In(CHEST, STIMPAK) == 5);

	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.storage.State().Stats().late == 1);
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.host.In(CHEST, STIMPAK) == 0);  // the true copy has the take
	CHECK(s.Total(STIMPAK) == 5);
}

TEST("shared storage: a take cut off by a disconnect, saved, loaded and rejoined: the save keeps it, numbered or not")
{
	for (const bool numbered : { true, false }) {
		Session s;
		s.guest.OpenMenu(s.f, CHEST);
		s.Settle();
		s.guest.Take(s.f, CHEST, STIMPAK, 5, numbered);
		if (numbered) {
			Pace(FRIEND_PEER);
			Sync(s.f, FRIEND_PEER);
		}
		s.Stash(STIMPAK, 5);
		s.guest.Disconnect(s.f);
		s.host.Receive(s.f);
		const auto saved = s.guest.SaveGame(s.f);  // the take on its way, in the save's record
		CHECK(saved.unanswered.size() == 1);
		s.guest.Restart(saved);
		s.guest.Join(s.f, "Guest");
		s.Settle();
		CHECK(s.guest.storage.State().InFlight() == 0);
		CHECK(s.guest.storage.State().Stats().late == (numbered ? 0u : 1u));
		CHECK(s.guest.storage.State().Stats().lacking == 0);
		CHECK(s.host.In(CHEST, STIMPAK) == 0);
		CHECK(s.Total(STIMPAK) == 5);
	}
}

TEST("shared storage: a put given back by an older build's timeout can't come back: a put unanswered stays on its way, moved away or not")
{
	// The route of the second review: a put given back unanswered, moved away, then numbered. Nothing is given back now.
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3, false);
	s.guest.FlushLoot();
	auto lost = std::exchange(s.guest.outbox, {});  // held up on its way
	s.guest.Wait(s.f, 2 * StorageView::ANSWER_WITHIN);
	CHECK(s.guest.Carried(WATER) == 0);
	for (auto& packet : lost) {
		s.f.fake->Deliver(FRIEND_PEER, std::move(packet));  // it comes after all
	}
	s.Settle();
	CHECK(s.guest.storage.State().InFlight() == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a put undone in the true copy that another friend took meanwhile: what is left comes out, nothing goes negative")
{
	Session s;
	Game other{ OTHER_PEER, OTHER_ID };
	Storage(other);
	other.Join(s.f, "Other");
	SettleAll(s.f, { &s.host, &s.guest, &other });
	const auto before = s.guest.SaveGame(s.f);
	s.guest.OpenMenu(s.f, CHEST);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	other.OpenMenu(s.f, CHEST);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	other.Take(s.f, CHEST, WATER, 2);
	other.CloseMenu(s.f);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	CHECK(other.Carried(WATER) == 2);

	s.guest.LoadGame(s.f, before);
	SettleAll(s.f, { &s.host, &s.guest, &other });
	CHECK(s.host.In(CHEST, WATER) == 0);  // the 1 left taken out; the 2 the other friend has stay theirs
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.host.storage.State().Stats().shorts == 0);
}

TEST("shared storage: a take from a chest nobody kept yet, then claimed, isn't undone after a reload")
{
	Session s;
	Storage(s.host, BOX);
	Storage(s.guest, BOX);
	s.host.away.insert(BOX);
	s.host.holders[BOX][STIMPAK] = 5;
	s.guest.holders[BOX][STIMPAK] = 5;  // each game's own roll of it
	s.Settle();
	const auto before = s.guest.SaveGame(s.f);
	s.guest.Take(s.f, BOX, STIMPAK, 2);  // ordinary loot: nobody keeps it
	s.Settle();
	CHECK(s.guest.Keeper(BOX) == Keeper::kNone);

	s.host.away.erase(BOX);  // the host comes near: the take goes into its copy, then it claims the box
	s.host.Wait(s.f, LootTest::APPLY_INTERVAL);
	s.Settle();
	REQUIRE(s.guest.Keeper(BOX) == Keeper::kFriend);
	CHECK(s.host.In(BOX, STIMPAK) == 3);
	CHECK(s.guest.storage.State().KeptSince(BOX) > 0);

	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 0);  // their loaded copy holds the 5 it had
	CHECK(s.guest.In(BOX, STIMPAK) == 5);
	CHECK(s.host.In(BOX, STIMPAK) == 3);  // not put back: it was ordinary loot, which both worlds agree on
	CHECK(s.guest.storage.State().Stats().lacking == 0);
}

namespace
{
	// Water across the session's worlds with the box in play: the host's box and chest, the friend's inventory, and the
	// friend's own copies of both (their stash; set aside while they view the host's).
	std::int64_t Waters(Session& a_s)
	{
		const auto&  aside = a_s.guest.storage.Aside();
		const auto   viewing = a_s.guest.storage.State().Viewing();
		std::int64_t own = 0;
		for (const auto holder : { CHEST, BOX }) {
			own += viewing == holder ? 0 : a_s.guest.In(holder, WATER);
		}
		own += viewing && aside.contains(WATER) ? aside.at(WATER) : 0;
		return a_s.host.In(BOX, WATER) + a_s.host.In(CHEST, WATER) + a_s.host.Carried(WATER) + a_s.guest.Carried(WATER) + own;
	}
}

TEST("shared storage: a put into a chest nobody kept yet comes back out of the host's copy as its game claims it; the friend's copy keeps it")
{
	Session s;
	Storage(s.host, BOX);
	Storage(s.guest, BOX);
	s.host.away.insert(BOX);
	s.Settle();
	s.guest.Put(s.f, BOX, WATER, 3);  // ordinary loot: nobody keeps it, and the host's copy will have it too
	s.Settle();
	CHECK(s.guest.Keeper(BOX) == Keeper::kNone);
	CHECK(Waters(s) == 5);

	s.host.away.erase(BOX);  // the host comes near: the put goes into its copy, then it claims the box
	s.host.Wait(s.f, LootTest::APPLY_INTERVAL);
	s.Settle();
	REQUIRE(s.guest.Keeper(BOX) == Keeper::kFriend);
	CHECK(s.host.In(BOX, WATER) == 0);   // given up at the claim
	CHECK(s.guest.In(BOX, WATER) == 3);  // their stash, in their copy
	CHECK(s.f.Logged("claimed: 3 items friends put in it before go back out"));
	CHECK(Waters(s) == 5);

	// Viewing the host's copy shows none, and the stash is set aside, not given.
	s.guest.OpenMenu(s.f, BOX);
	s.Settle();
	CHECK(s.guest.In(BOX, WATER) == 0);
	s.guest.Take(s.f, BOX, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.guest.In(BOX, WATER) == 3);
	CHECK(Waters(s) == 5);
}

TEST("shared storage: put into a chest nobody kept yet, quickload, put again, over and over: the claim gives every put up")
{
	Session s{ Reloads() };
	Storage(s.host, BOX);
	Storage(s.guest, BOX);
	s.host.away.insert(BOX);
	s.Settle();
	const auto before = s.guest.SaveGame(s.f);  // carrying 3 waters
	for (int i = 0; i < 3; ++i) {
		s.guest.Put(s.f, BOX, WATER, 3);
		s.Settle();
		s.guest.LoadGame(s.f, before);  // the put lacking, at a box nobody keeps: nothing to settle yet
		s.Settle();
		CHECK(s.guest.Carried(WATER) == 3);
	}
	CHECK(s.guest.storage.State().Stats().lacking == 0);

	s.host.away.erase(BOX);  // the host walks by: its copy has all nine, then it claims the box
	s.host.Wait(s.f, LootTest::APPLY_INTERVAL);
	s.Settle();
	REQUIRE(s.guest.Keeper(BOX) == Keeper::kFriend);
	CHECK(s.host.In(BOX, WATER) == 0);
	CHECK(s.f.Logged("claimed: 9 items friends put in it before go back out"));
	CHECK(Waters(s) == 5);

	// Taking it all out finds nothing of theirs.
	s.guest.OpenMenu(s.f, BOX);
	s.Settle();
	CHECK(s.guest.In(BOX, WATER) == 0);
	s.guest.Take(s.f, BOX, WATER, 9);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(Waters(s) == 5);

	// Nor does a load from before it all, after the claim.
	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.host.In(BOX, WATER) == 0);
	CHECK(s.guest.storage.State().Stats().lacking == 0);
	CHECK(Waters(s) == 5);
	CHECK(!s.f.Logged("has asked for the world too often"));
}

TEST("shared storage: a put and a take of the same items before the claim, both lost to a load after it: the host's own stay")
{
	Session s;
	Storage(s.host, BOX);
	Storage(s.guest, BOX);
	s.host.away.insert(BOX);
	s.host.holders[BOX][WATER] = 4;  // the host's own, in its copy
	s.Settle();
	const auto before = s.guest.SaveGame(s.f);  // carrying 3 waters
	s.guest.Put(s.f, BOX, WATER, 3);
	s.Settle();
	s.guest.Take(s.f, BOX, WATER, 3);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);

	s.host.away.erase(BOX);
	s.host.Wait(s.f, LootTest::APPLY_INTERVAL);
	s.Settle();
	REQUIRE(s.guest.Keeper(BOX) == Keeper::kFriend);
	CHECK(s.host.In(BOX, WATER) == 4);  // nothing put in, net: nothing given up

	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.host.In(BOX, WATER) == 4);  // neither undone: both before the claim
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(Waters(s) == 9);
}

TEST("shared storage: a take that came back short and was then lost to a reload: the chest has back only what it gave")
{
	Session s;
	const auto before = s.guest.SaveGame(s.f);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.host.holders[CHEST][STIMPAK] = 1;  // a script took 4 in the host's world, unreported
	s.guest.Take(s.f, CHEST, STIMPAK, 4);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 1);
	s.guest.CloseMenu(s.f);
	s.Settle();

	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 0);  // the save's
	CHECK(s.host.In(CHEST, STIMPAK) == 1);  // the server's log keeps the take as it was in the end: 1 back
	CHECK(s.Total(STIMPAK) == 1);
}

TEST("shared storage: a take with its shortfall settled in the log, answered after a load that kept it unanswered")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.host.holders[CHEST][STIMPAK] = 1;
	s.guest.Take(s.f, CHEST, STIMPAK, 4);
	s.guest.CloseMenu(s.f);
	const auto during = s.guest.SaveGame(s.f);  // the take on its way: in the save's record
	REQUIRE(during.unanswered.size() == 1);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 1);

	s.guest.LoadGame(s.f, during);  // carrying the 4 again
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 1);  // the catch-up's take of 1 matched it: the rest taken back
	CHECK(s.guest.storage.State().Stats().corrected == 1);
	CHECK(s.Total(STIMPAK) == 1);
}

TEST("shared storage: a change by nobody the host's chest held less of stays in the log as it was numbered")
{
	Session s;
	const auto before = s.guest.SaveGame(s.f);
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	s.host.holders[CHEST][WATER] = 1;  // a script took 2 in the host's world, unreported
	s.guest.LoadGame(s.f, before);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.f.Logged("held 2 fewer of"));
	CHECK(!s.f.Logged("that held less (change"));  // nobody's: nobody to give back
	// A later catch-up (anyone's) carries the take by nobody as it was numbered: 3.
	REQUIRE(s.f.Join(OTHER_PEER, Fixture::MakeHello("Other", OTHER_ID)));
	const auto states = Decoded(Received(s.f, OTHER_PEER), MT::kWorldState, [](auto a_data) { return Protocol::DecodeWorldState(a_data); });
	std::vector<Protocol::IndexedContainerChange> nobody;
	for (const auto& state : states) {
		for (const auto& change : state.containerChanges) {
			if (change.playerId == 0) {
				nobody.push_back(change);
			}
		}
	}
	REQUIRE(nobody.size() == 1);
	CHECK((nobody[0].change == Protocol::ContainerChange{ CHEST, WATER, -3 }));
}

// ---- what the server accepts in a settle: a modified game can't empty a chest -----------------------------------

TEST("shared storage: a settle undoes only the sender's own moves, once each, and only after the world state it answers")
{
	Fixture f{ Options() };
	f.fake->SetLocal(HOST_PEER);
	const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
	REQUIRE(host);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	REQUIRE(f.Join(OTHER_PEER, Fixture::MakeHello("Other", OTHER_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	SettleRaw(f, FRIEND_PEER);
	SettleRaw(f, OTHER_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 3 }, MT::kReportContainer));     // #0
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, -1 }, MT::kReportContainer));  // #1
	Sync(f, FRIEND_PEER);
	f.fake->Deliver(OTHER_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(OTHER_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 2 }, MT::kReportContainer));  // #2
	Sync(f, OTHER_PEER);
	Received(f, HOST_PEER);
	Received(f, FRIEND_PEER);
	Received(f, OTHER_PEER);

	// Not asked for a world state: a settle is ignored.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 1, 3, 0, { 0, 1, 2 }, {} }));
	CHECK(Count(Received(f, HOST_PEER), MT::kContainerChanged) == 0);

	// After a world request: a settle for an older one, then one naming everything, the others' and past the log too.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 3 }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 1, 3, 0, { 0, 1, 2 }, {} }));
	CHECK(Count(Received(f, HOST_PEER), MT::kContainerChanged) == 0);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 100, 0, { 0, 1, 2, 50 }, {} }));
	auto undone = Decoded(Received(f, HOST_PEER), MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	REQUIRE(undone.size() == 2);
	CHECK(undone[0].playerId == 0);
	CHECK((undone[0].change == Protocol::ContainerChange{ CHEST, STIMPAK, 1 }));  // what goes into the true copy first
	CHECK((undone[1].change == Protocol::ContainerChange{ CHEST, WATER, -3 }));   // the other friend's 2 stay
	CHECK(f.Logged("storage undid 2 and did again 0 of player"));

	// Settled: the same again changes nothing, nor does it after another request.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 100, 0, { 0, 1, 2 }, {} }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 5 }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 3, 100, 0, { 0, 1, 2 }, {} }));
	CHECK(Count(Received(f, HOST_PEER), MT::kContainerChanged) == 0);

	// What the chest holds: the host's 5 stimpaks and the other friend's 2 waters.
	f.fake->Deliver(OTHER_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	const auto shown = Decoded(Received(f, OTHER_PEER), MT::kStorageContents, [](auto a_data) { return Protocol::DecodeStorageListing(a_data); });
	REQUIRE(!shown.empty());
	CHECK((shown.back().stacks == std::vector<Protocol::StorageStack>{ { STIMPAK, 5 }, { WATER, 2 } }));
}

TEST("shared storage: a late move goes through only at the container the player viewed last, and only in a settle")
{
	Fixture f{ Options() };
	f.fake->SetLocal(HOST_PEER);
	const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
	REQUIRE(host);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ BOX, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	// Never viewed anything: a late move goes nowhere.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 1, 0, 0, {}, { { CHEST, STIMPAK, -5 } } }));
	auto got = Received(f, FRIEND_PEER);
	CHECK(Count(got, MT::kContainerChanged) == 0);
	auto refused = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].reason == Protocol::StorageReason::kNotViewing);

	// Viewed the chest, then asked for the world: the late take there goes through, the one at the box doesn't.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 0 }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 0, 0, {}, { { BOX, STIMPAK, -5 }, { CHEST, STIMPAK, -2 } } }));
	got = Received(f, FRIEND_PEER);
	const auto changes = Decoded(got, MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	REQUIRE(changes.size() == 1);
	CHECK((changes[0].change == Protocol::ContainerChange{ CHEST, STIMPAK, -2 }));
	refused = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refused.size() == 2);
	CHECK(refused[0].item == 0);  // the request closed the view
	CHECK((refused[1].container == BOX && refused[1].reason == Protocol::StorageReason::kNotViewing));

	// Settled: a late move outside a settle is an ordinary move, refused without a view.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 0, 0, {}, { { CHEST, STIMPAK, -2 } } }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, STIMPAK, -2 }, MT::kReportContainer));
	got = Received(f, FRIEND_PEER);
	CHECK(Count(got, MT::kContainerChanged) == 0);
	refused = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].reason == Protocol::StorageReason::kNotViewing);
}

TEST("shared storage: undoing and doing again one's own moves counts against one's share of the log, and stops there")
{
	auto options = Options();
	options.maxContainerChangesPerPlayer = 12;
	options.worldStateRefill = std::chrono::milliseconds{ 0 };
	Fixture f{ options };
	f.fake->SetLocal(HOST_PEER);
	const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
	REQUIRE(host);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	SettleRaw(f, FRIEND_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 1 }, MT::kReportContainer));  // #0 to #3
	}
	Pace(FRIEND_PEER);
	Sync(f, FRIEND_PEER);
	const auto numbered = [&] {
		Pace(HOST_PEER);
		return Count(Received(f, HOST_PEER), MT::kContainerChanged);
	};
	CHECK(numbered() == 4);

	// A modified game flips them between two loaded worlds: lacking all, then none, then all again.
	std::uint32_t asked = 1;
	const auto    settle = [&](std::vector<std::uint32_t> a_lacking) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 0 }));
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ ++asked, 4, 0, std::move(a_lacking), {} }));
		Pace(FRIEND_PEER);
		return Received(f, FRIEND_PEER);
	};
	settle({ 0, 1, 2, 3 });
	CHECK(numbered() == 4);  // undone: their share is 8 of 12 now
	settle({});
	CHECK(numbered() == 4);  // done again: 12 of 12
	for (int i = 0; i < 5; ++i) {
		const auto answer = settle({ 0, 1, 2, 3 });
		CHECK(numbered() == 0);  // nothing more of theirs goes in the log
		// Their game is told its settle was cut, once for each: no container, all of storage others keep.
		const auto cut = Decoded(answer, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
		REQUIRE(cut.size() == 1);
		CHECK(cut[0].container == 0);
		CHECK(cut[0].item == 0);
		CHECK(cut[0].reason == Protocol::StorageReason::kClosed);
	}
	CHECK(f.Logged("can't all be put right"));
	CHECK(f.Logged("their share"));

	// Their moves aren't all put right, so storage others keep stays closed to them.
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	Pace(FRIEND_PEER);
	const auto got = Received(f, FRIEND_PEER);
	CHECK(Count(got, MT::kStorageContents) == 0);
	const auto refusals = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refusals.size() == 1);
	CHECK(refusals[0].reason == Protocol::StorageReason::kClosed);
}

TEST("shared storage: a settle logs a shortfall only when the chest held less than the undone move called for, a few lines a settle, and never for a move already undone")
{
	Fixture f{ Reloads() };
	f.fake->SetLocal(HOST_PEER);
	const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
	REQUIRE(host);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	REQUIRE(f.Join(OTHER_PEER, Fixture::MakeHello("Other", OTHER_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	SettleRaw(f, FRIEND_PEER);
	SettleRaw(f, OTHER_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	for (int i = 0; i < 10; ++i) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 1 }, MT::kReportContainer));  // #0 to #9
	}
	Pace(FRIEND_PEER);
	Sync(f, FRIEND_PEER);
	f.fake->Deliver(OTHER_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(OTHER_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, -10 }, MT::kReportContainer));  // #10: all of it
	Pace(OTHER_PEER);
	Sync(f, OTHER_PEER);

	const auto settle = [&](std::uint32_t a_asked, std::vector<std::uint32_t> a_lacking) {
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 0 }));
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ a_asked, 11, 0, std::move(a_lacking), {} }));
		Pace(FRIEND_PEER);
		Received(f, FRIEND_PEER);
	};
	// Their world lacks all ten puts, which the other friend took out: the chest holds none to give back.
	settle(2, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
	CHECK(f.LoggedTimes("than undoing player") == 8);  // the first few get a line each
	CHECK(f.LoggedTimes("and 2 more of player") == 1);
	CHECK(f.Logged("only what it held went"));

	// Asked about the same again: every move is undone already, and the chest does nothing for it: no shortfall.
	settle(3, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
	CHECK(f.LoggedTimes("than undoing player") == 8);
	CHECK(f.LoggedTimes("and 2 more of player") == 1);
}

TEST("shared storage: a settle that doesn't all fit in one's share makes none of its changes: no take put back without its put taken out")
{
	auto options = Options();
	options.maxContainerChangesPerPlayer = 3;
	options.worldStateRefill = std::chrono::milliseconds{ 0 };
	Fixture f{ options };
	f.fake->SetLocal(HOST_PEER);
	const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
	REQUIRE(host);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	SettleRaw(f, FRIEND_PEER);
	std::int64_t hostChest = 0;  // the waters in the host's chest
	const auto   apply = [&] {
		Pace(HOST_PEER);
		for (const auto& change : Decoded(Received(f, HOST_PEER), MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); })) {
			hostChest += change.change.count;
		}
	};
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 50 }, MT::kReportContainer));   // #0
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, -50 }, MT::kReportContainer));  // #1
	Pace(FRIEND_PEER);
	Sync(f, FRIEND_PEER);
	apply();
	REQUIRE(hostChest == 0);

	// They load a save from before both: their world carries the 50 again. Two changes to put it right, one left in their share.
	const std::int64_t friendCarries = 50;
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ host->sessionId, 0 }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 2, 0, { 0, 1 }, {} }));
	Pace(FRIEND_PEER);
	Received(f, FRIEND_PEER);
	apply();
	CHECK(hostChest == 0);  // not the take's 50 put back alone
	CHECK(hostChest + friendCarries == 50);
	CHECK(f.Logged("can't all be put right (2 changes"));
}

TEST("shared storage: what the true copy undid outlasts a restart: none is undone twice, and a later save's world has it done again")
{
	TempSessionFile file;
	auto            options = Options();
	options.sessionFile = file.path.string();
	std::uint64_t   session = 0;
	{
		Fixture f{ options };
		f.fake->SetLocal(HOST_PEER);
		const auto host = f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID));
		REQUIRE(host);
		session = host->sessionId;
		REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
		f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 0, { { STIMPAK, 5 } } }, MT::kReportStorage));
		Sync(f, HOST_PEER);
		SettleRaw(f, FRIEND_PEER);
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageView{ CHEST, true }));
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 3 }, MT::kReportContainer));  // #0
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ session, 0 }));
		f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 1, 0, { 0 }, {} }));  // #1: the put undone
		Sync(f, FRIEND_PEER);
		REQUIRE(f.Logged("storage undid 1 and did again 0 of player"));
	}  // stopping writes the session file
	const auto saved = SessionFile::Load(file.path);
	REQUIRE(saved);
	REQUIRE(saved->storageReversals.size() == 1);

	Fixture f{ options };
	f.fake->SetLocal(HOST_PEER);
	REQUIRE(f.Join(HOST_PEER, Fixture::MakeHello("Host", HOST_ID)));
	f.fake->Deliver(HOST_PEER, Protocol::Encode(Protocol::StorageListing{ CHEST, 2, { { STIMPAK, 5 } } }, MT::kReportStorage));
	Sync(f, HOST_PEER);
	REQUIRE(f.Join(FRIEND_PEER, Fixture::MakeHello("Guest", FRIEND_ID)));
	Received(f, HOST_PEER);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 1, 2, 0, { 0 }, {} }));  // lacking it still
	CHECK(Count(Received(f, HOST_PEER), MT::kContainerChanged) == 0);
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::WorldRequest{ session, 2 }));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageSettle{ 2, 2, 0, {}, {} }));  // a world that has it
	const auto redone = Decoded(Received(f, HOST_PEER), MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	REQUIRE(redone.size() == 1);
	CHECK(redone[0].playerId == 0);
	CHECK((redone[0].change == Protocol::ContainerChange{ CHEST, WATER, 3 }));
}

TEST("shared storage: an item a quest alias holds a stack of in the friend's copy stays theirs for the view, all of it")
{
	Session s;
	s.host.holders[CHEST][WATER] = 3;
	s.host.storage.State().Unlist(CHEST);
	s.Settle();
	s.guest.questStacks.insert({ CHEST, WATER });  // their copy's 2 waters fill a quest alias
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	CHECK(s.guest.In(CHEST, WATER) == 2);  // theirs, not set aside; the host's 3 not shown
	CHECK(s.guest.storage.Aside().empty());
	s.guest.Take(s.f, CHEST, WATER, 1);   // from their own stack: kept in their game
	s.guest.Put(s.f, CHEST, WATER, 2);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.guest.In(CHEST, WATER) == 3);  // still theirs after the close
	CHECK(s.guest.loot.Stats().storageKept == 2);
	CHECK(s.Total(WATER) == 8);
}
