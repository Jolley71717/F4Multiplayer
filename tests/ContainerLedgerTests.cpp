// Which of the session's numbered container changes a player's world has (ContainerLedger), alone and
// between two or three players' games and a real server (LootGame.h).

#include "LootGame.h"

#include <filesystem>
#include <format>
#include <random>
#include <string_view>

using namespace std::chrono_literals;
using ContainerLedger::Ledger;
using LootTest::Clock;
using LootTest::Game;
using TestServer::Fixture;
using TestServer::MT;

namespace
{
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t SAFE = 0x0002C002;
	constexpr std::uint32_t STIMPAK = 0x00023736;
	constexpr std::uint32_t AMMO = 0x0001F278;
	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t RAIDER = 0x0001F001;
	constexpr std::uint32_t SETTLER = 0x0001F002;
	constexpr std::uint32_t FRIEND = 7;

	Protocol::IndexedContainerChange Change(std::uint32_t a_index, std::uint32_t a_player = FRIEND, std::int32_t a_count = -1)
	{
		return { a_index, a_player, { CHEST, STIMPAK, a_count } };
	}

	std::vector<std::uint32_t> Indices(const std::vector<ContainerLedger::Ready>& a_ready)
	{
		std::vector<std::uint32_t> out;
		for (const auto& ready : a_ready) {
			out.push_back(ready.change.index);
		}
		return out;
	}

	// A WorldState's container changes, from a_first to a_end (exclusive).
	std::vector<Protocol::IndexedContainerChange> Range(std::uint32_t a_first, std::uint32_t a_end)
	{
		std::vector<Protocol::IndexedContainerChange> out;
		for (auto i = a_first; i < a_end; ++i) {
			out.push_back(Change(i));
		}
		return out;
	}

	// Collects what a Receiver hands to the world.
	struct Applied
	{
		std::vector<std::uint32_t> indices;
		ContainerLedger::Receiver::Apply Fn()
		{
			return [this](const Protocol::IndexedContainerChange& a_change) { indices.push_back(a_change.index); };
		}
	};

	std::vector<std::uint32_t> Iota(std::uint32_t a_first, std::uint32_t a_end)
	{
		std::vector<std::uint32_t> out;
		for (auto i = a_first; i < a_end; ++i) {
			out.push_back(i);
		}
		return out;
	}
}

// ---- the numbering alone -------------------------------------------------------------------------

TEST("container ledger: a friend's loot made while our save loads goes in after the changes before it, once")
{
	// Saved at #100; a friend looted #100..#150 (we had them, then reloaded); #151 came live during the load.
	const Clock::time_point t0{};
	ContainerLedger::Receiver receiver;
	receiver.OnWelcome(1, 1);
	receiver.Load(1, 100);
	(void)receiver.Request(t0);
	Applied applied;
	receiver.Live(Change(151), t0, applied.Fn());
	CHECK(applied.indices.empty());
	CHECK(receiver.Numbers().Waiting() == 1);

	CHECK(receiver.CatchUp(100, Range(100, 152), t0, applied.Fn()) == 0);
	CHECK(applied.indices == Iota(100, 152));
	CHECK(receiver.Numbers().Next() == 152);
	CHECK(receiver.Numbers().Waiting() == 0);
	CHECK(receiver.Numbers().Stats().duplicates == 1);  // the reply's #151
}

TEST("container ledger: our own change echoed before the catch-up is skipped once, and isn't counted as made before the save")
{
	const Clock::time_point t0{};
	ContainerLedger::Receiver receiver;
	receiver.OnWelcome(1, 1);
	receiver.Load(1, 151);
	(void)receiver.Request(t0);
	Applied applied;
	receiver.Live(Change(151, 1), t0, applied.Fn());  // our own, made after the load
	CHECK(receiver.Numbers().Next() == 152);
	CHECK(receiver.CatchUp(151, { Change(151, 1) }, t0, applied.Fn()) == 0);
	CHECK(applied.indices.empty());
	CHECK(receiver.Numbers().Next() == 152);
	CHECK(receiver.OwnSkipped() == 0);
}

