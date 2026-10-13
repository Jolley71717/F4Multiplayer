// What one player may cost everyone else: their budgets (world states, XP, faces), kept across a
// reconnect, their share of each of the session's tables, and the tables' own size. Each test plays two
// or three players against a real server; the first one is the promise the rest must keep: honest play
// never meets a limit.

#include "FriendGame.h"

#include <array>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Reason = Protocol::ClaimReason;

	constexpr std::uint64_t HOST_IDENTITY = 0x1111;
	constexpr std::uint64_t BOB_STEAM = 0x5EA0B0B;  // what the transport knows Bob by
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t ITEM = 0x0004A001;
	constexpr std::uint32_t QUEST = 0x0005B001;

	// What a limit says in the log. Honest play must never write any of these.
	constexpr std::array<std::string_view, 7> LIMIT_LINES{ "too many events", "their share", "too often", "% full", "keeps at most", "dropping some", "no more of theirs" };

	std::vector<std::string> LimitLines(Fixture& a_f)
	{
		std::vector<std::string> out;
		for (const auto text : LIMIT_LINES) {
			std::ranges::move(a_f.LogLines(text), std::back_inserter(out));
		}
		return out;
	}

	Server::Options HostedOptions()
	{
		Server::Options options;
		options.hostIdentity = HOST_IDENTITY;
		return options;
	}

	// The host's own game: on the server's machine, with the identity the server was started for.
	std::uint32_t JoinAsHost(Fixture& a_f, PeerId a_peer)
	{
		a_f.fake->SetLocal(a_peer);
		return JoinAs(a_f, a_peer, "Host", HOST_IDENTITY);
	}

	Protocol::Hello HelloFrom(std::string a_name, std::uint64_t a_identity, std::uint64_t a_session = 0, std::uint32_t a_containerFrom = 0)
	{
		auto hello = Fixture::MakeHello(std::move(a_name), a_identity);
		hello.world = { a_session, a_containerFrom };
		return hello;
	}

	// --- what clients send ---

	auto DeathAt(std::uint32_t a_ref, float a_x) { return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true, a_x, 0.0f, 0.0f }, MT::kReportDeath); }
	auto Death(std::uint32_t a_ref) { return Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true }, MT::kReportDeath); }
	auto Health(std::uint32_t a_ref, float a_health) { return Protocol::Encode(Protocol::ActorHealth{ a_ref, a_health }, MT::kReportHealth); }
	auto Loot(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count) { return Protocol::Encode(Protocol::ContainerChange{ a_container, a_item, a_count }, MT::kReportContainer); }
	auto Pickup(std::uint32_t a_ref) { return Protocol::Encode(Protocol::RefPickedUp{ a_ref }, MT::kReportPickup); }
	auto Door(std::uint32_t a_ref, std::uint8_t a_open) { return Protocol::Encode(Protocol::RefState{ a_ref, a_open, Protocol::RefState::kNo }, MT::kReportRefState); }
	auto Stage(std::uint32_t a_quest, std::uint16_t a_stage) { return Protocol::Encode(Protocol::QuestStage{ a_quest, a_stage }, MT::kReportQuestStage); }
	auto Xp(float a_xp) { return Protocol::Encode(Protocol::XpGain{ 0, a_xp }, MT::kReportXp); }
	auto Gear(std::uint32_t a_ref, std::vector<std::uint32_t> a_items) { return Protocol::Encode(Protocol::NpcEquipment{ 0, a_ref, std::move(a_items) }, MT::kReportNpcEquipment); }
	auto Request(std::uint64_t a_session, std::uint32_t a_from) { return Protocol::Encode(Protocol::WorldRequest{ a_session, a_from }); }

	auto Claims(std::vector<Protocol::ActorClaim> a_claims) { return Protocol::Encode(a_claims); }

	auto Build(std::uint32_t a_ref, std::uint8_t a_op = Protocol::WorkshopOp::kPlaced, float a_x = 10.0f)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.op = a_op;
		item.base = 0x0001F2A1;
		item.workshop = 0x000250FE;
		item.position[0] = a_x;
		return Protocol::Encode(item, MT::kReportWorkshopItem);
	}

	auto Positions(const std::vector<std::uint32_t>& a_refs, float a_x)
	{
		std::vector<Protocol::ActorState> states;
		for (const auto ref : a_refs) {
			Protocol::ActorState state;
			state.refId = ref;
			state.x = a_x;
			states.push_back(state);
		}
		return Protocol::Encode(states, MT::kActorStates);
	}

	auto Face(float a_weight)
	{
		Protocol::Face face;
		face.race = 0x00013746;
		face.headParts = { 0x0004DA7A };
		face.weight[0] = a_weight;
		return Protocol::Encode(face, MT::kReportFace);
	}

	// The container change indexes in live echoes and world states, each as often as it came.
	std::multiset<std::uint32_t> ContainerIndexes(const Packets& a_packets)
	{
		std::multiset<std::uint32_t> out;
		for (const auto& change : Decoded(a_packets, MT::kContainerChanged, Protocol::DecodeIndexedContainerChange)) {
			out.insert(change.index);
		}
		for (const auto& world : WorldStates(a_packets)) {
			for (const auto& change : world.containerChanges) {
				out.insert(change.index);
			}
		}
		return out;
	}

	std::size_t IndexOf(const Packets& a_packets, MT a_type)
	{
		const auto it = std::ranges::find_if(a_packets, [&](const auto& a_packet) { return !a_packet.empty() && a_packet[0] == Type(a_type); });
		return static_cast<std::size_t>(it - a_packets.begin());
	}

	// Whether a_peer had been sent no packet of a_type once the server had handled everything delivered so
	// far (a_via, a welcomed player, is asked to make sure), keeping what came in a_got. That only means it
	// waited while their budget can't have refilled since a_asked (half a refill: it may have been part way
	// to the next one then). On a machine too slow for that, it says so and claims nothing.
	bool NoneYet(Fixture& a_f, PeerId a_via, PeerId a_peer, MT a_type, Packets& a_got, std::chrono::steady_clock::time_point a_asked, std::chrono::milliseconds a_refill)
	{
		Sync(a_f, a_via);
		std::ranges::move(a_f.fake->Take(a_peer), std::back_inserter(a_got));
		if (std::chrono::steady_clock::now() - a_asked >= a_refill / 2) {
			std::printf("    (too slow to tell whether it waited)\n");
			return true;
		}
		return Count(a_got, a_type) == 0;
	}

	// What Alice's game was told of the session's loot, as a game with all of it would have it.
	Inventories SessionAsToldTo(const Packets& a_packets)
	{
		return Inventories::FromEchoes(Decoded(a_packets, MT::kContainerChanged, Protocol::DecodeIndexedContainerChange));
	}
}

