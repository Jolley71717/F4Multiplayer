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

	// A session: the host's game (its chest holds 5 stimpaks) and a friend's (their copy holds 2 waters of their own, and
	// they carry 3 more).
	struct Session
	{
		Fixture f;
		Game    host{ HOST_PEER, HOST_ID };
		Game    guest{ FRIEND_PEER, FRIEND_ID };

		explicit Session(bool a_shared = true) :
			f(Options(a_shared))
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
			return host.In(CHEST, a_item) + host.Carried(a_item) + guest.Carried(a_item) + own;
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

TEST("shared storage: a friend's reload after a put: the put stands, and the loaded inventory loses it again")
{
	Session s;
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
	CHECK(s.guest.Carried(WATER) == 0);  // the catch-up brought the put, done again on the loaded player
	CHECK(s.guest.storage.State().Stats().redone == 1);
	CHECK(s.host.In(CHEST, WATER) == 3);
	CHECK(s.Total(WATER) == 5);

	// The loop that duped: put, quickload, take again. The chest holds 3, and only those come out.
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, WATER, 3);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.host.In(CHEST, WATER) == 0);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a friend's reload after a take: the take stands, and the loaded inventory gets it again")
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
	CHECK(s.guest.Carried(STIMPAK) == 0);
	s.Settle();
	CHECK(s.guest.Carried(STIMPAK) == 2);  // never lost: the chest gave them up
	CHECK(s.host.In(CHEST, STIMPAK) == 3);
	CHECK(s.Total(STIMPAK) == 5);
}

TEST("shared storage: a save made with a put unanswered, loaded again: the put is in it, and isn't done twice")
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
	CHECK(s.guest.storage.State().Stats().redone == 0);
	CHECK(s.guest.storage.State().InFlight() == 0);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a put cut off by a disconnect is given back, and done again if the server had it")
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
	CHECK(s.guest.Carried(WATER) == 3);  // both given back: no answer will come
	CHECK(s.guest.In(CHEST, WATER) == 2);
	CHECK(s.guest.storage.State().Orphans() == 2);
	s.host.Receive(s.f);
	CHECK(s.host.In(CHEST, WATER) == 1);  // the first got through

	s.guest.Join(s.f, "Guest");
	s.Settle();
	CHECK(s.guest.Carried(WATER) == 2);  // the catch-up numbered the first: done again
	CHECK(s.guest.storage.State().Stats().redone == 1);
	CHECK(s.Total(WATER) == 5);
}

TEST("shared storage: a put with no answer is given back after a while")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Put(s.f, CHEST, WATER, 3, false);
	s.guest.FlushLoot();
	s.guest.outbox.clear();  // lost on its way
	CHECK(s.guest.storage.State().InFlight() == 1);
	s.guest.Wait(s.f, StorageView::ANSWER_WITHIN);
	CHECK(s.guest.Carried(WATER) == 3);
	CHECK(s.guest.In(CHEST, WATER) == 0);
	CHECK(s.guest.Noticed(Notice::kPutRefused) == 1);
	s.guest.CloseMenu(s.f);
	s.Settle();
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