TEST("container ledger: a change below the next one, or one already waiting, is dropped")
{
	Ledger ledger;
	ledger.Restart(10);
	const Clock::time_point t0{};
	CHECK(ledger.Offer(Change(9), false, t0).empty());
	CHECK(ledger.Offer(Change(12), false, t0).empty());
	CHECK(ledger.Offer(Change(12), true, t0).empty());
	CHECK(ledger.Stats().duplicates == 2);
	CHECK(Indices(ledger.Offer(Change(10), false, t0)) == std::vector<std::uint32_t>{ 10 });
	CHECK(Indices(ledger.Offer(Change(11), false, t0)) == (std::vector<std::uint32_t>{ 11, 12 }));
	CHECK(Indices(ledger.Offer(Change(10), false, t0)).empty());
	CHECK(ledger.Next() == 13);
}

TEST("container ledger: a second gap over the first fills in order as the missing ones come")
{
	Ledger ledger;
	const Clock::time_point t0{};
	CHECK(ledger.Offer(Change(2), false, t0).empty());
	CHECK(ledger.Offer(Change(5), false, t0).empty());
	CHECK(ledger.Offer(Change(4), false, t0).empty());
	CHECK(Indices(ledger.Offer(Change(0), false, t0)) == std::vector<std::uint32_t>{ 0 });
	CHECK(Indices(ledger.Offer(Change(1), false, t0)) == (std::vector<std::uint32_t>{ 1, 2 }));
	CHECK(Indices(ledger.Offer(Change(3), false, t0)) == (std::vector<std::uint32_t>{ 3, 4, 5 }));
	CHECK(ledger.Waiting() == 0);
	CHECK(ledger.Stats().gaps == 1);  // one stretch of waiting, however many holes it had
}

TEST("container ledger: past the bound the newest waiting change is let go, and the next one we lack never moves")
{
	Ledger ledger;
	const Clock::time_point t0{};
	for (std::uint32_t i = 1; i <= Ledger::MAX_AHEAD; ++i) {
		CHECK(ledger.Offer(Change(i * 2), false, t0).empty());
	}
	CHECK(ledger.Waiting() == Ledger::MAX_AHEAD);
	CHECK(ledger.Offer(Change(Ledger::MAX_AHEAD * 2 + 2), false, t0).empty());  // newer than all: let go
	CHECK(ledger.Offer(Change(3), false, t0).empty());                          // older: the newest makes room
	CHECK(ledger.Waiting() == Ledger::MAX_AHEAD);
	CHECK(ledger.Stats().droppedAhead == 2);
	CHECK(ledger.Next() == 0);
	CHECK(Indices(ledger.Offer(Change(0), false, t0)) == std::vector<std::uint32_t>{ 0 });
	CHECK(Indices(ledger.Offer(Change(1), false, t0)) == (std::vector<std::uint32_t>{ 1, 2, 3, 4 }));
}

TEST("container ledger: a gap is asked for again only after 15 s without a change, then 30, then every 60")
{
	Ledger ledger;
	const Clock::time_point t0{};
	ledger.Offer(Change(3), false, t0);
	CHECK(!ledger.Due(t0 + 14s));
	CHECK(ledger.Due(t0 + 15s));
	CHECK(!ledger.Due(t0 + 44s));
	CHECK(ledger.Due(t0 + 45s));
	CHECK(!ledger.Due(t0 + 104s));
	CHECK(ledger.Due(t0 + 105s));
	CHECK(!ledger.Due(t0 + 164s));
	CHECK(ledger.Due(t0 + 165s));
	CHECK(ledger.Stats().asked == 4);
	CHECK(ledger.Next() == 0);  // asking never skips anything

	// A chunk arriving is progress: the clock starts again.
	ledger.OnChunk(0, 0, t0 + 200s);
	CHECK(!ledger.Due(t0 + 259s));
	CHECK(ledger.Due(t0 + 260s));

	// Filled: nothing to ask for, and the next gap starts at 15 s again.
	CHECK(Indices(ledger.Offer(Change(0), false, t0 + 300s)) == std::vector<std::uint32_t>{ 0 });
	ledger.Offer(Change(1), false, t0 + 300s);
	ledger.Offer(Change(2), false, t0 + 300s);
	CHECK(ledger.Waiting() == 0);
	CHECK(!ledger.Due(t0 + 1000s));
	ledger.Offer(Change(9), false, t0 + 1000s);
	CHECK(ledger.Due(t0 + 1015s));
}