// ---- honest play ----------------------------------------------------------------------------

// The promise every limit below keeps: the host and a friend over Steam fight, loot, build, reload a
// save and rejoin after a crash at the rates the game makes, and nothing of theirs is held back, delayed
// or logged as too much.
TEST("budgets: a busy evening of two honest players never meets a limit, and each sees all the other did")
{
	Fixture f{ HostedOptions() };
	JoinAsHost(f, 1);
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto bobWelcome = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(bobWelcome);
	const auto session = bobWelcome->sessionId;

	Packets toHost;
	Packets toBob;  // across both of his connections
	PeerId  bobPeer = 2;
	const auto collect = [&] {
		std::ranges::move(f.fake->Take(1), std::back_inserter(toHost));
		std::ranges::move(f.fake->Take(bobPeer), std::back_inserter(toBob));
	};

	// Each runs the raiders around them; a fight is ten raiders, five each.
	std::vector<std::uint32_t> hostRaiders, bobRaiders;
	for (std::uint32_t i = 0; i < 5; ++i) {
		hostRaiders.push_back(0x00031000 + i);
		bobRaiders.push_back(0x00032000 + i);
	}
	const auto claimAll = [&](PeerId a_peer, const std::vector<std::uint32_t>& a_raiders) {
		std::vector<Protocol::ActorClaim> claims;
		for (const auto ref : a_raiders) {
			claims.push_back({ ref, Reason::kUnowned });
		}
		f.fake->Deliver(a_peer, Claims(claims));
		for (const auto ref : a_raiders) {
			f.fake->Deliver(a_peer, Gear(ref, { 0x00024F55, 0x000B3F4E }));
		}
	};
	claimAll(1, hostRaiders);
	claimAll(2, bobRaiders);

	std::uint32_t sequence[2] = {};
	std::size_t   changes = 0, hostKills = 0, bobKills = 0, hostPickups = 0, bobPickups = 0, builds = 0;
	std::uint32_t bobSaveFrom = 0;
	for (int step = 0; step < 30; ++step) {  // three seconds, a tenth at a time
		for (const PeerId peer : { PeerId{ 1 }, bobPeer }) {
			const int  who = peer == 1 ? 0 : 1;
			const auto& raiders = who == 0 ? hostRaiders : bobRaiders;
			// Moving (30 a second), the NPCs they run (20 a second), shooting.
			for (int i = 0; i < 3; ++i) {
				Protocol::PlayerState state;
				state.sequence = ++sequence[who];
				state.x = static_cast<float>(step);
				f.fake->Deliver(peer, Protocol::Encode(state));
			}
			f.fake->Deliver(peer, Positions(raiders, static_cast<float>(step)));
			f.fake->Deliver(peer, Positions(raiders, static_cast<float>(step) + 0.5f));
			f.fake->Deliver(peer, Protocol::Encode(Protocol::Shot{ 0, 0, 0, 0 }, MT::kReportShot));
			if (step % 2 == 0) {
				f.fake->Deliver(peer, Health(raiders[(step / 6) % raiders.size()], 0.5f));
			}
			// A raider falls every 0.6 s: XP, and "take all" from the corpse (six kinds of thing).
			if (step % 6 == 0) {
				const auto raider = raiders[step / 6];
				f.fake->Deliver(peer, DeathAt(raider, 100.0f * step));
				f.fake->Deliver(peer, Xp(40.0f));
				for (std::uint32_t item = 0; item < 6; ++item) {
					f.fake->Deliver(peer, Loot(raider, ITEM + item, -1));
				}
				changes += 6;
				(who == 0 ? hostKills : bobKills) += 1;
			}
			// A loose item picked up now and then; a door; a quest moves on.
			if (step % 3 == who) {
				f.fake->Deliver(peer, Pickup(0x00040000 + who * 0x1000 + step));
				(who == 0 ? hostPickups : bobPickups) += 1;
			}
			if (step % 10 == 5) {
				f.fake->Deliver(peer, Door(0x0003D000 + who, step % 20 == 5 ? Protocol::RefState::kYes : Protocol::RefState::kNo));
			}
			if (step == 12) {
				f.fake->Deliver(peer, Stage(QUEST + who, 10));
			}
		}
		// The host empties a chest of forty kinds of thing in one go, and builds at a settlement: a
		// piece a tenth of a second, scrapping the junk that was there.
		if (step == 10) {
			for (std::uint32_t item = 0; item < 40; ++item) {
				f.fake->Deliver(1, Loot(CHEST, ITEM + 0x100 + item, -3));
			}
			changes += 40;
		}
		f.fake->Deliver(1, Build(0xFF000100 + step));
		++builds;
		if (step >= 20) {
			f.fake->Deliver(1, Build(0x00050000 + step, Protocol::WorkshopOp::kScrapped));
			++builds;
		}
		// Bob loads a save he made a moment ago (his game asks for what happened since), and later his
		// game crashes: he starts it again, loads that save and rejoins.
		if (step == 14) {
			Sync(f, bobPeer);
			collect();
			bobSaveFrom = static_cast<std::uint32_t>(ContainerIndexes(toBob).size());
			f.fake->Deliver(bobPeer, Request(session, bobSaveFrom));
		}
		if (step == 20) {
			f.fake->Drop(bobPeer);
			collect();
			bobPeer = 3;
			f.fake->SetIdentity(bobPeer, BOB_STEAM);
			f.fake->Connect(bobPeer);
			f.fake->Deliver(bobPeer, Protocol::Encode(HelloFrom("Bob", 0xB0B, session, bobSaveFrom)));
			REQUIRE(Await(f, bobPeer, MT::kWelcome, toBob, 1000ms));  // at once: no wait for a budget
			claimAll(bobPeer, std::vector<std::uint32_t>(bobRaiders.begin() + 4, bobRaiders.end()));
		}
		std::this_thread::sleep_for(100ms);
		collect();
	}
	Sync(f, 1);
	Sync(f, bobPeer);
	collect();

	CHECK(LimitLines(f).empty());
	for (const auto& line : LimitLines(f)) {
		std::printf("    limit hit: %s\n", line.c_str());
	}

	// Every kill, pickup and container change of each reached the other; Bob's two connections and his
	// world states between them cover every change exactly as many times as a game needs (at least once).
	CHECK(Count(toHost, MT::kActorDied) == bobKills);
	CHECK(Decoded(toHost, MT::kRefPickedUp, Protocol::DecodeRefPickedUp).size() == bobPickups);
	const auto hostSaw = ContainerIndexes(toHost);
	CHECK(hostSaw.size() == changes);
	CHECK(std::set<std::uint32_t>(hostSaw.begin(), hostSaw.end()).size() == changes);
	const auto bobSaw = ContainerIndexes(toBob);
	for (std::uint32_t index = 0; index < changes; ++index) {
		CHECK(bobSaw.contains(index));
	}
	std::size_t bobHeardOfKills = Count(toBob, MT::kActorDied);
	for (const auto& world : WorldStates(toBob)) {
		bobHeardOfKills += world.deadActors.size();
	}
	CHECK(bobHeardOfKills >= hostKills);
	// The host's building: relayed live, and the rest in the rejoin's welcome.
	std::set<std::uint32_t> builtSeen;
	for (const auto& item : Decoded(toBob, MT::kWorkshopItem, Protocol::DecodeWorkshopItem)) {
		builtSeen.insert(item.refId);
	}
	for (const auto& batch : Decoded(toBob, MT::kWorkshopItems, Protocol::DecodeWorkshopItems)) {
		for (const auto& item : batch) {
			builtSeen.insert(item.refId);
		}
	}
	CHECK(builtSeen.size() == builds);
	// The XP of every kill, and every gear report, went through.
	CHECK(Count(toHost, MT::kPartyXp) == bobKills);
	CHECK(Count(toBob, MT::kPartyXp) >= hostKills - 1);  // (one may have come while he was away)
	CHECK(Count(toHost, MT::kNpcEquipment) == bobRaiders.size() + 1);
	// Three world states, all straight away: joining, the reload, the rejoin.
	CHECK(WorldStates(toBob).size() == 3);
}

