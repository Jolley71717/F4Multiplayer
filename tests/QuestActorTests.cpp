// Creatures a quest's alias creates in every game's run of it (Salem's mirelurks in Gun Run), paired across games
// (src/game/QuestActors.h): which quests and NPCs, when a pairing is made and ends, the wire, and the server's refusal of a
// second game's report of the same alias.

#include "ServerFixture.h"

#include "game/QuestActors.h"
#include "game/SpawnBook.h"

#include <array>

using namespace TestServer;

namespace
{
	using namespace QuestActors;
	using RemoteEdits::Spot;

	constexpr std::uint32_t WORLD = 0x0000003C;      // the Commonwealth
	constexpr std::uint32_t DN083 = 0x001243D6;      // DN083_Barney, Gun Run (misc, runs once)
	constexpr std::uint32_t MIRELURK = 0x001243FF;   // DN083_StartMirelurk (unique)
	constexpr std::uint32_t SPAWN = 0xFF001234;
	constexpr std::uint32_t SPAWN2 = 0xFF005678;
	constexpr std::uint32_t SPAWN3 = 0xFF009ABC;
	constexpr std::uint32_t HUMAN_RACE = 0x50A08943;     // HumanRace's flags in Fallout4.esm
	constexpr std::uint32_t MIRELURK_RACE = 0x52008148;  // MirelurkRace's
	constexpr std::uint32_t ANY_RACE_FORM = 0x00001234;  // a race form that is not the vertibird's
	constexpr std::uint32_t VERTIBIRD_FLAGS = 0;         // VertibirdRace allows no PC dialogue either

	// DN083_Barney as the game has it running.
	Quest GunRun()
	{
		return { .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC };
	}

	Spot At(float a_x, std::uint32_t a_worldspace = WORLD)
	{
		return { 0, a_worldspace, a_x, 2000.0f, 100.0f };
	}

	Protocol::Spawn Mirelurk(std::uint32_t a_id, std::uint16_t a_alias, std::uint32_t a_quest = DN083)
	{
		return { 0, a_id, MIRELURK, 0, WORLD, 1000.0f, 2000.0f, 100.0f, 0.5f, a_quest, a_alias, a_quest != 0 ? Protocol::SpawnKind::kQuestActor : Protocol::SpawnKind::kPlain };
	}

	auto Report(const Protocol::Spawn& a_spawn)
	{
		return Protocol::Encode(a_spawn, MT::kReportSpawn);
	}

	std::vector<Protocol::Spawn> Spawned(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		return Decoded(a_packets, MT::kSpawned, [](auto a_data) { return Protocol::DecodeSpawn(a_data); });
	}

	std::vector<Protocol::ActorOwner> Owners(const std::vector<std::vector<std::uint8_t>>& a_packets)
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

	PairFacts Pairable()
	{
		return { .keyed = true, .questPairs = true, .ours = true, .creature = true, .oursAt = At(1100.0f), .theirsAt = At(1000.0f), .player = At(1500.0f) };
	}
}

TEST("quest actors: a quest pairs its creatures only while the session shares it")
{
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC }));
	CHECK(!QuestPairs({ .shared = false, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC }));  // sQuests = story, or alone
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_SIDE }));    // a side quest
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_NONE }));
	for (const std::int8_t type : std::array<std::int8_t, 6>{ 1, 2, 3, 4, 5, 8 }) {  // the main quest's, the factions', an add-on's story: never, even shared
		CHECK(!QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = type }));
	}
	CHECK(!QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC, .radiant = true }));
	CHECK(!QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC, .perGame = true }));
}

TEST("quest actors: a quest pairs its creatures only while it runs, and only one that runs once or starts with the game")
{
	static_assert(QUEST_RUNNING == 0x0001 && QUEST_STARTS_ENABLED == 0x0010 && QUEST_RUN_ONCE == 0x0100);  // RE::QuestFlag
	const auto with = [](std::uint16_t a_flags) { return Quest{ .shared = true, .official = true, .flags = a_flags, .type = QUEST_TYPE_MISC }; };
	CHECK(QuestPairs(with(0x0101)));   // running, runs once (Gun Run)
	CHECK(QuestPairs(with(0x0011)));   // running, starts with the game (MS13, DN054): never started again
	CHECK(QuestPairs(with(0x0111)));   // both
	CHECK(!QuestPairs(with(0x0001)));  // running only: a second run's alias would be the first's on the wire
	CHECK(!QuestPairs(with(0x0110)));  // not running here
	CHECK(!QuestPairs(with(0x0000)));
}