TEST("container ledger: a server log shorter than our world's counts again from its end, only when it answers our request")
{
	const Clock::time_point t0{};
	Ledger ledger;
	ledger.Restart(165);
	// Nobody asked: a WorldState starting lower moves nothing back.
	CHECK(ledger.OnChunk(120, 0, t0) == 0);
	CHECK(ledger.Next() == 165);

	// Asked from 165, answered from 120 with nothing: the host's game lost 45.
	ledger.OnRequest(165, t0);
	ledger.Offer(Change(170), false, t0);
	CHECK(ledger.OnChunk(120, 0, t0) == 45);
	CHECK(ledger.Next() == 120);
	CHECK(ledger.Waiting() == 0);
	CHECK(ledger.Stats().rewinds == 1);
	CHECK(Indices(ledger.Offer(Change(120), false, t0)) == std::vector<std::uint32_t>{ 120 });

	// Answered where we asked: nothing to do. And the answer is used up: a second chunk can't rewind.
	ledger.OnRequest(121, t0);
	CHECK(ledger.OnChunk(121, 0, t0) == 0);
	CHECK(ledger.OnChunk(100, 0, t0) == 0);
	CHECK(ledger.Next() == 121);

	// A reply that starts lower but has changes is another session's full list, not a short log.
	ledger.OnRequest(121, t0);
	CHECK(ledger.OnChunk(0, 5, t0) == 0);
	CHECK(ledger.Next() == 121);
}

TEST("container ledger: a different session starts our world from nothing; the same one keeps its count")
{
	ContainerLedger::Receiver receiver;
	receiver.Load(5, 40);
	CHECK(!receiver.OnWelcome(5, 2));
	CHECK(receiver.Point().containerFrom == 40);
	CHECK(receiver.OnWelcome(6, 3));
	CHECK(receiver.Point().containerFrom == 0);
	CHECK(receiver.Point().sessionId == 6);
	CHECK(!receiver.Ours(2));
	CHECK(receiver.Ours(3));
}

// ---- two and three games, a real server -------------------------------------------------------------

namespace
{
	using LootTest::IsType;

	// A session file of this run's own: test runs (several at once on one machine) don't share one.
	std::string SessionPath(std::string_view a_name)
	{
		return (std::filesystem::temp_directory_path() / std::format("f4mp-{}-{:08x}.session", a_name, std::random_device{}())).string();
	}

	// The container changes in a_packets' WorldStates, in order.
	std::vector<Protocol::IndexedContainerChange> CaughtUp(const LootTest::Packets& a_packets)
	{
		std::vector<Protocol::IndexedContainerChange> out;
		for (const auto& packet : a_packets) {
			if (IsType(packet, MT::kWorldState)) {
				if (const auto msg = Protocol::DecodeWorldState(packet)) {
					out.insert(out.end(), msg->containerChanges.begin(), msg->containerChanges.end());
				}
			}
		}
		return out;
	}

	// a_packets' container changes as the server relays them live (kContainerChanged), at a_index first.
	auto FindLive(LootTest::Packets& a_packets, std::uint32_t a_index)
	{
		return std::ranges::find_if(a_packets, [&](const std::vector<std::uint8_t>& a_packet) {
			const auto msg = IsType(a_packet, MT::kContainerChanged) ? Protocol::DecodeIndexedContainerChange(a_packet) : std::nullopt;
			return msg && msg->index == a_index;
		});
	}
}