// ---- the world state ------------------------------------------------------------------------

// G13: a friend whose modified game quits and rejoins in a loop (each time under a made-up identity, as
// the transport still knows who they are) gets no more world states than a player who reloads a lot,
// and what the others do meanwhile still reaches them, inside the world state, not before it.
TEST("budgets: a friend rejoining in a loop waits for their budget, and gets the loot made meanwhile in their world state")
{
	Server::Options options;
	options.worldStateRefill = 1000ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto first = f.Join(2, HelloFrom("Bob", 0xB01));
	REQUIRE(first);
	f.fake->Deliver(2, Request(first->sessionId, 0));
	f.fake->Deliver(2, Request(first->sessionId, 0));
	f.fake->Deliver(2, Request(first->sessionId, 0));  // the fourth in a moment: owed
	Sync(f, 2);
	CHECK(f.Logged("'Bob' has asked for the world too often"));
	f.fake->Drop(2);

	f.fake->SetIdentity(3, BOB_STEAM);
	f.fake->Connect(3);
	const auto asked = std::chrono::steady_clock::now();
	f.fake->Deliver(3, Protocol::Encode(HelloFrom("Bob", 0xB02)));
	Packets toBob;
	CHECK(NoneYet(f, 1, 3, MT::kWelcome, toBob, asked, options.worldStateRefill));
	CHECK(Count(toBob, MT::kWorldState) == 0);
	CHECK(f.Logged("says hello too often"));
	CHECK(!f.fake->Disconnected(3));  // waiting for a budget isn't "no hello"

	f.fake->Deliver(1, Loot(CHEST, ITEM, -1));  // Alice loots meanwhile
	const auto echo = Decoded(Received(f, 1), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	REQUIRE(echo.size() == 1);

	REQUIRE(Await(f, 3, MT::kWorldState, toBob));
	std::ranges::move(During(f, 3, 300ms), std::back_inserter(toBob));
	CHECK(Count(toBob, MT::kWelcome) == 1);
	CHECK(IndexOf(toBob, MT::kWelcome) < IndexOf(toBob, MT::kWorldState));
	const auto worlds = WorldStates(toBob);
	REQUIRE(worlds.size() == 1);
	REQUIRE(worlds[0].containerChanges.size() == 1);
	CHECK(worlds[0].containerChanges[0].index == echo[0].index);
	CHECK(IndexOf(toBob, MT::kContainerChanged) > IndexOf(toBob, MT::kWorldState));  // no echo of it before
}

// The dedicated server over UDP can't name a player by Steam ID: the identity their game says hello with
// keeps the budget instead, so a new connection doesn't bring a fresh one.
TEST("budgets: over UDP, a friend rejoining in a loop under one identity waits for the budget the last connection spent")
{
	Server::Options options;
	options.worldStateRefill = 1000ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	for (PeerId peer = 2; peer <= 4; ++peer) {  // three joins: the bucket
		REQUIRE(f.Join(peer, HelloFrom("Bob", 0xB0B)));
		f.fake->Drop(peer);
	}
	f.fake->Connect(5);
	const auto asked = std::chrono::steady_clock::now();
	f.fake->Deliver(5, Protocol::Encode(HelloFrom("Bob", 0xB0B)));
	Packets toBob;
	CHECK(NoneYet(f, 1, 5, MT::kWelcome, toBob, asked, options.worldStateRefill));
	CHECK(f.Logged("says hello too often"));
	REQUIRE(Await(f, 5, MT::kWorldState, toBob));
	CHECK(Count(toBob, MT::kWelcome) == 1);

	// Someone else's game, on a connection of its own, isn't held up by Bob's budget.
	f.fake->Connect(6);
	f.fake->Deliver(6, Protocol::Encode(HelloFrom("Carol", 0xCA201)));
	CHECK(f.fake->WaitFor(6, Type(MT::kWorldState), 500ms));
}

// Bob's game restarts before Steam notices (it keeps one connection per friend) and loads a save, with his
// world-state budget spent: his hello waits for it, and Alice loots meanwhile. None of her loot may reach his
// new game before its Welcome: the game would count it as had, and then skip, in the hello's world state,
// the changes his save lacks. His chest comes out as the session has it.
TEST("budgets: hello again on the same connection with an empty budget is answered when it refills, and the loot made meanwhile isn't lost")
{
	Server::Options options;
	options.worldStateRefill = 1500ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto bob = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(bob);
	FriendGame game;
	game.me = bob->playerId;
	game.loaded = { CHEST };
	game.Take(Received(f, 2));
	Packets toAlice = Received(f, 1);

	f.fake->Deliver(1, Loot(CHEST, ITEM, 3));  // #0: in the save he makes now
	Sync(f, 1);
	game.Take(Received(f, 2));
	const auto savedWorld = game.world;
	const auto savedNext = game.pending.Next();
	REQUIRE(savedNext == 1);
	f.fake->Deliver(1, Loot(CHEST, ITEM, -1));  // #1: after it
	// He reloaded twice already: with the join, his budget is spent.
	f.fake->Deliver(2, Request(bob->sessionId, savedNext));
	f.fake->Deliver(2, Request(bob->sessionId, savedNext));
	Sync(f, 1);
	game.Take(Received(f, 2));
	REQUIRE(game.worldStates == 3);  // the join's and the two reloads'
	std::ranges::move(f.fake->Take(1), std::back_inserter(toAlice));

	// His game restarts, loads that save and says hello again on the same connection.
	game.Restart(savedWorld, savedNext, {});
	const auto asked = std::chrono::steady_clock::now();
	f.fake->Deliver(2, Protocol::Encode(HelloFrom("Bob", 0xB0B, bob->sessionId, savedNext)));
	f.fake->Deliver(1, Loot(CHEST, ITEM, -1));     // #2, while his hello waits
	f.fake->Deliver(1, Loot(CHEST, ITEM + 1, 2));  // #3
	Packets toBob;
	CHECK(NoneYet(f, 1, 2, MT::kWelcome, toBob, asked, options.worldStateRefill));
	CHECK(Count(toBob, MT::kContainerChanged) == 0);
	REQUIRE(Await(f, 2, MT::kWorldState, toBob));
	game.Take(toBob);
	CHECK(game.welcomes == 1);
	CHECK(game.beforeWelcome == 0);
	const auto welcome = Decoded(toBob, MT::kWelcome, Protocol::DecodeWelcome);
	REQUIRE(welcome.size() == 1);
	CHECK(welcome[0].playerId == bob->playerId);

	// His chest is as Alice's game was told it: every change once, #1 (which his save lacked) too.
	const auto afterRestart = Received(f, 1);
	std::ranges::copy(afterRestart, std::back_inserter(toAlice));
	CHECK(game.world.Of(CHEST) == SessionAsToldTo(toAlice).Of(CHEST));
	CHECK((game.world.Of(CHEST).at({ CHEST, ITEM }) == 1));
	CHECK(game.pending.Empty());
	CHECK(game.pending.Next() == 4);
	// Alice was told he left and came back.
	CHECK(Count(afterRestart, MT::kPlayerLeft) == 1);
	CHECK(Count(afterRestart, MT::kPlayerJoined) == 1);
}

// The honest margin: a friend who joins and dies twice in a row, loading his last save each time, gets each
// world state at once. One more reload straight after is owed, not lost: it comes when his budget refills,
// with everything his save lacks, and his chest ends up as the session has it.
TEST("budgets: a friend who joins and reloads three times in a row gets every world state, the last one when his budget refills")
{
	Server::Options options;
	options.worldStateRefill = 1000ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	const auto bob = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(bob);
	FriendGame game;
	game.me = bob->playerId;
	game.loaded = { CHEST };
	game.Take(Received(f, 2));
	const auto joined = game.worldStates;  // the join's
	Packets    toAlice = Received(f, 1);

	// Each time: he saves, Alice loots (he sees it live), he dies and loads that save.
	std::vector<std::chrono::steady_clock::time_point> askedAt;
	for (std::uint32_t reload = 0; reload < 3; ++reload) {
		const auto savedWorld = game.world;
		const auto savedNext = game.pending.Next();
		const auto savedEntries = game.pending.Sorted();
		f.fake->Deliver(1, Loot(CHEST, ITEM + reload, 2));
		Sync(f, 1);
		game.Take(Received(f, 2));
		game.LoadSave(savedWorld, savedNext, savedEntries);
		askedAt.push_back(std::chrono::steady_clock::now());
		f.fake->Deliver(2, Request(bob->sessionId, savedNext));
		Sync(f, 2);
		game.Take(Received(f, 2));
	}
	CHECK(game.worldStates - joined >= 2);  // the first two at once

	Packets toBob;
	if (game.worldStates - joined == 2) {
		REQUIRE(Await(f, 2, MT::kWorldState, toBob, 3000ms));
		// Within about one refill of asking (with room for a slow machine).
		CHECK(std::chrono::steady_clock::now() - askedAt.back() < options.worldStateRefill * 2);
		game.Take(toBob);
	}
	CHECK(game.worldStates - joined == 3);
	CHECK(Count(During(f, 2, 300ms), MT::kWorldState) == 0);  // once, not once per request

	std::ranges::move(Received(f, 1), std::back_inserter(toAlice));
	CHECK(game.world.Of(CHEST) == SessionAsToldTo(toAlice).Of(CHEST));
	CHECK(game.world.Of(CHEST).size() == 3);
	CHECK(game.pending.Empty());
}

// The owed world state and the live echoes never overlap or leave a gap: while Bob's is owed, the
// container changes (his own too) wait in it; after it they come live again, numbered on from it.
TEST("budgets: while a player's world state is owed, every container change reaches them once, in order")
{
	Server::Options options;
	options.worldStateRefill = 1000ms;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	const auto bob = f.Join(2, HelloFrom("Bob", 0xB0B));
	REQUIRE(bob);
	f.fake->Deliver(1, Loot(CHEST, ITEM, -1));  // #0, before his save
	f.fake->Deliver(2, Request(bob->sessionId, 1));
	f.fake->Deliver(2, Request(bob->sessionId, 1));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Request(bob->sessionId, 1));  // loaded his save again: owed
	Sync(f, 2);

	f.fake->Deliver(1, Loot(CHEST, ITEM + 1, -1));  // #1
	f.fake->Deliver(2, Loot(CHEST, ITEM + 2, 1));   // #2, his own
	f.fake->Deliver(1, Loot(CHEST, ITEM + 3, -1));  // #3
	Packets toBob = Received(f, 2);
	CHECK(Count(toBob, MT::kContainerChanged) == 0);
	CHECK(Count(Received(f, 1), MT::kContainerChanged) == 3);  // Alice isn't owed anything

	REQUIRE(Await(f, 2, MT::kWorldState, toBob));
	f.fake->Deliver(1, Loot(CHEST, ITEM + 4, -1));  // #4: live again
	std::ranges::move(Received(f, 2), std::back_inserter(toBob));
	const auto worlds = WorldStates(toBob);
	REQUIRE(worlds.size() == 1);
	CHECK(worlds[0].containerFirstIndex == 1);
	std::vector<std::uint32_t> order;
	for (const auto& change : worlds[0].containerChanges) {
		order.push_back(change.index);
	}
	for (const auto& change : Decoded(toBob, MT::kContainerChanged, Protocol::DecodeIndexedContainerChange)) {
		order.push_back(change.index);
	}
	CHECK((order == std::vector<std::uint32_t>{ 1, 2, 3, 4 }));
	CHECK(f.Logged("sent 'Bob' the world they asked for"));
}

