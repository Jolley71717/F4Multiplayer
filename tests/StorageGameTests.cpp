// Shared storage between the host's game and friends' games, against a real server (LootGame.h plays each game's loot
// and storage code). The host's game keeps the one true copy of a home's or a settlement's container; a friend's copy
// shows it while their menu is open, and their moves go through only if the server can check them. Every test counts
// the items in the session's worlds: whatever is refused, nothing is duplicated and nothing is lost.

#include "LootGame.h"

#include "SessionFile.h"

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
	constexpr std::uint64_t OTHER_ID = 0x0000'C0C0'0000'0003;
	constexpr PeerId        HOST_PEER = 1;
	constexpr PeerId        FRIEND_PEER = 2;
	constexpr PeerId        OTHER_PEER = 3;
	constexpr std::uint32_t CHEST = 0x0001'2345;  // Home Plate's chest, say
	constexpr std::uint32_t BOX = 0x0001'2346;    // a settlement box the host's game hasn't loaded
	constexpr std::uint32_t STIMPAK = 0x0002'3736;
	constexpr std::uint32_t WATER = 0x0004'835D;
	constexpr std::uint32_t RIFLE = 0x0004'F46A;

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

	// A session: the host's game (its chest holds 5 stimpaks) and a friend's (their copy holds 2 waters of their own).
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
			host.Join(f, "Host");
			guest.Join(f, "Guest");
			SettleAll(f, { &host, &guest });
		}

		void Settle() { SettleAll(f, { &host, &guest }); }

		// The items of a_item in the session's worlds: the host's chest and both players' inventories (a friend's copy of
		// the chest only shows the host's while open; the host's copy of the friend's is the friend's own, not shared).
		std::int64_t Total(std::uint32_t a_item) const
		{
			return host.In(CHEST, a_item) + host.Carried(a_item) + guest.Carried(a_item) + (guest.storage.Viewing() ? 0 : guest.In(CHEST, a_item));
		}
	};
}

TEST("shared storage: the host's game claims its storage, and a friend's copy shows it only while their menu is open")
{
	Session s;
	CHECK(s.host.Keeper(CHEST) == Keeper::kUs);
	CHECK(s.guest.Keeper(CHEST) == Keeper::kFriend);
	CHECK(s.host.storage.Stats().listed == 1);

	s.guest.OpenMenu(s.f, CHEST);
	CHECK(s.guest.Carried(WATER) == 2);  // their copy's own waters come to them: the copy shows the host's now
	CHECK(s.guest.In(CHEST, WATER) == 0);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 5);

	s.guest.CloseMenu(s.f);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // the session's things leave the copy with the menu
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 5);
	CHECK(s.Total(STIMPAK) == 5);
	CHECK(s.Total(WATER) == 2);
}

TEST("shared storage: a friend's take and put go through to the host's chest, and come back to nobody twice")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.guest.Take(s.f, CHEST, STIMPAK, 3);
	s.guest.Put(s.f, CHEST, WATER, 2);
	s.Settle();
	CHECK(s.host.In(CHEST, STIMPAK) == 2);
	CHECK(s.host.In(CHEST, WATER) == 2);
	CHECK(s.guest.Carried(STIMPAK) == 3);
	CHECK(s.guest.Carried(WATER) == 0);
	CHECK(s.guest.In(CHEST, STIMPAK) == 2);
	CHECK(s.guest.In(CHEST, WATER) == 2);
	CHECK(s.guest.storage.InFlight() == 0);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(STIMPAK) == 5);
	CHECK(s.Total(WATER) == 2);

	// The host takes what the friend put in, at its own chest: an ordinary move of the keeper's.
	s.host.Take(s.f, CHEST, WATER, 2);
	s.Settle();
	CHECK(s.host.Carried(WATER) == 2);
	CHECK(s.guest.In(CHEST, WATER) == 0);  // nothing comes into the closed copy
	CHECK(s.Total(WATER) == 2);
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
	CHECK(s.guest.storage.Stats().refusedTakes + other.storage.Stats().refusedTakes == 1);
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
	CHECK(s.Total(WATER) == 2);
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
	CHECK(s.guest.storage.Stats().refusedTakes == 0);
}