TEST("loot sync: two players looting one chest in turns, with a change lost on the way and another arriving twice, end with the same chest and no copies")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	for (auto* game : { &alice, &bob }) {
		game->holders[CHEST] = { { STIMPAK, 5 }, { AMMO, 100 } };
	}

	// Interleaved: each takes or puts in their own game, which reports it; the server numbers them.
	alice.Take(f, CHEST, STIMPAK, 2);  // #0
	bob.Take(f, CHEST, AMMO, 30);      // #1
	alice.Receive(f);
	auto toBob = bob.Fetch(f);
	alice.Put(f, CHEST, STIMPAK, 1);   // #2
	alice.Take(f, CHEST, AMMO, 20);    // #3
	bob.Take(f, CHEST, STIMPAK, 1);    // #4
	alice.Receive(f);
	auto more = bob.Fetch(f);

	// Bob's copy of #2 is lost, and #3 comes twice.
	const auto lost = FindLive(more, 2);
	REQUIRE(lost != more.end());
	more.erase(lost);
	const auto twice = FindLive(more, 3);
	REQUIRE(twice != more.end());
	more.push_back(*twice);
	std::ranges::move(more, std::back_inserter(toBob));
	bob.Handle(f, toBob);

	// #3 and #4 wait for #2; nothing of Alice's after the gap is in Bob's chest yet.
	CHECK(bob.loot.Numbers().Numbers().Next() == 2);
	CHECK(bob.loot.Numbers().Numbers().Waiting() == 2);
	CHECK(bob.In(CHEST, AMMO) == 70);

	// Bob's game asks again after a quiet 15 s; the server sends from #2.
	CHECK(!bob.Wait(f, 14s));
	CHECK(bob.Wait(f, 1s));
	bob.Receive(f);

	CHECK(bob.loot.Numbers().Numbers().Next() == 5);
	CHECK(bob.loot.Numbers().Numbers().Waiting() == 0);
	CHECK(alice.loot.Numbers().Numbers().Next() == 5);
	CHECK(alice.holders[CHEST] == bob.holders[CHEST]);
	CHECK(alice.In(CHEST, STIMPAK) == 3);
	CHECK(alice.In(CHEST, AMMO) == 50);
	// Nothing made or lost: what the chest holds and the two carry adds up to what it held.
	CHECK(alice.In(CHEST, STIMPAK) + alice.Carried(STIMPAK) + bob.Carried(STIMPAK) == 5);
	CHECK(alice.In(CHEST, AMMO) + alice.Carried(AMMO) + bob.Carried(AMMO) == 100);
	CHECK(alice.loot.Numbers().OwnSkipped() + bob.loot.Numbers().OwnSkipped() == 0);
}

TEST("loot sync: a friend's loot during our reload goes into our world after what they took before it, so the chest isn't full again")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	for (auto* game : { &alice, &bob }) {
		game->holders[CHEST] = { { STIMPAK, 10 } };
		game->holders[SAFE] = { { CAPS, 300 } };
	}
	alice.Take(f, CHEST, STIMPAK, 1);  // #0
	bob.Receive(f);
	const auto save = bob.SaveGame(f);  // Bob saves: #0 is in it
	CHECK(save.point.containerFrom == 1);

	alice.Take(f, CHEST, STIMPAK, 4);  // #1..#3, while Bob plays on
	alice.Take(f, CHEST, STIMPAK, 2);
	alice.Take(f, SAFE, CAPS, 100);
	bob.Receive(f);
	CHECK(bob.In(CHEST, STIMPAK) == 3);

	// Bob dies and reloads. During his loading screen Alice loots again (#4): the server sends it to
	// him before it reads his request, so it waits in his inbox ahead of the catch-up.
	alice.Take(f, SAFE, CAPS, 50);
	LootTest::Sync(f, alice.peer);
	bob.LoadGame(f, save);
	bob.Receive(f);
	alice.Receive(f);

	CHECK(bob.loot.Numbers().Numbers().Next() == 5);
	CHECK(bob.loot.Numbers().Numbers().Waiting() == 0);
	CHECK(bob.In(CHEST, STIMPAK) == 3);
	CHECK(bob.In(SAFE, CAPS) == 150);
	CHECK(bob.holders == alice.holders);
}

TEST("loot sync: after the host's game stops without writing its last changes, a friend's next loot still reaches the others")
{
	const auto path = SessionPath("ledger");
	const auto kept = path + ".15s-ago";
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	Game host{ 1, 0xA11CE }, joiner{ 2, 0xB0B };
	for (auto* game : { &host, &joiner }) {
		game->holders[CHEST] = { { AMMO, 100 } };
	}
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");
		host.Take(f, CHEST, AMMO, 10);  // #0, #1
		host.Take(f, CHEST, AMMO, 10);
		host.Receive(f);
		joiner.Receive(f);
	}  // stopping writes the session file
	std::filesystem::copy_file(path, kept, std::filesystem::copy_options::overwrite_existing);
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");
		host.Receive(f);
		joiner.Receive(f);
		host.Take(f, CHEST, AMMO, 5);  // #2..#4
		host.Take(f, CHEST, AMMO, 5);
		host.Take(f, CHEST, AMMO, 5);
		joiner.Receive(f);
		CHECK(joiner.loot.Numbers().Numbers().Next() == 5);
	}
	// The host's game froze and was ended: the file it last wrote has only #0 and #1.
	std::filesystem::copy_file(kept, path, std::filesystem::copy_options::overwrite_existing);
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");  // asks from #5; the server has 2
		host.Receive(f);
		joiner.Receive(f);
		CHECK(joiner.loot.Numbers().Numbers().Stats().rewinds == 1);
		CHECK(joiner.loot.Numbers().Numbers().Next() == 2);

		// The next loot by either side reaches the other, though it takes a number the joiner had seen.
		host.Take(f, CHEST, AMMO, 7);    // #2 again
		joiner.Take(f, CHEST, AMMO, 3);  // #3
		host.Receive(f);
		joiner.Receive(f);
		CHECK(joiner.loot.Numbers().Numbers().Next() == 4);
		CHECK(host.loot.Numbers().Numbers().Next() == 4);
		CHECK(joiner.In(CHEST, AMMO) == 55);
		CHECK(host.In(CHEST, AMMO) == 55);
	}
	std::filesystem::remove(path);
	std::filesystem::remove(kept);
}