TEST("quest actors: only a shared, running, run-once quest of the game's own of no story or faction line pairs its creatures")
{
	CHECK(QuestPairs(GunRun()));
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_NONE }));
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_SIDE }));
	CHECK(!QuestPairs({ .official = true, .flags = 0x0101, .type = QUEST_TYPE_MISC }));  // a misc quest the session doesn't share
	auto quest = GunRun();
	quest.official = false;  // a mod's
	CHECK(!QuestPairs(quest));
	quest = GunRun();
	quest.flags &= ~QUEST_RUNNING;  // stopped, or not started here
	CHECK(!QuestPairs(quest));
	quest = GunRun();
	quest.flags &= ~(QUEST_RUN_ONCE | QUEST_STARTS_ENABLED);  // started again and again, each run its own
	CHECK(!QuestPairs(quest));
	for (std::int8_t type = 1; type <= 5; ++type) {  // the main quest's and the factions'
		quest = GunRun();
		quest.type = type;
		CHECK(!QuestPairs(quest));
	}
	for (std::int8_t type = 8; type <= 14; ++type) {  // the add-ons' stories
		quest = GunRun();
		quest.type = type;
		CHECK(!QuestPairs(quest));
	}
	quest = GunRun();
	quest.radiant = true;  // RadiantSync shares it by its run
	CHECK(!QuestPairs(quest));
	quest = GunRun();
	quest.perGame = true;  // each game fills it its own way
	CHECK(!QuestPairs(quest));
}

TEST("quest actors: a creature its alias created is named with the alias; a talker, or one the alias found, is not")
{
	CHECK(Keyed(GunRun(), FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 26));
	CHECK(!Keyed(GunRun(), FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 25));      // Barney: our player talks to him, his scenes are each game's
	CHECK(!Keyed(GunRun(), 0, MIRELURK_RACE, ANY_RACE_FORM, 26));             // filled by conditions: the plugin's, mirrored already
	CHECK(!Keyed(GunRun(), 1, MIRELURK_RACE, ANY_RACE_FORM, 26));             // forced
	CHECK(!Keyed(GunRun(), FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 0x10000));  // past what the wire holds
	auto main = GunRun();
	main.type = 1;
	CHECK(!Keyed(main, FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 26));
	static_assert((HUMAN_RACE & RACE_ALLOWS_DIALOGUE) != 0 && (MIRELURK_RACE & RACE_ALLOWS_DIALOGUE) == 0);
}

TEST("quest actors: a vertibird a shared quest's alias created is not named with the alias (the player sits in it)")
{
	static_assert((VERTIBIRD_FLAGS & RACE_ALLOWS_DIALOGUE) == 0);
	CHECK(!Keyed(GunRun(), FILL_CREATED, VERTIBIRD_FLAGS, VERTIBIRD_RACE, 26));
	CHECK(Keyed(GunRun(), FILL_CREATED, VERTIBIRD_FLAGS, ANY_RACE_FORM, 26));  // the same flags in another race still pair
}

TEST("quest actors: ours from the same alias is paired with a friend's spawn near it, alive, and nobody's yet")
{
	const float range = SpawnBook::PLACE_RANGE;
	CHECK(Pairs(Pairable(), range));
	auto facts = Pairable();
	facts.keyed = false;  // a spawn like any other
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.over = true;  // dead, or let go of
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.questPairs = false;  // our run of it stopped, or never started
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.ours = false;  // the alias holds none of ours (dead, unloaded, a puppet)
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.creature = false;
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.shared = true;  // ours already: the server granted it to us
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.oursAt = At(1000.0f + PAIR_RANGE + 1.0f);
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.oursAt = At(1000.0f, 0x00000099);  // in another space
	CHECK(!Pairs(facts, range));
	facts = Pairable();
	facts.player = At(1000.0f + range + 1.0f);  // our player far from it: not loaded for us to fight
	facts.oursAt = At(1000.0f);
	CHECK(!Pairs(facts, range));
}

