// Random encounters shared calm (src/game/Encounters.h): which quests are encounters, the spawn's kind on the wire, a
// line said by a spawn, the server granting two games' actors of one encounter quest, and the book that stops our run
// of an encounter where a friend's already runs, from the server's order.

#include "ServerFixture.h"

#include "game/Encounters.h"

#include <array>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Encounters::Book;
	using Encounters::Theirs;
	using Protocol::SpawnKind;
	using RemoteEdits::Spot;

	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t WORLD = 0x0000003C;      // the Commonwealth
	constexpr std::uint32_t CELL = 0x0001F3A0;       // an interior
	constexpr std::uint32_t ACTOR_BASE = 0x0017E78C; // RESceneLC01's first actor's base
	constexpr std::uint32_t RAIDER = 0x0002898B;     // LvlRaider
	constexpr std::uint32_t MIRELURK = 0x001243FF;   // DN083_StartMirelurk
	constexpr std::uint32_t LC01 = 0x0017E729;       // RESceneLC01
	constexpr std::uint32_t KMK01 = 0x0002DFED;      // RETravelKMK01
	constexpr std::uint32_t DN083 = 0x001243D6;      // DN083_Barney, Gun Run (a quest actors' quest)
	constexpr std::uint32_t SPAWN = 0xFF001234;
	constexpr std::uint32_t SPAWN2 = 0xFF005678;
	constexpr std::uint32_t SPAWN3 = 0xFF009ABC;
	constexpr std::uint32_t OURS = 0xFF010001;       // our runtime NPC's id
	constexpr std::uint32_t OURS2 = 0xFF010002;

	Spot At(float a_x, float a_y = 2000.0f, std::uint32_t a_worldspace = WORLD)
	{
		return { 0, a_worldspace, a_x, a_y, 100.0f };
	}

	Protocol::Spawn Actor(std::uint32_t a_id, std::uint32_t a_quest, std::uint16_t a_alias, SpawnKind a_kind, std::uint32_t a_base = ACTOR_BASE)
	{
		return { 0, a_id, a_base, 0, WORLD, 1000.0f, 2000.0f, 100.0f, 0.5f, a_quest, a_alias, a_kind };
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

	// A friend's encounter spawn as the book sees it.
	Theirs Friends(std::uint32_t a_spawn, std::uint32_t a_quest, const Spot& a_at, std::uint64_t a_seq, bool a_over = false)
	{
		return { a_spawn, a_quest, 7, a_at, a_seq, a_over };
	}
}

TEST("encounters: the game's random-encounter quests are told by their name and type")
{
	using Encounters::IsEncounter;
	using Encounters::IsEncounterName;
	constexpr std::int8_t MISC = Encounters::QUEST_TYPE_MISC;
	// Fallout4.esm's, of every kind the RE system has.
	for (const auto name : { "RESceneLC01", "RETravelKMK01", "REObjectKMK02", "RECampLC01", "REChokepointSC01", "REAssaultSC06", "RECheckpoint02_SynthAttackers", "REScene_VertibirdKMK01", "RESceneKMK_MS09Jack" }) {
		CHECK(IsEncounterName(name));
		CHECK(IsEncounter(MISC, name));
	}
	// The add-ons', after their prefix.
	for (const auto name : { "DLC03RESceneKMK01", "DLC04RETravelCT01", "DLC01RESceneKMK02", "DLC03_REObjectKMK01" }) {
		CHECK(IsEncounterName(name));
		CHECK(IsEncounter(MISC, name));
	}
	// Other quests that happen to begin with the letters, and quests of other kinds.
	for (const auto name : { "ReconScopeQuest", "RelayTowerQuest", "Reunions", "RE", "R", "", "FFDiamondCity12", "DialogueDiamondCity", "DLC03Reunite" }) {
		CHECK(!IsEncounterName(name));
		CHECK(!IsEncounter(MISC, name));
	}
	// The encounter system's own bookkeeping is of type none or a side quest: no encounter.
	CHECK(!IsEncounter(0, "REParent"));
	CHECK(!IsEncounter(0, "RECorpseSwarmCreator"));
	CHECK(!IsEncounter(7, "RECheckpointBoSWon01_Init"));
	CHECK(!IsEncounter(7, "REAssault_VertibirdKMK01"));
	CHECK(!IsEncounter(1, "RESceneLC01"));
}

