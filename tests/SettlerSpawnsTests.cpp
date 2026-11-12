// The settlers the host's game made at run time are shared with friends as spawns of a kind of their own
// (game/SettlerSpawns.h): which of the host's NPCs is one, how one is offered when the settlers' places are full, how the
// places are counted apart from a fight's in a game's own book and by the server, and whose word a settler is.

#include "Test.h"

#include "ServerFixture.h"

#include "game/SettlerSpawns.h"
#include "game/SpawnBook.h"

#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;
using Protocol::SpawnKind;
using SettlerSpawns::As;
using SettlerSpawns::Facts;
using SettlerSpawns::Held;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t WORLD = 0x0000003C;       // the Commonwealth
	constexpr std::uint32_t SETTLER = 0x00020593;     // a settler's base (a plugin's NPC)
	constexpr std::uint32_t RAIDER = 0x0001D4D2;
	constexpr std::uint32_t BENCH = 0x000250FE;       // Sanctuary's workbench (a plugin's reference)
	constexpr std::uint32_t SPAWN = 0xFF001234;
	constexpr std::uint64_t HOST_IDENTITY = 0x1111;

	Protocol::Spawn Settler(std::uint32_t a_id, std::uint32_t a_bench = BENCH)
	{
		return { 0, a_id, SETTLER, 0, WORLD, 1000.0f, 2000.0f, 100.0f, 0.5f, a_bench, 0, SpawnKind::kSettler };
	}

	Protocol::Spawn Raider(std::uint32_t a_id)
	{
		return { 0, a_id, RAIDER, 0, WORLD, 1000.0f, 2000.0f, 100.0f, 0.5f };
	}

	auto Report(const Protocol::Spawn& a_spawn)
	{
		return Protocol::Encode(a_spawn, MT::kReportSpawn);
	}

	std::vector<Protocol::ActorOwner> Owners(const Packets& a_packets)
	{
		std::vector<Protocol::ActorOwner> out;
		for (const auto& batch : Decoded(a_packets, MT::kActorOwners, [](auto a_data) { return Protocol::DecodeOwners(a_data); })) {
			out.insert(out.end(), batch.begin(), batch.end());
		}
		return out;
	}

	bool HasOwner(const std::vector<Protocol::ActorOwner>& a_owners, std::uint32_t a_ref, std::uint32_t a_player)
	{
		return std::ranges::any_of(a_owners, [&](const auto& a_owner) { return a_owner.refId == a_ref && a_owner.playerId == a_player; });
	}

	std::vector<Protocol::Spawn> Spawned(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kSpawned, [](auto a_data) { return Protocol::DecodeSpawn(a_data); });
	}

	Server::Options Hosted(bool a_sharedWorkshop = true)
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		options.rules.sharedWorkshop = a_sharedWorkshop;
		return options;
	}

	// The host's own game: on the server's machine, with the identity the server was started for.
	std::uint32_t JoinAsHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		return JoinAs(a_f, a_peer, "Host", HOST_IDENTITY);
	}

	// A runtime settler of a settlement the host's game runs.
	constexpr Facts Recruit()
	{
		Facts facts;
		facts.serves = true;
		facts.bench = BENCH;
		facts.benchIsWorkbench = true;
		facts.benchOurs = true;
		facts.alive = true;
		return facts;
	}

	template <class Change>
	constexpr std::uint32_t BenchWith(Change a_change)
	{
		auto facts = Recruit();
		a_change(facts);
		return SettlerSpawns::BenchToName(facts);
	}
}

// ---- the host's game ---------------------------------------------------------------------------------------------