// ---- other budgets kept across a reconnect --------------------------------------------------

TEST("budgets: shared XP spent before a rejoin isn't there again after it")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob", 0xB0B);
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Xp(Protocol::MAX_XP_SHARE));
	const auto first = Decoded(Received(f, 1), MT::kPartyXp, Protocol::DecodeXpGain);
	REQUIRE(first.size() == 1);
	CHECK(first[0].xp == Protocol::MAX_XP_SHARE);

	f.fake->Drop(2);
	JoinAs(f, 3, "Bob", 0xB0B);
	Clear(f, { 1 });
	f.fake->Deliver(3, Xp(Protocol::MAX_XP_SHARE));
	// Only what refilled in the moment between: a tenth of the budget a second, not all of it again.
	float again = 0.0f;
	for (const auto& gain : Decoded(Received(f, 1), MT::kPartyXp, Protocol::DecodeXpGain)) {
		again += gain.xp;
	}
	CHECK(again < Protocol::MAX_XP_SHARE / 4);
}

// F13: a barber visit changes a face several times in a few seconds. The others used to keep the first
// change for the rest of the session; now the last one follows when the interval is up.
TEST("budgets: a face changed again within the interval reaches the others as the latest one when it ends")
{
	Server::Options options;
	options.faceInterval = 600ms;
	Fixture f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Face(0.1f));
	f.fake->Deliver(1, Face(0.5f));
	f.fake->Deliver(1, Face(0.9f));
	auto faces = Decoded(Received(f, 2), MT::kPlayerFace, Protocol::DecodeFace);
	REQUIRE(faces.size() == 1);
	CHECK(faces[0].weight[0] == 0.1f);

	Packets toBob;
	REQUIRE(Await(f, 2, MT::kPlayerFace, toBob));
	faces = Decoded(toBob, MT::kPlayerFace, Protocol::DecodeFace);
	REQUIRE(faces.size() == 1);
	CHECK(faces[0].playerId == alice);
	CHECK(faces[0].weight[0] == 0.9f);
	CHECK(Count(During(f, 2, 800ms), MT::kPlayerFace) == 0);  // once
	CHECK(Count(Received(f, 1), MT::kPlayerFace) == 0);       // never to herself
}