TEST("encounters: a spawn says its kind on the wire, and a plain one names no quest")
{
	for (const auto type : { MT::kReportSpawn, MT::kSpawned }) {
		const auto encounter = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter), type));
		REQUIRE(encounter);
		CHECK(encounter->kind == SpawnKind::kEncounter);
		CHECK(encounter->quest == LC01 && encounter->alias == 3);
		CHECK(*encounter == Actor(SPAWN, LC01, 3, SpawnKind::kEncounter));

		const auto questActor = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, DN083, 26, SpawnKind::kQuestActor, MIRELURK), type));
		REQUIRE(questActor);
		CHECK(questActor->kind == SpawnKind::kQuestActor);

		const auto plain = Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kPlain, RAIDER), type));
		REQUIRE(plain);
		CHECK(plain->kind == SpawnKind::kPlain && plain->quest == 0 && plain->alias == 0);

		// A plain spawn naming a quest, a kind naming none, and a kind the protocol has not: refused.
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, SpawnKind::kPlain), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kEncounter), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, 0, 0, SpawnKind::kQuestActor), type)));
		CHECK(!Protocol::DecodeSpawn(Protocol::Encode(Actor(SPAWN, LC01, 3, static_cast<SpawnKind>(3)), type)));
		// The default is plain: a spawn made with no kind said is as before.
		CHECK(Protocol::Spawn{}.kind == SpawnKind::kPlain);
	}
}

TEST("encounters: a line may be said by a spawn, under its spawn id")
{
	for (const auto type : { MT::kReportLine, MT::kLineSpoken }) {
		const auto line = Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, SPAWN, "Hello there", 0x0017E7A0, 1, 0x0002F7A1 }, type));
		REQUIRE(line);
		CHECK(line->speaker == SPAWN);
		CHECK(line->info == 0x0017E7A0 && line->response == 1 && line->voiceType == 0x0002F7A1);
		// A plugin's NPC and the player's own still go; the one id no NPC has is refused.
		CHECK(Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0x0001F001, "Hello" }, type)));
		CHECK(Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0, "Hello" }, type)));
		CHECK(!Protocol::DecodeLine(Protocol::Encode(Protocol::Line{ 2, 0xFF000000, "Hello" }, type)));
	}
}

TEST("encounters: the server grants two games' actors of one encounter quest and alias, and still refuses a quest actor's twin")
{
	Fixture    f;
	const auto first = JoinAs(f, 1, "Player One");
	const auto second = JoinAs(f, 2, "Player Two");
	Clear(f, { 1, 2 });

	// The first game rolled RESceneLC01 and shares an actor of it; the second rolled the same encounter (elsewhere, or
	// at the same spot: the games settle that, not the server) and shares its own from the same alias. Both are granted.
	f.fake->Deliver(1, Report(Actor(SPAWN, LC01, 3, SpawnKind::kEncounter)));
	const auto toSecond = Spawned(Received(f, 2));
	REQUIRE(toSecond.size() == 1);
	CHECK(toSecond[0].kind == SpawnKind::kEncounter && toSecond[0].quest == LC01 && toSecond[0].alias == 3 && toSecond[0].playerId == first);
	Clear(f, { 1 });
	f.fake->Deliver(2, Report(Actor(SPAWN2, LC01, 3, SpawnKind::kEncounter)));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN2, second));
	const auto toFirst = Spawned(Received(f, 1));
	REQUIRE(toFirst.size() == 1);
	CHECK(toFirst[0].kind == SpawnKind::kEncounter && toFirst[0].playerId == second);

	// A quest actor's twin is refused as before (protocol 61).
	f.fake->Deliver(1, Report(Actor(SPAWN3, DN083, 26, SpawnKind::kQuestActor, MIRELURK)));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Report(Actor(0xFF00DEF0, DN083, 26, SpawnKind::kQuestActor, MIRELURK)));
	const auto refused = Owners(Received(f, 2));
	CHECK(HasOwner(refused, 0xFF00DEF0, 0));
	CHECK(!HasOwner(refused, 0xFF00DEF0, second));
	CHECK(Spawned(Received(f, 1)).empty());

	// A joiner hears the kind of each spawn already shared.
	Sync(f, 1);
	JoinAs(f, 3, "Player Three");
	const auto toThird = Spawned(Received(f, 3));
	REQUIRE(toThird.size() == 3);
	CHECK(std::ranges::count_if(toThird, [](const auto& a_spawn) { return a_spawn.kind == SpawnKind::kEncounter; }) == 2);
	CHECK(std::ranges::count_if(toThird, [](const auto& a_spawn) { return a_spawn.kind == SpawnKind::kQuestActor; }) == 1);
}