TEST("loot sync: a change the host's game lost, still waiting for its chest in a friend's game, never lands there once the host is back")
{
	// The joiner's safe isn't loaded when the host takes caps from it. The host's game then freezes before
	// writing that to the session file, and starts again from its save: in the session the caps were never
	// taken, and the number goes to the host's next loot. The joiner's safe must end as the host's.
	const auto path = SessionPath("lost");
	const auto kept = path + ".15s-ago";
	std::filesystem::remove(path);
	Server::Options options;
	options.sessionFile = path;
	Game host{ 1, 0xA11CE }, joiner{ 2, 0xB0B };
	for (auto* game : { &host, &joiner }) {
		game->holders[CHEST] = { { AMMO, 100 } };
		game->holders[SAFE] = { { CAPS, 300 } };
	}
	joiner.away.insert(SAFE);
	Game::Save hostSave;
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");
		host.Take(f, CHEST, AMMO, 10);  // #0, #1
		host.Take(f, CHEST, AMMO, 10);
		host.Receive(f);
		joiner.Receive(f);
		hostSave = host.SaveGame(f);
	}
	std::filesystem::copy_file(path, kept, std::filesystem::copy_options::overwrite_existing);
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");
		host.Take(f, SAFE, CAPS, 50);  // #2
		joiner.Receive(f);
		CHECK(joiner.loot.Waiting().size() == 1);
		CHECK(joiner.In(SAFE, CAPS) == 300);
	}
	std::filesystem::copy_file(kept, path, std::filesystem::copy_options::overwrite_existing);
	host.Restart(hostSave);
	{
		Fixture f{ options };
		host.Join(f, "Host");
		joiner.Join(f, "Joiner");  // asks from #3; the server has 2
		host.Receive(f);
		joiner.Receive(f);
		CHECK(joiner.loot.Numbers().Numbers().Stats().rewinds == 1);
		CHECK(joiner.loot.Waiting().empty());

		host.Take(f, CHEST, AMMO, 5);  // #2 again: another change
		joiner.away.clear();           // the joiner walks to the safe
		joiner.Receive(f);
		CHECK(joiner.In(SAFE, CAPS) == 300);
		CHECK(host.In(SAFE, CAPS) == 300);
		CHECK(joiner.In(CHEST, AMMO) == 75);
		CHECK(joiner.holders == host.holders);

		// The joiner's save made now carries no trace of the lost change either.
		CHECK(joiner.SaveGame(f).waiting.empty());
	}
	std::filesystem::remove(path);
	std::filesystem::remove(kept);
}

TEST("loot sync: a save with no session's record, loaded in a session, catches up once and its next save carries the record")
{
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	for (auto* game : { &alice, &bob }) {
		game->holders[CHEST] = { { STIMPAK, 10 } };
	}
	alice.Take(f, CHEST, STIMPAK, 3);
	alice.Take(f, CHEST, STIMPAK, 3);
	bob.Receive(f);

	// Bob loads a save he made alone: no record.
	Game::Save solo{ { { CHEST, { { STIMPAK, 10 } } } }, {}, {}, {} };
	bob.LoadGame(f, solo);
	bob.Receive(f);
	CHECK(bob.In(CHEST, STIMPAK) == 4);
	const auto resaved = bob.SaveGame(f);
	CHECK(resaved.point.sessionId == bob.sessionId);
	CHECK(resaved.point.containerFrom == 2);

	// Reloading that save asks only for what came after it: Alice's loot isn't taken out a second time,
	// and a later loot of hers is.
	alice.Take(f, CHEST, STIMPAK, 1);
	bob.LoadGame(f, resaved);
	bob.Receive(f);
	CHECK(bob.In(CHEST, STIMPAK) == 3);
	CHECK(bob.loot.Numbers().Numbers().Next() == 3);
}