// ---- shares of the session's tables ---------------------------------------------------------

// F10: one joiner reporting junk can no longer fill a table for everyone; the honest player's kills
// still go round.
TEST("shares: a joiner past their share of kills is logged once and no more of theirs are kept; the others' still are")
{
	Server::Options options;
	options.maxWorldEntriesPerPlayer = 5;
	Fixture    f{ options };
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Mallory");
	Clear(f, { 1, 2 });

	for (std::uint32_t i = 0; i < 8; ++i) {
		f.fake->Deliver(2, Death(0x0007F000 + i));
	}
	CHECK(Count(Received(f, 1), MT::kActorDied) == 5);
	CHECK(f.LogLines("'Mallory' has added 5 dead NPC entries this run").size() == 1);

	f.fake->Deliver(1, Death(0x00010001));
	const auto toMallory = Decoded(Received(f, 2), MT::kActorDied, Protocol::DecodeActorDeath);
	REQUIRE(toMallory.size() == 1);
	CHECK(toMallory[0].playerId == alice);

	// A newcomer's world has what was kept: five of hers and Alice's one.
	JoinAs(f, 3, "Carol");
	const auto worlds = WorldStates(Received(f, 3));
	REQUIRE(worlds.size() == 1);
	CHECK(worlds[0].deadActors.size() == 6);
}