TEST("shared storage: when the host leaves, the friend's view closes and their moves there are refused and undone")
{
	Session s;
	s.guest.OpenMenu(s.f, CHEST);
	s.Settle();
	s.f.fake->Drop(HOST_PEER);
	std::vector<std::vector<std::uint8_t>> got;
	CHECK(Await(s.f, FRIEND_PEER, MT::kStorageRefused, got));
	s.guest.Handle(s.f, got);
	CHECK(!s.guest.storage.Viewing());
	CHECK(s.guest.storage.Stats().closedByHost == 1);
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // the host's things left the copy
	CHECK(s.guest.Carried(STIMPAK) == 0);
	CHECK(s.guest.Carried(WATER) == 2);
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
	CHECK(s.guest.Carried(STIMPAK) == 0);  // refused, given back (or the view closed first, and it never went)
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);
	CHECK(!s.guest.storage.Viewing());

	// The host's world has its answer and lists the chest again; the friend opens it again.
	s.Settle();
	CHECK(s.host.storage.Stats().listed == 2);
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
	CHECK(s.host.storage.Stats().shorts == 1);
	CHECK(s.guest.storage.Stats().shortTakes == 1);
	CHECK(s.guest.Carried(STIMPAK) == 1);     // 4 taken, 3 of them never there
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);   // shown as it is: the host's game listed it again
	CHECK(s.host.storage.Stats().listed == 2);
	s.guest.CloseMenu(s.f);
	s.Settle();
	CHECK(s.Total(STIMPAK) == 1);
}

TEST("shared storage: storage the host's game hasn't claimed is each player's own while the rule is on")
{
	Session s;
	Storage(s.guest, BOX);
	s.host.away.insert(BOX);
	Storage(s.host, BOX);
	s.guest.holders[BOX][WATER] = 3;
	s.guest.Take(s.f, BOX, WATER, 3);
	s.Settle();
	CHECK(s.guest.Keeper(BOX) == Keeper::kLocal);
	CHECK(s.guest.loot.Stats().storageKept == 1);
	CHECK(s.host.In(BOX, WATER) == 0);
	CHECK(s.host.Keeper(BOX) == Keeper::kNone);  // the host's own: claimed once its game has it loaded
	CHECK(s.host.storage.OwnerOf(BOX) == std::nullopt);
}

TEST("shared storage: off (the default), every game keeps its own copy and moves go as before")
{
	Session s{ false };
	CHECK(s.host.Keeper(CHEST) == Keeper::kNone);
	CHECK(s.guest.Keeper(CHEST) == Keeper::kNone);
	CHECK(s.host.storage.Stats().listed == 0);
	s.guest.OpenMenu(s.f, CHEST);
	CHECK(s.guest.Carried(WATER) == 0);
	s.guest.Take(s.f, CHEST, WATER, 1);
	s.host.Take(s.f, CHEST, STIMPAK, 1);
	s.Settle();
	CHECK(s.guest.In(CHEST, STIMPAK) == 0);  // removeitem takes what is there: their copy had none
	CHECK(s.guest.loot.Stats().storageKept == 0);
	CHECK(s.guest.storage.Stats().views == 0);
}

TEST("shared storage: only the host's game claims storage: a friend's listing is nobody's truth")
{
	Session s;
	Storage(s.guest, BOX);
	s.f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::StorageListing{ BOX, 0, { { WATER, 9 } } }, MT::kReportStorage));
	s.Settle();
	CHECK(s.guest.storage.OwnerOf(BOX) == std::nullopt);
	CHECK(s.host.storage.OwnerOf(BOX) == std::nullopt);
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
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, MOD_ITEM, 1 }, MT::kReportContainer));
	f.fake->Deliver(FRIEND_PEER, Protocol::Encode(Protocol::ContainerChange{ CHEST, WATER, 1 }, MT::kReportContainer));
	const auto got = Received(f, FRIEND_PEER);
	const auto refused = Decoded(got, MT::kStorageRefused, [](auto a_data) { return Protocol::DecodeStorageRefusal(a_data); });
	REQUIRE(refused.size() == 1);
	CHECK((refused[0] == Protocol::StorageRefusal{ CHEST, MOD_ITEM, 1, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kCantHold }));
	const auto changes = Decoded(got, MT::kContainerChanged, [](auto a_data) { return Protocol::DecodeIndexedContainerChange(a_data); });
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].change.item == WATER);
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