TEST("encounters: a friend's encounter that was first at the spot stops our run begun there")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	// Our run of RESceneLC01: its two actors seen, no grant yet.
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	book.OursSeen(LC01, OURS2, At(1100.0f), now);
	CHECK(book.Knows(LC01));
	CHECK(!book.Knows(KMK01));

	// A friend's actor of another encounter 500 units away, which came before any grant of ours: ours gives way.
	const std::array near{ Friends(SPAWN, KMK01, At(1500.0f), 1) };
	const auto       rival = book.RivalOf(LC01, near, now + 1s);
	REQUIRE(rival);
	CHECK(rival->spawnId == SPAWN && rival->quest == KMK01 && rival->player == 7);
	CHECK(rival->distance > 399.0f && rival->distance < 401.0f);  // from the nearer of ours

	// The same encounter rolled by both at one trigger is settled the same way.
	const std::array same{ Friends(SPAWN, LC01, At(1200.0f), 1) };
	CHECK(book.RivalOf(LC01, same, now + 1s));

	// Far away, in another place, or over (dead or let go of): no rival.
	const std::array far{ Friends(SPAWN, KMK01, At(1100.0f + Encounters::COLLIDE_RANGE + 1.0f), 1) };  // past the range of both of ours
	CHECK(!book.RivalOf(LC01, far, now + 1s));
	const std::array elsewhere{ Friends(SPAWN, KMK01, Spot{ CELL, 0, 1000.0f, 2000.0f, 100.0f }, 1) };
	CHECK(!book.RivalOf(LC01, elsewhere, now + 1s));
	const std::array otherWorld{ Friends(SPAWN, KMK01, At(1000.0f, 2000.0f, 0x0000DDDD), 1) };
	CHECK(!book.RivalOf(LC01, otherWorld, now + 1s));
	const std::array over{ Friends(SPAWN, KMK01, At(1500.0f), 1, true) };
	CHECK(!book.RivalOf(LC01, over, now + 1s));
	const std::array noQuest{ Friends(SPAWN, 0, At(1500.0f), 1) };
	CHECK(!book.RivalOf(LC01, noQuest, now + 1s));

	// A run the book doesn't know, and nothing of theirs at all.
	CHECK(!book.RivalOf(KMK01, near, now + 1s));
	CHECK(!book.RivalOf(LC01, std::span<const Theirs>{}, now + 1s));
}

TEST("encounters: the server's order decides, so two games decide alike")
{
	// What the server did: granted game A's actor first (its kActorOwners to both, kSpawned to B), then game B's.
	// Game A: its own grant arrives first, then B's spawn.
	Book       a;
	const auto now = Encounters::Clock::now();
	a.OursSeen(LC01, OURS, At(1000.0f), now);
	a.OursGranted(LC01, a.Next());                           // 1
	const std::array bSpawn{ Friends(SPAWN2, KMK01, At(1300.0f), a.Next()) };  // 2
	CHECK(!a.RivalOf(LC01, bSpawn, now + 2s));  // ours was first: it stays

	// Game B: A's spawn arrives first, then its own grant.
	Book b;
	b.OursSeen(KMK01, OURS, At(1300.0f), now);
	const std::array aSpawn{ Friends(SPAWN, LC01, At(1000.0f), b.Next()) };  // 1
	b.OursGranted(KMK01, b.Next());                                            // 2
	const auto rival = b.RivalOf(KMK01, aSpawn, now + 2s);
	REQUIRE(rival);
	CHECK(rival->quest == LC01);

	// A later grant of a second actor of the run doesn't move the run's place in the order; an earlier one would.
	b.OursGranted(KMK01, 9);
	CHECK(b.RivalOf(KMK01, aSpawn, now + 2s));
	a.OursGranted(LC01, 5);
	CHECK(!a.RivalOf(LC01, bSpawn, now + 2s));
	Book c;
	c.OursSeen(KMK01, OURS, At(1300.0f), now);
	c.OursGranted(KMK01, 3);
	const std::array laterSpawn{ Friends(SPAWN, LC01, At(1000.0f), 4) };
	CHECK(!c.RivalOf(KMK01, laterSpawn, now + 2s));
	c.OursGranted(KMK01, 5);  // the lowest is kept
	CHECK(!c.RivalOf(KMK01, laterSpawn, now + 2s));
}