TEST("loot sync: a friend taking what a reloader put in after their save counts as a duplication risk")
{
	// The loop rc5 closes (replaying our own moves); until then the soak measures it.
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	bob.carried[CAPS] = 100;
	const auto save = bob.SaveGame(f);
	bob.Put(f, SAFE, CAPS, 100);  // #0, after his save
	alice.Receive(f);
	alice.Take(f, SAFE, CAPS, 100);  // #1
	bob.Receive(f);
	CHECK(bob.loot.Numbers().DupRisk() == 0);  // live: the caps really left Bob

	bob.LoadGame(f, save);
	bob.Receive(f);
	CHECK(bob.loot.Numbers().OwnSkipped() == 1);
	CHECK(bob.loot.Numbers().DupRisk() == 1);
	CHECK(alice.loot.Numbers().DupRisk() == 0);
}

TEST("loot sync: a friend's take arriving just as our chest loads waits for the put before it, so the chest doesn't keep what was taken")
{
	// Alice puts stimpaks in a chest Bob's game hasn't loaded; Carol takes them out. Bob walks in, and
	// Carol's take reaches him before his game has tried Alice's put again.
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		game->Join(f, "P");
		game->Receive(f);
	}
	bob.away.insert(CHEST);
	alice.carried[STIMPAK] = 2;
	alice.Put(f, CHEST, STIMPAK, 2);  // #0
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 1);
	carol.Receive(f);
	carol.Take(f, CHEST, STIMPAK, 2);  // #1
	CHECK(carol.Carried(STIMPAK) == 2);

	bob.away.clear();
	bob.Handle(nullptr, bob.Fetch(f));  // the packets, before the frame that tries again
	CHECK(bob.In(CHEST, STIMPAK) == 0);
	bob.Frame(f);
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(CHEST, STIMPAK) == 0);
	CHECK(bob.holders[CHEST] == carol.holders[CHEST]);
}

TEST("loot sync: a corpse a friend looted before its kill reached us keeps its loot until the kill comes, then loses it")
{
	// The server sends the kill first; here it comes late (a reordering, or the kill lost and resent).
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B };
	alice.Join(f, "Alice");
	bob.Join(f, "Bob");
	for (auto* game : { &alice, &bob }) {
		game->holderFacts[RAIDER] = { .kind = LootScope::Holder::kActor, .alive = true };
		game->holders[RAIDER] = { { AMMO, 20 }, { STIMPAK, 2 } };
	}
	alice.Kill(f, RAIDER);
	alice.Take(f, RAIDER, AMMO, 20);
	alice.Take(f, RAIDER, STIMPAK, 1);
	CHECK(alice.loot.Stats().sent == 2);

	auto       toBob = bob.Fetch(f);
	const auto death = std::ranges::find_if(toBob, [](const auto& a_packet) { return IsType(a_packet, MT::kActorDied); });
	REQUIRE(death != toBob.end());
	const auto kill = *death;
	toBob.erase(death);
	bob.Handle(f, toBob);
	CHECK(bob.In(RAIDER, AMMO) == 20);  // alive here: its pockets wait
	CHECK(bob.loot.Waiting().size() == 2);
	bob.Wait(f, 5s);
	CHECK(bob.In(RAIDER, AMMO) == 20);

	bob.Handle(f, { kill });
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(RAIDER, AMMO) == 0);
	CHECK(bob.In(RAIDER, STIMPAK) == 1);
	CHECK(bob.holders[RAIDER] == alice.holders[RAIDER]);
}

