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
}

TEST("quest actors: a quest pairs its creatures only while the session shares it")
{
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC }));
	CHECK(!QuestPairs({ .shared = false, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC }));  // sQuests = story, or alone
	CHECK(QuestPairs({ .shared = false, .official = true, .flags = 0x0511, .type = QUEST_TYPE_MISC }, true));  // ... but an enemy pairs: only the fight is shared
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_SIDE }));    // a side quest
	CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_NONE }));
	for (const std::int8_t type : std::array<std::int8_t, 6>{ 1, 2, 3, 4, 5, 8 }) {  // the main quest's, the factions', an add-on's story: never, even shared...
		CHECK(!QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = type }));
		CHECK(QuestPairs({ .shared = true, .official = true, .flags = 0x0511, .type = type }, true));  // ... but for an enemy
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

TEST("quest actors: a repeatable quest QuestFilter::RUNS_ONCE names pairs as one that runs once, while it runs")
{
	// A quest like DN143, Vault 75: running, with no run-once or start-up flag.
	Quest vault75{ .shared = true, .official = true, .flags = 0x0001, .type = QUEST_TYPE_MISC };
	CHECK(!QuestPairs(vault75));  // as the flags alone read it
	vault75.runsOnce = true;      // QuestFilter::IsRunsOnce(0, 0x0FCB15), as Spawns fills it
	CHECK(QuestPairs(vault75));
	vault75.flags = 0x0000;  // not running here
	CHECK(!QuestPairs(vault75));
	vault75.flags = 0x0001;
	vault75.shared = false;  // the session doesn't share it
	CHECK(!QuestPairs(vault75));
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
	CHECK(Keyed(GunRun(), 1, MIRELURK_RACE, ANY_RACE_FORM, 26, true));        // ... but a runtime enemy the quest's script forced into it is
	CHECK(Keyed(GunRun(), 0, HUMAN_RACE, ANY_RACE_FORM, 26, true));           // ... whatever the fill
	CHECK(!Keyed(GunRun(), FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 0x10000));  // past what the wire holds
	auto main = GunRun();
	main.type = 1;
	CHECK(!Keyed(main, FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 26));
	static_assert((HUMAN_RACE & RACE_ALLOWS_DIALOGUE) != 0 && (MIRELURK_RACE & RACE_ALLOWS_DIALOGUE) == 0);
}