TEST("encounters: past the window ours stays, a stopped run is left alone, and old runs are forgotten")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	const std::array theirs{ Friends(SPAWN, KMK01, At(1200.0f), 1) };
	CHECK(book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW - 1s));
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW));
	// Seen again later: the run's start is still its first sighting.
	book.OursSeen(LC01, OURS, At(1000.0f), now + Encounters::STOP_WINDOW + 5s);
	CHECK(!book.RivalOf(LC01, theirs, now + Encounters::STOP_WINDOW + 6s));
	CHECK(book.StartedAt(LC01) == now);

	// Stopped: left alone for STOPPED_FOR, and its actors seen again in that time don't make a new run.
	Book stopped;
	stopped.OursSeen(LC01, OURS, At(1000.0f), now);
	stopped.Stopped(LC01, now + 1s);
	CHECK(stopped.IsStopped(LC01, now + 1s));
	CHECK(!stopped.Knows(LC01));
	stopped.OursSeen(LC01, OURS, At(1000.0f), now + 2s);
	CHECK(stopped.IsStopped(LC01, now + 2s));
	CHECK(!stopped.RivalOf(LC01, theirs, now + 3s));
	CHECK(!stopped.IsStopped(LC01, now + 1s + Encounters::STOPPED_FOR));
	CHECK(!stopped.IsStopped(KMK01, now + 1s));
	stopped.Expire(now + 1s + Encounters::STOPPED_FOR);
	CHECK(!stopped.IsStopped(LC01, now + 2s));  // forgotten: the quest may run here again

	// A run none of whose actors has been seen for FORGET_AFTER is forgotten; one seen lately is kept.
	Book old;
	old.OursSeen(LC01, OURS, At(1000.0f), now);
	old.OursSeen(KMK01, OURS2, At(5000.0f), now + Encounters::FORGET_AFTER - 1s);
	old.Expire(now + Encounters::FORGET_AFTER);
	CHECK(!old.Knows(LC01));
	CHECK(old.Knows(KMK01));
	CHECK(old.Size() == 1);
	old.Clear();
	CHECK(old.Size() == 0);
}

TEST("encounters: the nearest of several rivals, and a run's actors are kept to a limit")
{
	Book       book;
	const auto now = Encounters::Clock::now();
	book.OursSeen(LC01, OURS, At(1000.0f), now);
	const std::array theirs{ Friends(SPAWN, KMK01, At(2500.0f), 1), Friends(SPAWN2, DN083, At(1800.0f), 2), Friends(SPAWN3, KMK01, At(1200.0f), 3) };
	const auto       rival = book.RivalOf(LC01, theirs, now + 1s);
	REQUIRE(rival);
	CHECK(rival->spawnId == SPAWN3);
	CHECK(rival->distance > 199.0f && rival->distance < 201.0f);

	// A friend's spawn that came after our grant is no rival even when nearest.
	book.OursGranted(LC01, 3);
	const auto earlier = book.RivalOf(LC01, theirs, now + 1s);
	REQUIRE(earlier);
	CHECK(earlier->spawnId == SPAWN2);

	// Actors past MAX_ACTORS are not kept, so a crowd doesn't grow the book; a known actor's spot is moved.
	Book crowd;
	const auto spread = [](std::uint32_t a_i) { return At(100000.0f + static_cast<float>(a_i) * 2.0f * Encounters::COLLIDE_RANGE); };  // each out of the others' range
	for (std::uint32_t i = 0; i < Encounters::MAX_ACTORS + 8; ++i) {
		crowd.OursSeen(KMK01, 0xFF020000 + i, spread(i), now);
	}
	crowd.OursSeen(KMK01, 0xFF020000, At(1000.0f), now);  // the first actor walked here
	const std::array nearFirst{ Friends(SPAWN, LC01, At(1200.0f), 1) };
	const auto       moved = crowd.RivalOf(KMK01, nearFirst, now + 1s);
	REQUIRE(moved);
	CHECK(moved->distance > 199.0f && moved->distance < 201.0f);
	const std::array nearKept{ Friends(SPAWN, LC01, spread(Encounters::MAX_ACTORS - 1), 1) };
	CHECK(crowd.RivalOf(KMK01, nearKept, now + 1s));  // the last actor within the limit was noted
	const std::array nearLast{ Friends(SPAWN, LC01, spread(Encounters::MAX_ACTORS + 7), 1) };
	CHECK(!crowd.RivalOf(KMK01, nearLast, now + 1s));  // an actor past the limit was never noted
}