TEST("shares: a junk flood of pickups doesn't stop an honest friend's being shared")
{
	Server::Options options;
	options.maxWorldEntries = 30;
	options.maxWorldEntriesPerPlayer = 10;
	Fixture f{ options };
	JoinAs(f, 1, "Mallory");
	JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	Clear(f, { 1, 2, 3 });
	for (std::uint32_t i = 0; i < 40; ++i) {
		f.fake->Deliver(1, Pickup(0x7F000001 + i));  // made-up IDs from a plugin index nobody has
	}
	CHECK(Count(Received(f, 2), MT::kRefPickedUp) == 10);
	for (std::uint32_t i = 0; i < 5; ++i) {
		f.fake->Deliver(2, Pickup(0x00040001 + i));
	}
	CHECK(Count(Received(f, 3), MT::kRefPickedUp) == 15);  // Mallory's ten, then all of Bob's
	CHECK(!f.Logged("% full"));
}

TEST("shares: the host's own game has no share limit")
{
	auto options = HostedOptions();
	options.maxWorldEntriesPerPlayer = 3;
	Fixture f{ options };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 0; i < 6; ++i) {
		f.fake->Deliver(1, Death(0x00010001 + i));
		f.fake->Deliver(1, Pickup(0x00040001 + i));
		f.fake->Deliver(1, Stage(QUEST + i, 10));
	}
	const auto toBob = Received(f, 2);
	CHECK(Count(toBob, MT::kActorDied) == 6);
	CHECK(Count(toBob, MT::kRefPickedUp) == 6);
	CHECK(Count(toBob, MT::kQuestStage) == 6);
	CHECK(!f.Logged("their share"));
}