TEST("quest actors: an enemy a story's or a faction's quest created is named with the alias, a human too; a calm one is not")
{
	// When Freedom Calls (Min00, 001A001C): a Minutemen quest (type 4), running, runs once; its waves of raiders in Concord's
	// street are humans its aliases 20 to 22 hold (made by another quest: the test below), hostile to the player (a Nexus
	// report on 0.21.0: each game fought its own).
	const Quest min00{ .shared = true, .official = true, .flags = 0xAD21, .type = 4 };
	CHECK(!QuestPairs(min00));                                            // as before, for one that is no enemy
	CHECK(QuestPairs(min00, true));                                       // an enemy's quest, of any line
	CHECK(Keyed(min00, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	CHECK(!Keyed(min00, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20));    // a calm human of a story's quest stays each game's own
	CHECK(!Keyed(min00, FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 20)); // ... and a calm creature of one, as before
	for (const std::int8_t type : std::array<std::int8_t, 7>{ 1, 2, 3, 4, 5, 8, 14 }) {  // the main quest's, the factions', an add-on's story
		auto quest = min00;
		quest.type = type;
		CHECK(Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	}
	// A misc or side quest's human enemy pairs too; its talker (Gun Run's Barney, calm) does not.
	CHECK(Keyed(GunRun(), FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 25, true));
	CHECK(!Keyed(GunRun(), FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 25));
}

TEST("quest actors: an enemy is paired only under the quest's other guards, and never in a vertibird")
{
	const Quest min00{ .shared = true, .official = true, .flags = 0xAD21, .type = 4 };
	auto        quest = min00;
	quest.shared = false;  // the session doesn't share it (sQuests = story, sStory = own): the fight is shared all the same
	CHECK(Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	CHECK(!Keyed(quest, FILL_CREATED, MIRELURK_RACE, ANY_RACE_FORM, 20));  // ... a calm creature of it is not
	quest = min00;
	quest.official = false;  // a mod's
	CHECK(!Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	quest = min00;
	quest.flags &= ~QUEST_RUNNING;  // over, or not started here
	CHECK(!Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	quest = min00;
	quest.flags &= ~(QUEST_RUN_ONCE | QUEST_STARTS_ENABLED);  // started again and again: a second run's alias would be the first's
	CHECK(!Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	quest = min00;
	quest.radiant = true;
	CHECK(!Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	quest = min00;
	quest.perGame = true;
	CHECK(!Keyed(quest, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 20, true));
	CHECK(Keyed(min00, 0, HUMAN_RACE, ANY_RACE_FORM, 5, true));                     // a runtime raider its script put in the alias (a placed one is never asked)
	CHECK(!Keyed(min00, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 0x10000, true));    // past what the wire holds
	CHECK(!Keyed(min00, FILL_CREATED, VERTIBIRD_FLAGS, VERTIBIRD_RACE, 20, true));   // a hostile vertibird: still never
}

TEST("quest actors: a vertibird a shared quest's alias created is not named with the alias (the player sits in it)")
{
	static_assert((VERTIBIRD_FLAGS & RACE_ALLOWS_DIALOGUE) == 0);
	CHECK(!Keyed(GunRun(), FILL_CREATED, VERTIBIRD_FLAGS, VERTIBIRD_RACE, 26));
	CHECK(Keyed(GunRun(), FILL_CREATED, VERTIBIRD_FLAGS, ANY_RACE_FORM, 26));  // the same flags in another race still pair
}

TEST("quest actors: of our twins of a friend's spawn, the nearest ready one that nobody's spawn holds pairs with it")
{
	const Candidate ready{ .twin = true, .ready = true, .at = At(1100.0f) };
	CHECK(Pick(std::array{ ready }, At(1000.0f)) == 0);
	auto c = ready;
	c.twin = false;  // not its twin (another alias, another NPC, or too far from where theirs was shared)
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	c = ready;
	c.ready = false;  // dead, switched off, unloaded, a puppet or our companion
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	c = ready;
	c.paired = true;  // paired with another friend's spawn already: one to one
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	c = ready;
	c.shared = true;  // ours shared, and it keeps its spawn (theirs lets go)
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	c.yields = true;  // ... or lets go of it for theirs
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 0);
	c = ready;
	c.at = At(1000.0f + PAIR_RANGE + 1.0f);
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	c.at = At(1000.0f, 0x00000099);  // in another space
	CHECK(Pick(std::array{ c }, At(1000.0f)) == 1);
	// A collection's members: the nearest free one, then the next for the next spawn.
	auto far = ready;
	far.at = At(1600.0f);
	auto taken = ready;
	taken.at = At(1010.0f);
	taken.paired = true;
	CHECK(Pick(std::array{ far, ready, taken }, At(1000.0f)) == 1);
	CHECK(Pick(std::array{ far, taken }, At(1000.0f)) == 0);
}

TEST("quest actors: a friend's spawn of an alias that keys ours is its twin; a loose alias's only of the same NPC nearby")
{
	const Own    ours{ .enemy = true, .firstAt = At(1100.0f), .at = At(1100.0f), .base = 0x0002898B, .original = 0x0002898B };
	const Theirs spawn{ .keyResolves = true, .sharedAt = At(1000.0f), .at = At(1000.0f), .base = 0x0002898B };
	CHECK(KeyTwin(ours, spawn, true, false));
	CHECK(!KeyTwin(ours, spawn, false, false));  // the alias does not hold ours, or does not key it
	auto over = spawn;
	over.over = true;
	CHECK(!KeyTwin(ours, over, true, false));
	// A created single alias pairs whatever each game rolled; a collection or a script's alias only the same NPC nearby.
	auto other = spawn;
	other.base = 0x00012345;
	CHECK(KeyTwin(ours, other, true, false));
	CHECK(!KeyTwin(ours, other, true, true));
	other.original = 0x0002898B;  // their roll of the same leveled NPC
	CHECK(KeyTwin(ours, other, true, true));
	auto apart = spawn;
	apart.at = At(1100.0f + PAIR_RANGE + 1.0f);
	CHECK(KeyTwin(ours, apart, true, false));
	CHECK(!KeyTwin(ours, apart, true, true));
}

TEST("quest actors: a script's enemy in no alias is the twin of a friend's spawn of the same NPC their game shared where ours first stood")
{
	// Far Harbor's gate siege, Kiddie Kingdom's ghouls: placed at the same marker in each game, in no alias that keys them.
	const Own    ours{ .enemy = true, .firstAt = At(1000.0f), .at = At(2500.0f), .base = 0x0002898B, .original = 0x0002898B };
	const Theirs spawn{ .sharedAt = At(1300.0f), .at = At(400.0f), .base = 0x0002898B };
	CHECK(SpotTwin(ours, spawn));
	auto far = spawn;
	far.sharedAt = At(1000.0f + SPOT_TWIN + 1.0f);
	CHECK(!SpotTwin(ours, far));
	auto elsewhere = spawn;
	elsewhere.sharedAt = At(1000.0f, 0x00000099);
	CHECK(!SpotTwin(ours, elsewhere));
	auto other = spawn;
	other.base = 0x00012345;  // another NPC
	CHECK(!SpotTwin(ours, other));
	auto keyed = spawn;
	keyed.keyResolves = true;  // its alias keys NPCs here: that alias's own pairs with it
	CHECK(!SpotTwin(ours, keyed));
	auto calm = ours;
	calm.enemy = false;
	CHECK(!SpotTwin(calm, spawn));
	auto system = ours;
	system.otherSystem = true;  // a radiant's, an encounter's, a settlement attack's: their own systems
	CHECK(!SpotTwin(system, spawn));
	auto over = spawn;
	over.over = true;
	CHECK(!SpotTwin(ours, over));
}

TEST("quest actors: of two twins both shared, the plain one lets go for the keyed one, else the higher player id's")
{
	CHECK(Yields(false, 1, true, 2));   // ours plain (shared before it was hostile), theirs keyed
	CHECK(!Yields(true, 2, false, 1));  // ... and their game keeps none
	CHECK(Yields(true, 2, true, 1));    // two keys (two quests' aliases each game keyed apart): the higher player id lets go
	CHECK(!Yields(true, 1, true, 2));
	CHECK(Yields(false, 2, false, 1));  // two plain ones (a script's raider in each game)
	CHECK(!Yields(false, 1, false, 2));
	// Exactly one of the two lets go, whatever the pair.
	for (const bool a : { false, true }) {
		for (const bool b : { false, true }) {
			CHECK(Yields(a, 1, b, 2) != Yields(b, 2, a, 1));
		}
	}
}

TEST("quest actors: ours waits for a twin, goes keyed when nobody has its key, else plain; its key is the lowest that holds it")
{
	const Key min00{ 0x001A001C, 20 };  // a raider held by two quests' aliases (Min00's, and another of a lower form ID)
	const Key other{ 0x0001ED3A, 49 };
	CHECK(Canonical(std::array{ min00, other }) == other);
	CHECK(Canonical(std::array{ other, min00 }) == other);
	CHECK(Canonical(std::array{ Key{ 0x001A001C, 22 }, min00 }) == min00);
	CHECK(Canonical(std::array<Key, 0>{}).quest == 0);
	CHECK(ReportAs(min00, true, false) == Report::kWait);
	CHECK(ReportAs({}, true, false) == Report::kWait);  // a spot twin
	CHECK(ReportAs(min00, false, false) == Report::kKeyed);
	CHECK(ReportAs(min00, false, true) == Report::kPlain);  // the friend's game shares the alias's (a collection's member past their count)
	CHECK(ReportAs({}, false, false) == Report::kPlain);
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

TEST("quest actors: an external alias is judged by the alias its chain ends at: Min00's raiders, created by DN092, are keyed with Min00's alias")
{
	// Min00's WaveRaider01 (alias 20) holds DN092's alias 10 (The Museum of Freedom, a misc quest the session does not share),
	// which creates the raider: the raider is keyed with Min00's alias, which both games run as the same story.
	const Quest min00{ .shared = true, .official = true, .flags = 0xAD21, .type = 4 };
	CHECK(KeyFill(FILL_EXTERNAL, FILL_CREATED) == FILL_CREATED);
	CHECK(Keyed(min00, KeyFill(FILL_EXTERNAL, FILL_CREATED), HUMAN_RACE, ANY_RACE_FORM, 20, true));
	// DN092 itself, not shared here, keys its enemy too now: the raider is held by both quests' aliases, and is reported
	// under the lowest key (Canonical), which both games pick alike.
	const Quest dn092{ .shared = false, .official = true, .flags = 0x8501, .type = 6 };
	CHECK(Keyed(dn092, FILL_CREATED, HUMAN_RACE, ANY_RACE_FORM, 10, true));
	// An alias's own fill is its own; an enemy is keyed through an external alias whatever its chain ends at.
	CHECK(KeyFill(FILL_CREATED, FILL_NONE) == FILL_CREATED);
	CHECK(KeyFill(0, FILL_CREATED) == 0);
	CHECK(Keyed(min00, KeyFill(FILL_EXTERNAL, 1), HUMAN_RACE, ANY_RACE_FORM, 20, true));          // forced
	CHECK(Keyed(min00, KeyFill(FILL_EXTERNAL, 0), HUMAN_RACE, ANY_RACE_FORM, 20, true));          // found by its conditions
	CHECK(Keyed(min00, KeyFill(FILL_EXTERNAL, FILL_NONE), HUMAN_RACE, ANY_RACE_FORM, 20, true));  // a chain that broke
	CHECK(!Keyed(min00, KeyFill(FILL_EXTERNAL, 1), MIRELURK_RACE, ANY_RACE_FORM, 20));            // a calm creature: only a created one
	// A calm human is not keyed through an external alias; a calm creature of a quest not shared is not either.
	CHECK(!Keyed(min00, KeyFill(FILL_EXTERNAL, FILL_CREATED), HUMAN_RACE, ANY_RACE_FORM, 20));
	auto quest = min00;
	quest.shared = false;
	CHECK(Keyed(quest, KeyFill(FILL_EXTERNAL, FILL_CREATED), HUMAN_RACE, ANY_RACE_FORM, 20, true));
	quest.type = QUEST_TYPE_MISC;
	CHECK(!Keyed(quest, KeyFill(FILL_EXTERNAL, FILL_CREATED), MIRELURK_RACE, ANY_RACE_FORM, 20));
}

TEST("quest actors: a reference collection's members are keyed with the collection, as a reference alias's created NPC is")
{
	// Special Delivery (MS09Mission01, a side quest that runs once) creates its raider reinforcements in the collection
	// RaiderReinforcements (alias 16) and their leader in the reference alias RaiderReinforcementLeader (alias 18), both
	// "create reference" at the same marker. Both are keyed: the members pair member by member (Pick).
	const Quest delivery{ .shared = true, .official = true, .flags = 0x0101, .type = QUEST_TYPE_SIDE };
	CHECK(AliasFill(REF_ALIAS, FILL_CREATED) == FILL_CREATED);
	CHECK(AliasFill(COLLECTION_ALIAS, FILL_CREATED) == FILL_CREATED);
	CHECK(Keyed(delivery, AliasFill(REF_ALIAS, FILL_CREATED), HUMAN_RACE, ANY_RACE_FORM, 18, true));
	CHECK(Keyed(delivery, AliasFill(COLLECTION_ALIAS, FILL_CREATED), HUMAN_RACE, ANY_RACE_FORM, 16, true));
	// A creature in one too: Far Harbor's Walk in the Park (DLC03MQ02, runs once) creates its mirelurk waves in the
	// collections Wave01 and Wave02 (aliases 8 and 9).
	const Quest walk{ .shared = true, .official = true, .flags = 0x0511, .type = QUEST_TYPE_SIDE };
	CHECK(Keyed(walk, AliasFill(COLLECTION_ALIAS, FILL_CREATED), MIRELURK_RACE, ANY_RACE_FORM, 8));
	CHECK(Keyed(walk, AliasFill(COLLECTION_ALIAS, FILL_CREATED), MIRELURK_RACE, ANY_RACE_FORM, 9, true));
	// An empty collection the quest's script fills (Far Harbor's gate siege, DLC03MQ02Fight's Enemies_Wave1: PlaceActorAtMe,
	// then AddRef) keys its enemies, never a calm creature.
	CHECK(Keyed(walk, AliasFill(COLLECTION_ALIAS, 0), MIRELURK_RACE, ANY_RACE_FORM, 9, true));
	CHECK(!Keyed(walk, AliasFill(COLLECTION_ALIAS, 0), MIRELURK_RACE, ANY_RACE_FORM, 9));
	// A location alias holds no NPC.
	CHECK(AliasFill("Loc", FILL_CREATED) == FILL_NONE);
	CHECK(AliasFill("", FILL_CREATED) == FILL_NONE);
	CHECK(KindOf(REF_ALIAS) == AliasKind::kRef && KindOf(COLLECTION_ALIAS) == AliasKind::kCollection && KindOf("Loc") == AliasKind::kOther);
	// An external reference alias keeps its chain's fill (Min00's raiders, held by DN092's created aliases).
	CHECK(KeyFill(AliasFill(REF_ALIAS, FILL_EXTERNAL), FILL_CREATED) == FILL_CREATED);
	// Only a single alias that created its NPC pairs without its NPC and spot being matched (LooseAlias).
	CHECK(!LooseAlias(AliasKind::kRef, FILL_CREATED));
	CHECK(LooseAlias(AliasKind::kRef, 1));
	CHECK(LooseAlias(AliasKind::kCollection, FILL_CREATED));
	CHECK(LooseAlias(AliasKind::kCollection, 0));
}

TEST("quest actors: the server refuses a second game's keyed member of a collection, and grants its plain one (ReportAs sends a member past the friend's count plain)")
{
	// Plain members: each game shares its own, and the server grants them all, so each player's
	// game holds the other's members too. Keyed with the collection's alias, the second game's are refused: a game whose
	// friend shares the alias already holds its own back to pair them, and sends any past the friend's count plain.
	constexpr std::uint32_t MS09_MISSION01 = 0x000503B8;
	constexpr std::uint32_t LVL_RAIDER = 0x0002898B;
	const auto              raider = [](std::uint32_t a_id, float a_x, std::uint32_t a_quest, std::uint16_t a_alias) {
		const auto kind = a_quest != 0 ? Protocol::SpawnKind::kQuestActor : Protocol::SpawnKind::kPlain;
		return Protocol::Spawn{ 0, a_id, LVL_RAIDER, 0, WORLD, a_x, 2000.0f, 100.0f, 0.5f, a_quest, a_alias, kind };
	};
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(raider(SPAWN, 1000.0f, 0, 0)));
	f.fake->Deliver(1, Report(raider(SPAWN2, 1100.0f, 0, 0)));
	CHECK(HasOwner(Owners(Received(f, 1)), SPAWN, alice));
	CHECK(Spawned(Received(f, 2)).size() == 2);
	f.fake->Deliver(2, Report(raider(SPAWN3, 1000.0f, 0, 0)));
	CHECK(HasOwner(Owners(Received(f, 2)), SPAWN3, bob));
	const auto toAlice = Spawned(Received(f, 1));
	REQUIRE(toAlice.size() == 1);
	CHECK(toAlice[0].refId == SPAWN3 && toAlice[0].playerId == bob && toAlice[0].quest == 0);

	// Alice's members keyed with the collection's alias, and Bob's refused for them.
	f.fake->Deliver(1, Report(raider(0xFF00AAAA, 1200.0f, MS09_MISSION01, 16)));
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Report(raider(0xFF00BBBB, 1200.0f, MS09_MISSION01, 16)));
	CHECK(HasOwner(Owners(Received(f, 2)), 0xFF00BBBB, 0));
}