TEST("settler spawns: the host's game shares its runtime settler of a workbench it owns, and nothing else as one")
{
	CHECK(SettlerSpawns::BenchToName(Recruit()) == BENCH);
	static_assert(SettlerSpawns::BenchToName(Recruit()) == BENCH);

	// Not the host's game, the rule off, or no session: nothing is a settler to share.
	CHECK(BenchWith([](Facts& a_facts) { a_facts.serves = false; }) == 0);
	// Linked to nothing, to something that is no workbench, or to a workbench our player does not own.
	CHECK(BenchWith([](Facts& a_facts) { a_facts.bench = 0; }) == 0);
	CHECK(BenchWith([](Facts& a_facts) { a_facts.benchIsWorkbench = false; }) == 0);
	CHECK(BenchWith([](Facts& a_facts) { a_facts.benchOurs = false; }) == 0);
	// A workbench our game made at run time names nothing a friend's game has.
	CHECK(BenchWith([](Facts& a_facts) { a_facts.bench = 0xFF000900; }) == 0);
	// Dead; our player's companion (the companion handling's); a turret or a robot (a built piece, shared as the pieces are).
	CHECK(BenchWith([](Facts& a_facts) { a_facts.alive = false; }) == 0);
	CHECK(BenchWith([](Facts& a_facts) { a_facts.companion = true; }) == 0);
	CHECK(BenchWith([](Facts& a_facts) { a_facts.built = true; }) == 0);
	// Nothing known: nothing shared.
	CHECK(SettlerSpawns::BenchToName(Facts{}) == 0);
}

TEST("settler spawns: with a settler's place free it goes as a settler; with none, as a fight's spawn or not at all")
{
	CHECK(SettlerSpawns::OfferAs(true, false) == As::kSettler);
	CHECK(SettlerSpawns::OfferAs(true, true) == As::kSettler);  // fighting too: the place is the settlers'
	CHECK(SettlerSpawns::OfferAs(false, true) == As::kFight);   // as before the rule
	CHECK(SettlerSpawns::OfferAs(false, false) == As::kWait);
}

// ---- places ------------------------------------------------------------------------------------------------------

TEST("settler spawns: the settlers' places and a fight's are counted apart")
{
	constexpr auto S = Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER;
	constexpr auto F = Protocol::MAX_SPAWNS_PER_PLAYER;
	CHECK(S >= 20);  // a large settlement's settlers
	CHECK(SettlerSpawns::MAX_IN_SESSION == S);  // one game runs the settlements

	CHECK(SettlerSpawns::Room(true, Held{ 0, 0 }, S, F));
	CHECK(SettlerSpawns::Room(true, Held{ S - 1, F }, S, F));   // a full fight takes no settler's place
	CHECK(!SettlerSpawns::Room(true, Held{ S, 0 }, S, F));
	CHECK(SettlerSpawns::Room(false, Held{ S, F - 1 }, S, F));  // a full settlement takes none of a fight's
	CHECK(!SettlerSpawns::Room(false, Held{ 0, F }, S, F));

	CHECK(SettlerSpawns::IsSettler(SpawnKind::kSettler));
	CHECK(!SettlerSpawns::IsSettler(SpawnKind::kPlain) && !SettlerSpawns::IsSettler(SpawnKind::kQuestActor) && !SettlerSpawns::IsSettler(SpawnKind::kEncounter));
}