// G13: the share goes by who the transport says they are, so a new player ID from a made-up identity
// doesn't bring a new one.
TEST("shares: a friend rejoining under a made-up identity is still at their share of loot")
{
	Server::Options options;
	options.maxContainerChangesPerPlayer = 5;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	f.fake->SetIdentity(2, BOB_STEAM);
	const auto first = JoinAs(f, 2, "Bob", 0xB01);
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 0; i < 5; ++i) {
		f.fake->Deliver(2, Loot(CHEST, ITEM + i, -1));
	}
	CHECK(Count(Received(f, 1), MT::kContainerChanged) == 5);

	f.fake->Drop(2);
	f.fake->SetIdentity(3, BOB_STEAM);
	const auto second = JoinAs(f, 3, "Bob", 0xB02);
	CHECK(second != first);  // a new player as far as the session knows
	Clear(f, { 1, 3 });
	f.fake->Deliver(3, Loot(CHEST, ITEM + 9, -1));
	CHECK(Count(Received(f, 1), MT::kContainerChanged) == 0);
	f.fake->Deliver(1, Loot(CHEST, ITEM + 9, -1));  // Alice's are hers
	CHECK(Count(Received(f, 3), MT::kContainerChanged) == 1);
}

TEST("shares: past their share of building, nothing new of theirs is kept, but what they built still moves and goes")
{
	Server::Options options;
	options.maxWorkshopItemsPerPlayer = 3;
	Fixture f{ options };
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 0; i < 4; ++i) {
		f.fake->Deliver(1, Build(0xFF000001 + i));
	}
	CHECK(Count(Received(f, 2), MT::kWorkshopItem) == 3);

	f.fake->Deliver(1, Build(0xFF000001, Protocol::WorkshopOp::kPlaced, 50.0f));  // moved
	f.fake->Deliver(1, Build(0xFF000002, Protocol::WorkshopOp::kScrapped));       // scrapped
	CHECK(Count(Received(f, 2), MT::kWorkshopItem) == 2);
	f.fake->Deliver(1, Build(0xFF000009));  // room again
	CHECK(Count(Received(f, 2), MT::kWorkshopItem) == 1);

	JoinAs(f, 3, "Carol");
	std::size_t kept = 0;
	for (const auto& batch : Decoded(Received(f, 3), MT::kWorkshopItems, Protocol::DecodeWorkshopItems)) {
		kept += batch.size();
	}
	CHECK(kept == 3);
}