TEST("loot sync: a joiner's report of a living NPC's pockets changes nothing for the others, and after ten minutes stops holding up that NPC's loot")
{
	// Carol's game reports what it shouldn't (a build from before the rule, or a changed one): she
	// pickpockets the settler, and trades with him. The settler lives in Bob's game, and never dies there.
	Fixture f;
	Game    alice{ 1, 0xA11CE }, bob{ 2, 0xB0B }, carol{ 3, 0xCA401 };
	for (auto* game : { &alice, &bob, &carol }) {
		game->Join(f, "P");
		game->holderFacts[SETTLER] = { .kind = LootScope::Holder::kActor, .alive = true };
		game->holders[SETTLER] = { { CAPS, 30 }, { STIMPAK, 3 } };
	}
	// Alice's own pickpocketing isn't reported at all.
	alice.Take(f, SETTLER, CAPS, 10);
	CHECK(alice.loot.Stats().sent == 0);
	CHECK(alice.loot.KeptSent(LootScope::Scope::kNotYetDead) == 1);

	carol.SendRaw(f, SETTLER, CAPS, -30);
	carol.SendRaw(f, SETTLER, STIMPAK, -3);
	carol.SendRaw(f, SETTLER, STIMPAK, 1);
	bob.Receive(f);
	CHECK(bob.loot.Waiting().size() == 3);
	CHECK(bob.In(SETTLER, CAPS) == 30);

	CHECK(!bob.Wait(f, 9min));
	CHECK(bob.loot.Waiting().size() == 3);
	bob.Wait(f, 1min);
	// All three were given up together: none went in.
	CHECK(bob.loot.Waiting().empty());
	CHECK(bob.In(SETTLER, CAPS) == 30);
	CHECK(bob.In(SETTLER, STIMPAK) == 3);
	CHECK(bob.loot.KeptApplied(LootScope::Scope::kNotYetDead) == 3);

	// Later the settler dies in Alice's game, and she loots him: that reaches Bob at once.
	alice.Kill(f, SETTLER);
	alice.Take(f, SETTLER, STIMPAK, 3);
	bob.Receive(f);
	CHECK(bob.In(SETTLER, STIMPAK) == 0);
	CHECK(bob.In(SETTLER, CAPS) == 30);
}

TEST("loot sync: a flood of living-NPC loot from a joiner can't push out a friend's loot for a chest that isn't loaded yet")
{
	// The server relays a joiner's flood of pickpocketing reports for NPCs alive in our game (more than
	// the queue holds); among them, a friend takes from a chest across the map.
	Game bob{ 2, 0xB0B };
	bob.holderFacts[SETTLER] = { .kind = LootScope::Holder::kActor, .alive = true };
	bob.holderFacts[RAIDER] = { .kind = LootScope::Holder::kActor, .alive = true };
	bob.holders[SETTLER] = { { CAPS, 9000 } };
	bob.holders[RAIDER] = { { CAPS, 9000 } };
	bob.holders[CHEST] = { { AMMO, 50 } };
	bob.away.insert(CHEST);
	constexpr std::uint32_t CAROL = 3, ALICE = 1;
	LootTest::Packets flood;
	std::uint32_t     index = 0;
	for (; index < LootSync::Loot::MAX_PENDING + 100; ++index) {
		if (index == 1000) {
			flood.push_back(Protocol::Encode(Protocol::IndexedContainerChange{ index++, ALICE, { CHEST, AMMO, -20 } }));
		}
		flood.push_back(Protocol::Encode(Protocol::IndexedContainerChange{ index, CAROL, { index % 2 ? SETTLER : RAIDER, CAPS, -1 } }));
	}
	flood.push_back(Protocol::Encode(Protocol::IndexedContainerChange{ index, ALICE, { SAFE, CAPS, -5 } }));  // after the queue filled
	bob.away.insert(SAFE);
	bob.holders[SAFE] = { { CAPS, 10 } };
	bob.Handle(nullptr, flood);
	CHECK(bob.loot.Waiting().size() == LootSync::Loot::MAX_PENDING);
	CHECK(bob.loot.Stats().pendingFull > 0);

	// Bob walks to the chest and the safe: Alice's takes go in.
	bob.away.clear();
	bob.loot.Retry(bob, LootTest::APPLY_INTERVAL);
	CHECK(bob.In(CHEST, AMMO) == 30);
	CHECK(bob.In(SAFE, CAPS) == 5);
	CHECK(bob.In(SETTLER, CAPS) == 9000);  // nothing of the flood: the NPCs live
	CHECK(bob.In(RAIDER, CAPS) == 9000);
	CHECK(bob.applied == 2);
}