TEST("settler spawns: our book holds a settlement's settlers beside a fight's spawns")
{
	const auto              T0 = SpawnBook::Clock::time_point{} + 1h;
	const auto              none = [](std::uint32_t) { return false; };
	constexpr std::uint32_t US = 1;
	SpawnBook::Ours         ours;
	CHECK(SpawnBook::Ours::MAX_SETTLERS == Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER);
	CHECK(ours.HasRoom(true) && ours.HasRoom(false));

	// A full settlement: every settler has a place, the next has none, and a fight still has its whole share.
	const auto first = ours.Offer(0xFF010000, T0, none, true);
	REQUIRE(first);
	for (std::uint32_t i = 1; i < SpawnBook::Ours::MAX_SETTLERS; ++i) {
		CHECK(ours.Offer(0xFF010000 + i, T0, none, true));
	}
	CHECK(ours.Settlers() == SpawnBook::Ours::MAX_SETTLERS);
	CHECK(!ours.HasRoom(true));
	CHECK(!ours.Offer(0xFF01FFFF, T0, none, true));
	CHECK(ours.HasRoom(false));
	for (std::uint32_t i = 0; i < SpawnBook::Ours::MAX; ++i) {
		CHECK(ours.Offer(0xFF020000 + i, T0, none));
	}
	CHECK(!ours.HasRoom(false));
	CHECK(!ours.Offer(0xFF02FFFF, T0, none));
	CHECK(ours.Size() == SpawnBook::Ours::MAX_SETTLERS + SpawnBook::Ours::MAX);
	CHECK(ours.Settlers() == SpawnBook::Ours::MAX_SETTLERS);

	// A granted settler is ours under its id as any spawn is, and keeps its place.
	CHECK(ours.Answered(*first, US, US, T0 + 1s));
	CHECK(ours.IdOf(0xFF010000) == *first);
	CHECK(ours.Settlers() == SpawnBook::Ours::MAX_SETTLERS);
	// One the server refuses (nobody's) gives its place back: a settler's place, not a fight's.
	const auto second = ours.Offer(0xFF010001, T0 + 6s, none, true);  // unanswered: asked again under its id
	REQUIRE(second);
	CHECK(ours.Answered(*second, 0, US, T0 + 7s));
	CHECK(ours.Settlers() == SpawnBook::Ours::MAX_SETTLERS - 1);
	CHECK(ours.HasRoom(true) && !ours.HasRoom(false));

	// The other way round: a full fight first, and the settlers still have theirs.
	SpawnBook::Ours fight;
	for (std::uint32_t i = 0; i < SpawnBook::Ours::MAX; ++i) {
		CHECK(fight.Offer(0xFF020000 + i, T0, none));
	}
	CHECK(fight.Settlers() == 0);
	CHECK(!fight.HasRoom(false) && fight.HasRoom(true));
	CHECK(fight.Offer(0xFF010000, T0, none, true));
	CHECK(fight.Settlers() == 1);

	// A friend's game's book has room for the server's spawns and the host's settlers beside them.
	CHECK(SpawnBook::Theirs::MAX == 128 + Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER);
}

// ---- the wire ----------------------------------------------------------------------------------------------------

TEST("settler spawns: a settler names its workbench and no alias on the wire")
{
	for (const auto type : { MT::kReportSpawn, MT::kSpawned }) {
		const auto settler = Protocol::DecodeSpawn(Protocol::Encode(Settler(SPAWN), type));
		REQUIRE(settler);
		CHECK(*settler == Settler(SPAWN));
		CHECK(settler->kind == SpawnKind::kSettler && settler->quest == BENCH && settler->alias == 0);

		auto bad = Settler(SPAWN);
		bad.quest = 0;  // a settler of no workbench
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(bad, type)));
		bad = Settler(SPAWN);
		bad.alias = 1;  // a settler is in no alias
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(bad, type)));
		bad = Settler(SPAWN);
		bad.kind = static_cast<SpawnKind>(4);  // a kind the protocol has not
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(bad, type)));
	}
}

// ---- whose word a settler is ---------------------------------------------------------------------------------------

TEST("settler spawns: a settler that may not be one is taken as a plain spawn")
{
	const auto kept = SettlerSpawns::Taken(Settler(SPAWN), true);
	CHECK(kept == Settler(SPAWN));
	const auto plain = SettlerSpawns::Taken(Settler(SPAWN), false);
	CHECK(plain.kind == SpawnKind::kPlain && plain.quest == 0 && plain.alias == 0);
	CHECK(plain.refId == SPAWN && plain.base == SETTLER && plain.x == 1000.0f);
	CHECK(Protocol::DecodeSpawn(Protocol::Encode(plain, MT::kSpawned)).has_value());  // what is passed on reads back

	// Any other kind is as it was, allowed or not.
	const Protocol::Spawn encounter{ 0, SPAWN, RAIDER, 0, WORLD, 1.0f, 2.0f, 3.0f, 0.0f, 0x0017E729, 3, SpawnKind::kEncounter };
	CHECK(SettlerSpawns::Taken(encounter, false) == encounter);
	CHECK(SettlerSpawns::Taken(Raider(SPAWN), false) == Raider(SPAWN));
}

// ---- the server ----------------------------------------------------------------------------------------------------

TEST("settler spawns: the server passes the host's game's settler on as one, to friends there and to a newcomer")
{
	Fixture    f{ Hosted() };
	const auto host = JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(Settler(SPAWN)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, host));
	const auto toBob = Spawned(Received(f, 2));
	REQUIRE(toBob.size() == 1);
	auto expected = Settler(SPAWN);
	expected.playerId = host;
	CHECK(toBob[0] == expected);

	JoinAs(f, 3, "Carol");
	const auto toCarol = Spawned(Received(f, 3));
	REQUIRE(toCarol.size() == 1);
	CHECK(toCarol[0] == expected);
}