// A friend's share of loot is part of the session (by their player ID), so it holds after the host
// quits and hosts again from the session file; the host's own game is still not limited.
TEST("shares: a friend's loot share holds after the host restarts from the session file")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-budget-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	auto options = HostedOptions();
	options.sessionFile = path;
	options.maxContainerChangesPerPlayer = 4;
	{
		Fixture f{ options };
		JoinAsHost(f, 1);
		JoinAs(f, 2, "Bob", 0xB0B);
		for (std::uint32_t i = 0; i < 4; ++i) {
			f.fake->Deliver(2, Loot(CHEST, ITEM + i, -1));
		}
		Clear(f, { 1, 2 });
	}  // stopping saves

	Fixture f{ options };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob", 0xB0B);  // his old player ID back
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Loot(CHEST, ITEM + 8, -1));
	CHECK(Count(Received(f, 1), MT::kContainerChanged) == 0);
	f.fake->Deliver(1, Loot(CHEST, ITEM + 8, 1));
	const auto echo = Decoded(Received(f, 2), MT::kContainerChanged, Protocol::DecodeIndexedContainerChange);
	REQUIRE(echo.size() == 1);
	CHECK(echo[0].index == 4);  // numbered on from the saved session
	std::filesystem::remove(path);
}

// F38: a table filling up used to fail without a word. Now the log says so near the end and at it.
TEST("tables: a session table is logged once at 80% and once full, and past full nothing more is shared")
{
	auto options = HostedOptions();
	options.maxWorldEntries = 10;
	Fixture f{ options };
	JoinAsHost(f, 1);
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 0; i < 12; ++i) {
		f.fake->Deliver(1, Death(0x00010001 + i));
	}
	CHECK(Count(Received(f, 2), MT::kActorDied) == 10);
	CHECK(f.LogLines("dead NPC table is 80% full (8/10); kill sharing stops at 100%").size() == 1);
	CHECK(f.LogLines("dead NPC table is 100% full (10/10)").size() == 1);
	CHECK(f.LogLines("% full").size() == 2);
}

TEST("tables: a saved session near full says so when the host starts again")
{
	const auto path = (std::filesystem::temp_directory_path() / std::format("f4mp-tables-{}.session", std::rand())).string();
	std::filesystem::remove(path);
	auto options = HostedOptions();
	options.sessionFile = path;
	options.maxWorldEntries = 10;
	{
		Fixture f{ options };
		JoinAsHost(f, 1);
		for (std::uint32_t i = 0; i < 7; ++i) {
			f.fake->Deliver(1, Pickup(0x00040001 + i));
		}
		Clear(f, { 1 });
		CHECK(!f.Logged("% full"));
	}
	{
		Fixture f{ options };
		JoinAsHost(f, 1);
		f.fake->Deliver(1, Pickup(0x00040100));
		Clear(f, { 1 });
		CHECK(f.LogLines("picked-up item table is 80% full (8/10)").size() == 1);
	}
	{
		Fixture f{ options };  // the next start: already that full
		REQUIRE(f.fake);
		std::this_thread::sleep_for(100ms);
		CHECK(f.LogLines("picked-up item table is 80% full (8/10)").size() == 1);
	}
	std::filesystem::remove(path);
}

// ---- NPC ownership --------------------------------------------------------------------------

// Counting a player's NPCs once a packet (F11) must still count an NPC they already run that becomes
// their companion in the same packet.
TEST("claims: an NPC a player runs that becomes their companion counts toward their four, within one packet")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Claims({ { 0x00020001, Reason::kUnowned },
						   { 0x00020001, Reason::kCompanion },
						   { 0x00020002, Reason::kCompanion },
						   { 0x00020003, Reason::kCompanion },
						   { 0x00020004, Reason::kCompanion },
						   { 0x00020005, Reason::kCompanion } }));
	std::set<std::uint32_t> granted;
	for (const auto& batch : Decoded(Received(f, 2), MT::kActorOwners, Protocol::DecodeOwners)) {
		for (const auto& owner : batch) {
			if (owner.playerId == alice) {
				granted.insert(owner.refId);
			}
		}
	}
	CHECK((granted == std::set<std::uint32_t>{ 0x00020001, 0x00020002, 0x00020003, 0x00020004 }));
	f.fake->Deliver(1, Claims({ { 0x00020006, Reason::kCompanion } }));
	CHECK(Count(Received(f, 2), MT::kActorOwners) == 0);
	f.fake->Deliver(1, Claims({ { 0x00020006, Reason::kUnowned } }));  // as an ordinary NPC she can run it
	CHECK(Count(Received(f, 2), MT::kActorOwners) == 1);
}