TEST("quest actors: a pairing lasts while ours is there and near, through its death, and ends when their game lets a living one go")
{
	const float keep = SpawnBook::KEEP_RANGE;
	KeepFacts facts{ .there = true, .loaded = true, .oursAt = At(1000.0f), .player = At(1500.0f) };
	CHECK(Decide(facts, keep) == Keep::kKeep);
	facts.dead = true;
	CHECK(Decide(facts, keep) == Keep::kKeep);
	facts.gone = true;  // dead and let go of: its corpse stays paired while near
	CHECK(Decide(facts, keep) == Keep::kKeep);
	facts.dead = false;  // alive and let go of (their player left): ours runs here again
	CHECK(Decide(facts, keep) == Keep::kUnpair);
	facts.gone = false;
	facts.loaded = false;
	CHECK(Decide(facts, keep) == Keep::kUnpair);
	facts.loaded = true;
	facts.there = false;
	CHECK(Decide(facts, keep) == Keep::kUnpair);
	facts.there = true;
	facts.player = At(1000.0f + keep + 1.0f);
	CHECK(Decide(facts, keep) == Keep::kUnpair);
}

TEST("quest actors: a friend's live spawn from the alias is its twin; a dead one, another alias or quest is not")
{
	const auto spawn = Mirelurk(SPAWN, 26);
	CHECK(Twin(spawn, false, DN083, 26));
	CHECK(!Twin(spawn, true, DN083, 26));
	CHECK(!Twin(spawn, false, DN083, 38));
	CHECK(!Twin(spawn, false, 0x0012AAAA, 26));
	CHECK(!Twin(spawn, false, 0, 0));
	CHECK(!Twin(Mirelurk(SPAWN, 0, 0), false, 0, 0));  // no alias named: no twin of an unkeyed one either
}

TEST("quest actors: a spawn carries its quest alias, and one naming an alias of no plugin's quest does not read")
{
	const auto spawn = Mirelurk(SPAWN, 26);
	CHECK(Protocol::DecodeSpawn(Report(spawn)) == spawn);
	auto bad = spawn;
	bad.quest = 0;  // an alias of no quest
	CHECK(!Protocol::DecodeSpawn(Report(bad)));
	bad.alias = 0;
	CHECK(!Protocol::DecodeSpawn(Report(bad)));  // a quest actor with no quest named
	bad.kind = Protocol::SpawnKind::kPlain;
	CHECK(Protocol::DecodeSpawn(Report(bad)));  // no alias named: a spawn like any other
	bad = spawn;
	bad.quest = 0xFF000801;  // a quest made at runtime: no other game has it
	CHECK(!Protocol::DecodeSpawn(Report(bad)));
	bad = spawn;
	bad.alias = 0;  // alias 0 of a quest is one
	CHECK(Protocol::DecodeSpawn(Report(bad)) == bad);
}

TEST("quest actors: the server refuses a second game's report of a quest alias another game shares, and grants it once that one is over")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(Mirelurk(SPAWN, 26)));
	const auto toBob = Received(f, 2);
	const auto spawned = Spawned(toBob);
	REQUIRE(spawned.size() == 1);
	CHECK(spawned[0].quest == DN083 && spawned[0].alias == 26 && spawned[0].playerId == alice);
	Clear(f, { 1 });

	// Bob's game made its own from the same alias and reported it before it heard of hers: refused, naming nobody.
	f.fake->Deliver(2, Report(Mirelurk(SPAWN2, 26)));
	const auto refused = Owners(Received(f, 2));
	CHECK(HasOwner(refused, SPAWN2, 0));
	CHECK(!HasOwner(refused, SPAWN2, bob));
	CHECK(Spawned(Received(f, 1)).empty());

	// Another alias of the quest, or the same id as no alias, is his to have.
	f.fake->Deliver(2, Report(Mirelurk(SPAWN2, 38)));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN2, bob));
	const auto toAlice = Spawned(Received(f, 1));
	REQUIRE(toAlice.size() == 1);
	CHECK(toAlice[0].alias == 38);

	// Alice's game reporting the alias again (a new id: hers is still live) is not refused for it: only another game's is.
	f.fake->Deliver(1, Report(Mirelurk(SPAWN3, 26)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN3, alice));

	// Hers over (let go of), Bob's report of the alias is granted.
	f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ SPAWN, SPAWN3 }));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Report(Mirelurk(0xFF00DEF0, 26)));
	CHECK(HasOwner(Owners(Received(f, 2)), 0xFF00DEF0, bob));
}

TEST("quest actors: a joiner hears the quest alias of a spawn already shared")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	f.fake->Deliver(1, Report(Mirelurk(SPAWN, 41)));
	Sync(f, 1);
	JoinAs(f, 2, "Bob");
	const auto spawned = Spawned(Received(f, 2));
	REQUIRE(spawned.size() == 1);
	CHECK(spawned[0].playerId == alice && spawned[0].quest == DN083 && spawned[0].alias == 41);
}