TEST("settler spawns: the server takes a friend's settler, and the host's with the rule off, as a plain spawn")
{
	{
		Fixture f{ Hosted() };
		JoinAsHost(f, 1);
		const auto bob = JoinAs(f, 2, "Bob");
		Clear(f, { 1, 2 });
		f.fake->Deliver(2, Report(Settler(SPAWN)));
		CHECK(HasOwner(Owners(Received(f, 2)), SPAWN, bob));  // shared, as any spawn of his
		const auto toHost = Spawned(Received(f, 1));
		REQUIRE(toHost.size() == 1);
		CHECK(toHost[0].kind == SpawnKind::kPlain && toHost[0].quest == 0 && toHost[0].alias == 0);
		CHECK(toHost[0].refId == SPAWN && toHost[0].base == SETTLER && toHost[0].playerId == bob);
	}
	{
		Fixture    f{ Hosted(false) };
		const auto host = JoinAsHost(f, 1);
		JoinAs(f, 2, "Bob");
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Report(Settler(SPAWN)));
		CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, host));
		const auto toBob = Spawned(Received(f, 2));
		REQUIRE(toBob.size() == 1);
		CHECK(toBob[0].kind == SpawnKind::kPlain && toBob[0].quest == 0);
	}
	{
		// A dedicated server has no host's game: nobody's settler is one.
		Fixture f;
		JoinAs(f, 1, "Alice");
		JoinAs(f, 2, "Bob");
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, Report(Settler(SPAWN)));
		const auto toBob = Spawned(Received(f, 2));
		REQUIRE(toBob.size() == 1);
		CHECK(toBob[0].kind == SpawnKind::kPlain && toBob[0].quest == 0);
	}
}

TEST("settler spawns: the server holds the host's settlers beside its fight's spawns, each against its own count")
{
	Fixture    f{ Hosted() };
	const auto host = JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// A full settlement, within the burst a game may share at once.
	for (std::uint32_t i = 0; i < Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER; ++i) {
		f.fake->Deliver(1, Report(Settler(0xFF100000 + i)));
	}
	auto owners = Owners(Received(f, 1));
	CHECK(owners.size() == Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER);
	CHECK(HasOwner(owners, 0xFF100000, host));
	CHECK(HasOwner(owners, 0xFF100000 + static_cast<std::uint32_t>(Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER) - 1, host));
	const auto toBob = Spawned(Received(f, 2));
	CHECK(toBob.size() == Protocol::MAX_SETTLER_SPAWNS_PER_PLAYER);
	CHECK(std::ranges::all_of(toBob, [](const auto& a_spawn) { return a_spawn.kind == SpawnKind::kSettler && a_spawn.quest == BENCH; }));

	// One more settler has no place, whatever the budget; a fight's spawn has (once the budget has a token again: the
	// settlers took the burst).
	std::this_thread::sleep_for(600ms);
	f.fake->Deliver(1, Report(Settler(0xFF1000FF)));
	CHECK(HasOwner(Owners(Received(f, 1)), 0xFF1000FF, 0));  // refused: nobody's
	CHECK(f.Logged("spawn FF1000FF from player"));
	f.fake->Deliver(1, Report(Raider(0xFF200000)));
	CHECK(HasOwner(Owners(Received(f, 1)), 0xFF200000, host));
	const auto more = Spawned(Received(f, 2));
	REQUIRE(more.size() == 1);
	CHECK(more[0].refId == 0xFF200000 && more[0].kind == SpawnKind::kPlain);

	// A settler dies: its place is free for the next.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ActorDeath{ 0xFF100000, 999, true, 1000.0f, 2000.0f, 100.0f }, MT::kReportDeath));
	Clear(f, { 1, 2 });
	std::this_thread::sleep_for(600ms);
	f.fake->Deliver(1, Report(Settler(0xFF1000FF)));
	CHECK(HasOwner(Owners(Received(f, 1)), 0xFF1000FF, host));
}
