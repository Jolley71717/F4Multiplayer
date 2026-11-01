// One roll per leveled NPC for the session (docs/SHARED-ROLLS.md): the pure pick every game makes (SharedRoll.h), the
// session's NPC level and the host's rule for it (SessionLevel.h), their wire and session file, and the server keeping
// both for the session's life.

#include "ServerFixture.h"

#include "SessionFile.h"
#include "SessionLevel.h"
#include "game/SharedRoll.h"

#include <cstring>
#include <limits>
#include <map>
#include <set>

using namespace TestServer;
using namespace std::chrono_literals;
using SharedRoll::Form;

namespace
{
	// A leveled raider: RAIDER is templated on LIST for traits, stats and inventory, and on its own STATS_LIST for the
	// stats slot alone; it can be legendary (LEGEND, rolled from LEGEND_LIST). LIST gives a plain raider by level, a
	// nested list at 20, and one entry that is no NPC.
	constexpr std::uint32_t RAIDER = 0x0001A000;
	constexpr std::uint32_t LIST = 0x0001B000;
	constexpr std::uint32_t STATS_LIST = 0x0001B001;
	constexpr std::uint32_t NESTED = 0x0001B002;
	constexpr std::uint32_t LEGEND_LIST = 0x0001B003;
	constexpr std::uint32_t EMPTY_LIST = 0x0001B004;
	constexpr std::uint32_t CYCLE = 0x0001B005;
	constexpr std::uint32_t BAD_LIST = 0x0001B006;
	constexpr std::uint32_t PSYCHO = 0x0001C001;  // level 1
	constexpr std::uint32_t SCUM = 0x0001C002;    // level 1
	constexpr std::uint32_t WASTER = 0x0001C005;  // level 5
	constexpr std::uint32_t VETERAN = 0x0001C012; // level 12
	constexpr std::uint32_t BOSS = 0x0001C030;    // level 30
	constexpr std::uint32_t DEEP_A = 0x0001C020;  // through NESTED, level 20
	constexpr std::uint32_t DEEP_B = 0x0001C021;
	constexpr std::uint32_t LEGEND = 0x0001C099;
	constexpr std::uint32_t WEAPON = 0x0001D000;  // no NPC

	constexpr std::uint16_t TRAITS = 1 << 0, STATS = 1 << 1, INVENTORY = 1 << 8;

	struct World
	{
		std::map<std::uint32_t, Form> forms;
		mutable std::size_t           looks = 0;

		std::optional<Form> operator()(std::uint32_t a_id) const
		{
			++looks;
			const auto it = forms.find(a_id);
			return it != forms.end() ? std::optional{ it->second } : std::nullopt;
		}
	};

	Form Npc() { return Form{ .kind = Form::Kind::kNpc }; }

	Form List(std::vector<SharedRoll::Entry> a_entries, std::uint8_t a_flags = 0)
	{
		Form list{ .kind = Form::Kind::kList };
		list.entries = std::move(a_entries);
		list.listFlags = a_flags;
		return list;
	}

	World Raiders(float a_legendChance = 0.0f)
	{
		World world;
		auto  raider = Npc();
		raider.useFlags = TRAITS | STATS | INVENTORY;
		raider.baseTemplate = LIST;
		raider.slotTemplates[1] = STATS_LIST;
		raider.legendTemplate = LEGEND_LIST;
		raider.legendChance = a_legendChance;
		world.forms[RAIDER] = raider;
		world.forms[LIST] = List({ { PSYCHO, 1 }, { SCUM, 1 }, { WASTER, 5 }, { VETERAN, 12 }, { NESTED, 20 }, { BOSS, 30 } });
		world.forms[STATS_LIST] = List({ { PSYCHO, 1 }, { WASTER, 5 }, { VETERAN, 12 }, { BOSS, 30 } });
		world.forms[NESTED] = List({ { DEEP_A, 1 }, { DEEP_B, 1 } });
		world.forms[LEGEND_LIST] = List({ { LEGEND, 1 } });
		world.forms[EMPTY_LIST] = List({ { BOSS, 30 } });
		world.forms[CYCLE] = List({ { CYCLE, 1 } });
		world.forms[BAD_LIST] = List({ { WEAPON, 1 } });
		world.forms[WEAPON] = Form{ .kind = Form::Kind::kOther };
		for (const auto npc : { PSYCHO, SCUM, WASTER, VETERAN, BOSS, DEEP_A, DEEP_B, LEGEND }) {
			world.forms[npc] = Npc();
		}
		return world;
	}

	SharedRoll::Levels At(std::uint16_t a_level)
	{
		return { .plain = a_level, .legendary = a_level };
	}

	constexpr std::uint64_t SESSION = 0x1122334455667788ull;

	// What each of a_count references (keyed as plugin references of Fallout4.esm) rolls at a_level.
	std::vector<SharedRoll::Templates> Rolls(const World& a_world, std::uint16_t a_level, std::uint64_t a_session = SESSION, std::uint32_t a_count = 64)
	{
		std::vector<SharedRoll::Templates> out;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			const auto seed = SharedRoll::SeedOf(a_session, SharedRoll::StableKey("Fallout4.esm", 0x0010'0000 + i), 0);
			const auto result = SharedRoll::Pick(RAIDER, At(a_level), seed, 1.0f, a_world);
			REQUIRE(result.picked);
			out.push_back(result.picked->templates);
		}
		return out;
	}

	std::uint16_t Level(Protocol::NpcLevelRule a_rule, std::uint16_t a_current, std::vector<SessionLevel::Player> a_players)
	{
		return SessionLevel::Next(a_rule, a_current, a_players);
	}
}

// ---- the pick ----------------------------------------------------------------------------------------------------

TEST("shared rolls: one seed rolls one NPC in every game, whatever else its game rolled before")
{
	const auto world = Raiders(25.0f);
	const auto seed = SharedRoll::SeedOf(SESSION, SharedRoll::StableKey("Fallout4.esm", 0x0001F0B1), 0);
	const auto first = SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, world);
	REQUIRE(first.picked);
	for (int i = 0; i < 20; ++i) {
		// Another game's own dice are its own: the pick never touches them.
		const auto again = SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, Raiders(25.0f));
		REQUIRE(again.picked);
		CHECK(again.picked->templates == first.picked->templates);
		CHECK(again.picked->legendary == first.picked->legendary);
		CHECK(again.picked->mask == first.picked->mask);
	}
	// The slots it fills are its use flags': traits, stats, inventory.
	CHECK(first.picked->mask == (TRAITS | STATS | INVENTORY));
	// Another session, another reference: other rolls (over many, some differ).
	CHECK(Rolls(world, 14, SESSION) != Rolls(world, 14, SESSION + 1));
	CHECK(Rolls(world, 14) == Rolls(world, 14));
}

TEST("shared rolls: a list gives its highest level up to the session's, or every entry up to it when it says all levels")
{
	auto world = Raiders();
	std::set<std::uint32_t> seen;
	for (const auto& roll : Rolls(world, 14, SESSION, 200)) {
		seen.insert(roll[0]);
		CHECK(roll[0] == roll[8]);  // one list rolled once per resolve: the traits and the inventory are one raider
	}
	CHECK(seen == std::set<std::uint32_t>{ VETERAN });
	seen.clear();
	for (const auto& roll : Rolls(world, 3, SESSION, 200)) {
		seen.insert(roll[0]);
	}
	CHECK(seen == (std::set<std::uint32_t>{ PSYCHO, SCUM }));
	// Nested lists are rolled down to an NPC.
	seen.clear();
	for (const auto& roll : Rolls(world, 25, SESSION, 200)) {
		seen.insert(roll[0]);
	}
	CHECK(seen == (std::set<std::uint32_t>{ DEEP_A, DEEP_B }));

	world.forms[LIST].listFlags = SharedRoll::ALL_LEVELS;
	seen.clear();
	for (const auto& roll : Rolls(world, 14, SESSION, 400)) {
		seen.insert(roll[0]);
	}
	CHECK(seen == (std::set<std::uint32_t>{ PSYCHO, SCUM, WASTER, VETERAN }));
	// The reference's modifier can ask for every level, or never, whatever the list says.
	SharedRoll::Levels never = At(14);
	never.allBelow = SharedRoll::AllBelow::kNever;
	const auto seed = SharedRoll::SeedOf(SESSION, 1, 0);
	CHECK(SharedRoll::Pick(RAIDER, never, seed, 1.0f, world).picked->templates[0] == VETERAN);
}

TEST("shared rolls: a list is rolled once per resolve, unless the NPC rolls each slot on its own")
{
	auto world = Raiders();
	world.forms[RAIDER].slotTemplates[1] = 0;  // stats from the default list too
	bool differed = false;
	for (const auto& roll : Rolls(world, 3, SESSION, 100)) {
		CHECK(roll[0] == roll[1]);
	}
	world.forms[RAIDER].eachSlot = true;
	for (const auto& roll : Rolls(world, 3, SESSION, 100)) {
		differed = differed || roll[0] != roll[1];
	}
	CHECK(differed);
}

TEST("shared rolls: the legendary chance is drawn every time, times the difficulty's multiplier, and makes every slot its template's")
{
	const auto never = Raiders(0.0f);
	const auto always = Raiders(100.0f);
	const auto half = Raiders(50.0f);
	int        legendaries = 0;
	constexpr int COUNT = 400;
	for (int i = 0; i < COUNT; ++i) {
		const auto seed = SharedRoll::SeedOf(SESSION, static_cast<std::uint64_t>(i), 0);
		CHECK(!SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, never).picked->legendary);
		const auto legend = SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, always).picked;
		CHECK(legend->legendary);
		CHECK(legend->mask == (1u << Protocol::IDENTITY_TEMPLATES) - 1);
		CHECK(legend->templates[0] == LEGEND && legend->templates[12] == LEGEND);
		legendaries += SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, half).picked->legendary;
		// A plain roll is the same whether or not the NPC could have been legendary: the draw is made either way.
		const auto plain = SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, never).picked;
		const auto halfPick = SharedRoll::Pick(RAIDER, At(14), seed, 1.0f, half).picked;
		if (!halfPick->legendary) {
			CHECK(halfPick->templates == plain->templates);
		}
		// A dead or deleted reference is never made legendary.
		SharedRoll::Levels dead = At(14);
		dead.legendaryAllowed = false;
		CHECK(!SharedRoll::Pick(RAIDER, dead, seed, 1.0f, always).picked->legendary);
	}
	CHECK(legendaries > COUNT * 35 / 100);
	CHECK(legendaries < COUNT * 65 / 100);
	// Survival's multiplier: half the chance doubled is every one.
	for (int i = 0; i < 50; ++i) {
		CHECK(SharedRoll::Pick(RAIDER, At(14), SharedRoll::SeedOf(SESSION, static_cast<std::uint64_t>(i), 0), 2.0f, half).picked->legendary);
	}
	// A legendary roll reads its lists at its own level (no modifier).
	SharedRoll::Levels split{ .plain = 3, .legendary = 14 };
	auto               legendList = Raiders(100.0f);
	legendList.forms[LEGEND_LIST] = List({ { PSYCHO, 1 }, { VETERAN, 12 } });
	CHECK(SharedRoll::Pick(RAIDER, split, 7, 1.0f, legendList).picked->templates[0] == VETERAN);
}

TEST("shared rolls: a plugin NPC's next life (its cell reset) is rolled anew, legendary or not, and the legendary draw isn't the same for every level")
{
	const auto half = Raiders(50.0f);
	const auto key = SharedRoll::StableKey("Fallout4.esm", 0x0001F0B1);
	constexpr SharedRoll::Levels LEVELS{ .plain = 3, .legendary = 14 };  // at 3 the list has two to pick from
	int        legendaries = 0;
	std::set<std::uint32_t> plainPicks;
	constexpr int LIVES = 200;
	for (std::uint16_t life = 0; life < LIVES; ++life) {
		const auto seed = SharedRoll::SeedOf(SESSION, key, life);
		const auto pick = SharedRoll::Pick(RAIDER, LEVELS, seed, 1.0f, half).picked;
		const auto again = SharedRoll::Pick(RAIDER, LEVELS, seed, 1.0f, half).picked;  // the same life is the same everywhere
		CHECK(again->legendary == pick->legendary);
		CHECK(again->templates == pick->templates);
		legendaries += pick->legendary;
		if (!pick->legendary) {
			plainPicks.insert(pick->templates[0]);
		}
	}
	CHECK(legendaries > LIVES * 30 / 100);  // a legendary spot isn't legendary forever, nor a plain one never
	CHECK(legendaries < LIVES * 70 / 100);
	CHECK(plainPicks.size() > 1);  // and a plain one's slots are rolled anew

	// One seed, levels apart: the legendary verdict is drawn per level, so it is not one number for them all.
	int levelsDiffered = 0;
	for (std::uint64_t i = 0; i < 100; ++i) {
		const auto seed = SharedRoll::SeedOf(SESSION, i, 0);
		std::set<bool> verdicts;
		for (std::uint16_t level = 1; level <= 30; ++level) {
			verdicts.insert(SharedRoll::Pick(RAIDER, At(level), seed, 1.0f, half).picked->legendary);
		}
		levelsDiffered += verdicts.size() == 2;
		// The plain slots don't hang on the legendary level.
		const auto never = Raiders(0.0f);
		CHECK(SharedRoll::Pick(RAIDER, { .plain = 3, .legendary = 14 }, seed, 1.0f, never).picked->templates ==
			  SharedRoll::Pick(RAIDER, { .plain = 3, .legendary = 29 }, seed, 1.0f, never).picked->templates);
	}
	CHECK(levelsDiffered > 90);
}

TEST("shared rolls: lives and packed rolls read back, and damage is refused")
{
	const std::vector<Protocol::RollLife> lives{ { 0x0001F0B1, 1 }, { 0x0001F0B2, 7 } };
	CHECK(Protocol::DecodeRollLives(Protocol::Encode(lives)) == lives);
	CHECK(!Protocol::DecodeRollLives(Protocol::Encode(std::vector<Protocol::RollLife>{ { 0x0001F0B1, 0 } })));                    // no life
	CHECK(!Protocol::DecodeRollLives(Protocol::Encode(std::vector<Protocol::RollLife>{ { 0xFF001234, 1 } })));                    // a spawn
	CHECK(!Protocol::DecodeRollLives(Protocol::Encode(std::vector<Protocol::RollLife>{ { 0x0001F0B1, 1 }, { 0x0001F0B1, 2 } })));  // twice

	std::vector<Protocol::NpcIdentity> rolls;
	for (std::uint32_t i = 0; i < Protocol::MAX_IDENTITIES_PER_PACKET; ++i) {
		Protocol::NpcIdentity roll{ .playerId = 2, .refId = 0x0001F000 + i, .originalBase = RAIDER, .level = 9, .mask = TRAITS };
		roll.templates[0] = VETERAN;
		rolls.push_back(roll);
	}
	CHECK(Protocol::DecodeNpcIdentities(Protocol::Encode(rolls)) == rolls);
	auto more = rolls;
	more.push_back(rolls[0]);
	CHECK(Protocol::DecodeNpcIdentities(Protocol::Encode(more)) == rolls);  // more than a packet holds: the sender splits
	auto cut = Protocol::Encode(rolls);
	cut.pop_back();
	CHECK(!Protocol::DecodeNpcIdentities(cut));
	auto bad = rolls;
	bad[3].level = static_cast<std::uint16_t>(Protocol::MAX_IDENTITY_LEVEL + 1);
	CHECK(!Protocol::DecodeNpcIdentities(Protocol::Encode(bad)));  // one bad entry spoils the packet
}

TEST("shared rolls: what can't be rolled whole is left to the game, and says why")
{
	auto world = Raiders();
	CHECK(SharedRoll::Pick(0x00099999, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kNotLeveled);
	CHECK(SharedRoll::Pick(PSYCHO, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kNotLeveled);  // no template slots
	world.forms[RAIDER].baseTemplate = EMPTY_LIST;
	world.forms[RAIDER].slotTemplates[1] = 0;
	CHECK(SharedRoll::Pick(RAIDER, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kEmptyList);
	CHECK(SharedRoll::Pick(RAIDER, At(30), 1, 1.0f, world).picked);
	world.forms[RAIDER].baseTemplate = BAD_LIST;
	CHECK(SharedRoll::Pick(RAIDER, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kNotNpc);
	world.forms[RAIDER].baseTemplate = CYCLE;
	CHECK(SharedRoll::Pick(RAIDER, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kTooDeep);
	world.forms[RAIDER].baseTemplate = 0x00098765;  // not in the game
	CHECK(SharedRoll::Pick(RAIDER, At(10), 1, 1.0f, world).miss == SharedRoll::Miss::kNotNpc);
	for (const auto miss : { SharedRoll::Miss::kNone, SharedRoll::Miss::kNotLeveled, SharedRoll::Miss::kEmptyList, SharedRoll::Miss::kNotNpc, SharedRoll::Miss::kTooDeep }) {
		CHECK(std::string_view{ SharedRoll::Name(miss) } != "?");
	}
}

// ---- keys --------------------------------------------------------------------------------------------------------

TEST("shared rolls: a reference is keyed by its plugin's name and its id there, whatever the load order or the name's case")
{
	CHECK(SharedRoll::StableKey("Fallout4.esm", 0x01F0B1) == SharedRoll::StableKey("FALLOUT4.ESM", 0x01F0B1));
	CHECK(SharedRoll::StableKey("Fallout4.esm", 0x01F0B1) != SharedRoll::StableKey("DLCCoast.esm", 0x01F0B1));
	CHECK(SharedRoll::StableKey("Fallout4.esm", 0x01F0B1) != SharedRoll::StableKey("Fallout4.esm", 0x01F0B2));
	const auto full = SharedRoll::LocalOf(0x0401F0B1);
	REQUIRE(full);
	CHECK(!full->light);
	CHECK(full->index == 0x04);
	CHECK(full->id == 0x01F0B1);
	const auto light = SharedRoll::LocalOf(0xFE012ABC);
	REQUIRE(light);
	CHECK(light->light);
	CHECK(light->index == 0x012);
	CHECK(light->id == 0xABC);
	CHECK(!SharedRoll::LocalOf(0xFF001234));
}

TEST("shared rolls: a runtime NPC is keyed by what it was placed as and where, on a grid, and the how-many-th there")
{
	const auto raider = SharedRoll::StableKey("Fallout4.esm", RAIDER);
	const auto world = SharedRoll::StableKey("Fallout4.esm", 0x3C);
	const auto here = SharedRoll::SpotKey(raider, world, 1000.0f, 2000.0f);
	REQUIRE(here);
	CHECK(SharedRoll::SpotKey(raider, world, 1010.0f, 2010.0f) == here);  // the same marker, a step apart
	CHECK(SharedRoll::SpotKey(raider, world, 1000.0f + SharedRoll::SPOT_GRID, 2000.0f) != here);
	CHECK(SharedRoll::SpotKey(raider + 1, world, 1000.0f, 2000.0f) != here);
	CHECK(!SharedRoll::SpotKey(raider, world, 0.0f, 0.0f));  // not placed yet
	CHECK(!SharedRoll::SpotKey(raider, world, std::numeric_limits<float>::quiet_NaN(), 1.0f));
	CHECK(!SharedRoll::SpotKey(raider, world, 2.0e6f, 1.0f));

	SharedRoll::Ordinals ordinals;
	CHECK(ordinals.Next(*here, 1000) == 0);
	CHECK(ordinals.Next(*here, 1100) == 1);  // a group placed together
	CHECK(ordinals.Next(*here, 1200) == 2);
	CHECK(ordinals.Next(*here + 1, 1200) == 0);
	CHECK(ordinals.Next(*here, 1200 + SharedRoll::ORDINAL_WINDOW_MS + 1) == 0);  // the encounter again, later
	for (std::uint64_t i = 0; i < SharedRoll::Ordinals::MAX * 2; ++i) {
		ordinals.Next(i, 100000);  // bounded
	}
}

// ---- levels ------------------------------------------------------------------------------------------------------

TEST("shared rolls: a list's level is the session's through the encounter zone, times the reference's modifier")
{
	CHECK(SharedRoll::ListLevel(14, 1.0f, std::nullopt) == 14);
	CHECK(SharedRoll::ListLevel(14, 0.5f, std::nullopt) == 7);
	CHECK(SharedRoll::ListLevel(14, 1.5f, std::nullopt) == 21);
	CHECK(SharedRoll::ListLevel(0, 1.0f, std::nullopt) == 1);
	CHECK(SharedRoll::ListLevel(14, std::numeric_limits<float>::quiet_NaN(), std::nullopt) == 14);
	const SharedRoll::Zone zone{ .min = 10, .max = 30 };
	CHECK(SharedRoll::ListLevel(5, 1.0f, zone) == 10);
	CHECK(SharedRoll::ListLevel(20, 1.0f, zone) == 20);
	CHECK(SharedRoll::ListLevel(45, 1.0f, zone) == 30);
	CHECK(SharedRoll::ListLevel(5, 1.0f, SharedRoll::Zone{ .min = 10, .max = 30, .matchBelowMin = true }) == 5);
	CHECK(SharedRoll::ListLevel(30, 1.0f, SharedRoll::Zone{ .min = 10, .max = 0, .scale = 0.5f }) == 20);
	CHECK(SharedRoll::ListLevel(45, 2.0f, zone) == 60);  // a boss placement past the zone

	CHECK(SharedRoll::NpcLevel(20, 1000, 0, 0) == 20);
	CHECK(SharedRoll::NpcLevel(20, 1500, 0, 0) == 30);
	CHECK(SharedRoll::NpcLevel(20, 500, 15, 0) == 15);
	CHECK(SharedRoll::NpcLevel(40, 1000, 0, 25) == 25);
	CHECK(SharedRoll::NpcLevel(0, 1000, 0, 0) == 1);
}

TEST("shared rolls: five players at five levels build the same raiders with the host's rule highest or host; own leaves each its own")
{
	using Protocol::NpcLevelRule;
	const auto                              world = Raiders(10.0f);
	const std::vector<SessionLevel::Player> players{ { 3, false }, { 12, true }, { 21, false }, { 30, false }, { 45, false } };

	// Highest: one level for all of them, so one roll each.
	const auto highest = Level(NpcLevelRule::kHighest, 0, players);
	CHECK(highest == 45);
	const auto everyone = Rolls(world, highest);
	CHECK(everyone == Rolls(world, Level(NpcLevelRule::kHighest, highest, { { 45, false }, { 3, true } })));
	// The host's: the host's level (12), whoever else is there.
	CHECK(Level(NpcLevelRule::kHost, 0, players) == 12);
	// Own: no session level (each game rolls at its own player's, as before); their rolls differ.
	CHECK(Level(NpcLevelRule::kOwn, 30, players) == 0);
	std::set<std::vector<SharedRoll::Templates>> own;
	for (const auto& player : players) {
		own.insert(Rolls(world, player.level));
	}
	CHECK(own.size() > 1);
}

TEST("shared rolls: the session's level never goes down with highest, a late joiner at a low level included; host keeps the host's while they are away")
{
	using Protocol::NpcLevelRule;
	auto level = Level(NpcLevelRule::kHighest, 0, { { 10, true }, { 14, false } });
	CHECK(level == 14);
	level = Level(NpcLevelRule::kHighest, level, { { 10, true } });  // the level-14 player left
	CHECK(level == 14);
	level = Level(NpcLevelRule::kHighest, level, { { 10, true }, { 2, false } });  // a late joiner, new to the game
	CHECK(level == 14);
	level = Level(NpcLevelRule::kHighest, level, { { 16, true }, { 2, false } });  // someone levelled up
	CHECK(level == 16);
	CHECK(Level(NpcLevelRule::kHighest, 0, { { 5000, false } }) == Protocol::MAX_IDENTITY_LEVEL);
	CHECK(Level(NpcLevelRule::kHighest, 0, { { 0, false } }) == 0);  // nobody's status yet

	CHECK(Level(NpcLevelRule::kHost, 9, { { 20, false } }) == 9);  // the host is away: theirs is kept
	CHECK(Level(NpcLevelRule::kHost, 9, { { 20, false }, { 0, true } }) == 9);  // the host's status is yet to come
	CHECK(Level(NpcLevelRule::kHost, 9, { { 20, false }, { 7, true } }) == 7);  // down too: the host's is the host's
}

TEST("shared rolls: sNpcLevel and --npc-level read average, highest, host and own in any case, sLegendaryDifficulty average, host and own, and nothing else")
{
	CHECK(SessionLevel::Parse("Average") == Protocol::NpcLevelRule::kAverage);
	CHECK(SessionLevel::Parse("highest") == Protocol::NpcLevelRule::kHighest);
	CHECK(SessionLevel::Parse("Host") == Protocol::NpcLevelRule::kHost);
	CHECK(SessionLevel::Parse("OWN") == Protocol::NpcLevelRule::kOwn);
	CHECK(!SessionLevel::Parse(""));
	CHECK(!SessionLevel::Parse("max"));
	for (const auto rule : { Protocol::NpcLevelRule::kOwn, Protocol::NpcLevelRule::kHighest, Protocol::NpcLevelRule::kHost, Protocol::NpcLevelRule::kAverage }) {
		CHECK(SessionLevel::Parse(SessionLevel::Name(rule)) == rule);
	}
	CHECK(SessionLevel::ParseLegendary("AVERAGE") == Protocol::LegendaryRule::kAverage);
	CHECK(SessionLevel::ParseLegendary("host") == Protocol::LegendaryRule::kHost);
	CHECK(SessionLevel::ParseLegendary("Own") == Protocol::LegendaryRule::kOwn);
	CHECK(!SessionLevel::ParseLegendary("highest"));
	CHECK(!SessionLevel::ParseLegendary("survival"));
	for (const auto rule : { Protocol::LegendaryRule::kOwn, Protocol::LegendaryRule::kAverage, Protocol::LegendaryRule::kHost }) {
		CHECK(SessionLevel::ParseLegendary(SessionLevel::Name(rule)) == rule);
	}
	// The defaults, for a host who sets neither.
	CHECK(Protocol::SessionRules{}.npcLevel == Protocol::NpcLevelRule::kAverage);
	CHECK(Protocol::SessionRules{}.legendary == Protocol::LegendaryRule::kAverage);
}

TEST("shared rolls: with average, the session's level is the mean of the players' here now, rounded to the nearest, a half up, and follows them as they come, go and level up")
{
	using Protocol::NpcLevelRule;
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 10, true }, { 11, false } }) == 11);             // 10.5: a tie goes up
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 10, true }, { 11, false }, { 11, false } }) == 11);  // 10.67
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 10, true }, { 10, false }, { 11, false } }) == 10);  // 10.33
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 3, false }, { 12, true }, { 21, false }, { 30, false }, { 45, false } }) == 22);  // 22.2
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 1, false }, { 2, false } }) == 2);  // 1.5

	// An evening: two friends, a third joins new to the game, one levels up, one goes home.
	auto level = Level(NpcLevelRule::kAverage, 0, { { 10, true }, { 20, false } });
	CHECK(level == 15);
	level = Level(NpcLevelRule::kAverage, level, { { 10, true }, { 20, false }, { 0, false } });  // the joiner's status is yet to come
	CHECK(level == 15);
	level = Level(NpcLevelRule::kAverage, level, { { 10, true }, { 20, false }, { 4, false } });
	CHECK(level == 11);  // 11.33: it goes down too
	level = Level(NpcLevelRule::kAverage, level, { { 10, true }, { 23, false }, { 4, false } });
	CHECK(level == 12);  // 12.33
	level = Level(NpcLevelRule::kAverage, level, { { 10, true }, { 4, false } });
	CHECK(level == 7);
	CHECK(Level(NpcLevelRule::kAverage, 7, {}) == 7);  // everyone gone: the session keeps its level
	CHECK(Level(NpcLevelRule::kAverage, 0, { { 5000, false }, { 5000, false } }) == Protocol::MAX_IDENTITY_LEVEL);

	// Every game picks at the one level, whoever's game it is: the five players build the same raiders at 22.
	const auto world = Raiders(10.0f);
	CHECK(Rolls(world, 22) == Rolls(world, Level(NpcLevelRule::kAverage, 0, { { 45, false }, { 30, false }, { 21, true }, { 12, false }, { 3, false } })));
}

TEST("shared rolls: the legendary chance's multiplier is the mean of the games' here now, a half up, the host game's, or each game's own")
{
	using Protocol::LegendaryRule;
	const auto legendary = [](LegendaryRule a_rule, std::uint16_t a_current, std::vector<SessionLevel::Player> a_players) {
		return SessionLevel::NextLegendary(a_rule, a_current, a_players);
	};
	// Mixed difficulties: Normal (1x) and a game whose difficulty gives twice the chance.
	CHECK(legendary(LegendaryRule::kAverage, 0, { { 10, true, 1000 }, { 30, false, 2000 } }) == 1500);
	CHECK(legendary(LegendaryRule::kAverage, 0, { { 10, true, 1000 }, { 30, false, 1001 } }) == 1001);  // 1000.5: a tie goes up
	CHECK(legendary(LegendaryRule::kAverage, 0, { { 10, true, 1000 }, { 30, false, 1000 }, { 3, false, 1001 } }) == 1000);
	CHECK(legendary(LegendaryRule::kAverage, 0, { { 10, true, 750 }, { 30, false, 1000 }, { 3, false, 2000 } }) == 1250);
	// A join and a leave change it; a game that hasn't said yet (0) counts for nothing; nobody: it stays.
	auto mult = legendary(LegendaryRule::kAverage, 0, { { 10, true, 1000 } });
	CHECK(mult == 1000);
	mult = legendary(LegendaryRule::kAverage, mult, { { 10, true, 1000 }, { 5, false, 0 } });
	CHECK(mult == 1000);
	mult = legendary(LegendaryRule::kAverage, mult, { { 10, true, 1000 }, { 5, false, 3000 } });
	CHECK(mult == 2000);
	mult = legendary(LegendaryRule::kAverage, mult, { { 5, false, 3000 } });  // the host left
	CHECK(mult == 3000);
	CHECK(legendary(LegendaryRule::kAverage, mult, {}) == 3000);
	CHECK(legendary(LegendaryRule::kAverage, 0, { { 1, false, 60000 } }) == Protocol::MAX_LEGENDARY_MULT);

	CHECK(legendary(LegendaryRule::kHost, 0, { { 10, false, 3000 }, { 10, true, 1000 } }) == 1000);
	CHECK(legendary(LegendaryRule::kHost, 1200, { { 10, false, 3000 } }) == 1200);  // the host away: theirs is kept
	CHECK(legendary(LegendaryRule::kOwn, 1200, { { 10, true, 3000 } }) == 0);

	// The pick at the session's multiplier is the same in every game, whatever each game's own difficulty.
	const auto half = Raiders(50.0f);
	for (std::uint64_t i = 0; i < 100; ++i) {
		const auto seed = SharedRoll::SeedOf(SESSION, i, 0);
		const auto shared = legendary(LegendaryRule::kAverage, 0, { { 10, true, 1000 }, { 30, false, 2000 } }) / 1000.0f;
		CHECK(SharedRoll::Pick(RAIDER, At(14), seed, shared, half).picked->legendary == SharedRoll::Pick(RAIDER, At(14), seed, shared, Raiders(50.0f)).picked->legendary);
	}
}

// ---- wire and file -----------------------------------------------------------------------------------------------

TEST("shared rolls: the welcome carries the host's NPC level rule, and the session's level reads back within its bounds")
{
	for (const auto rule : { Protocol::NpcLevelRule::kOwn, Protocol::NpcLevelRule::kHighest, Protocol::NpcLevelRule::kHost, Protocol::NpcLevelRule::kAverage }) {
		const auto welcome = Protocol::DecodeWelcome(Protocol::Encode(Protocol::Welcome{ 3, SESSION, false, true, Protocol::SessionRules{ .npcLevel = rule } }));
		REQUIRE(welcome);
		CHECK(welcome->rules.npcLevel == rule);
	}
	// A rule this build doesn't know is the default.
	for (const auto rule : { Protocol::LegendaryRule::kOwn, Protocol::LegendaryRule::kAverage, Protocol::LegendaryRule::kHost }) {
		const auto welcome = Protocol::DecodeWelcome(Protocol::Encode(Protocol::Welcome{ 3, SESSION, false, true, Protocol::SessionRules{ .legendary = rule } }));
		REQUIRE(welcome);
		CHECK(welcome->rules.legendary == rule);
	}
	// Rules this build doesn't know are the defaults.
	auto odd = Protocol::Encode(Protocol::Welcome{ 3, SESSION, false, true, Protocol::SessionRules{ .npcLevel = Protocol::NpcLevelRule::kOwn, .legendary = Protocol::LegendaryRule::kOwn } });
	odd[odd.size() - 2] = 9;
	odd.back() = 9;
	const auto read = Protocol::DecodeWelcome(odd);
	REQUIRE(read);
	CHECK(read->rules.npcLevel == Protocol::NpcLevelRule::kAverage);
	CHECK(read->rules.legendary == Protocol::LegendaryRule::kAverage);
	CHECK(Protocol::SessionRules{ .npcLevel = static_cast<Protocol::NpcLevelRule>(7) }.Clamped().npcLevel == Protocol::NpcLevelRule::kAverage);
	CHECK(Protocol::SessionRules{ .legendary = static_cast<Protocol::LegendaryRule>(7) }.Clamped().legendary == Protocol::LegendaryRule::kAverage);

	CHECK(Protocol::DecodeSessionWorld(Protocol::Encode(Protocol::SessionWorld{ 23 }))->level == 23);
	CHECK((Protocol::DecodeSessionWorld(Protocol::Encode(Protocol::SessionWorld{ 23, 1500 })) == Protocol::SessionWorld{ 23, 1500 }));
	CHECK(!Protocol::DecodeSessionWorld(Protocol::Encode(Protocol::SessionWorld{ 23, static_cast<std::uint16_t>(Protocol::MAX_LEGENDARY_MULT + 1) })));

	// Each game's difficulty and its multiplier, in its status.
	Protocol::PlayerStatus status;
	status.level = 21;
	CHECK(Protocol::DecodePlayerStatus(Protocol::Encode(status, MT::kReportStatus))->difficulty == Protocol::NO_DIFFICULTY);
	status.difficulty = 6;
	status.legendaryMult = 2000;
	const auto back = Protocol::DecodePlayerStatus(Protocol::Encode(status, MT::kReportStatus));
	REQUIRE(back);
	CHECK(back->difficulty == 6);
	CHECK(back->legendaryMult == 2000);
	CHECK(*back == status);
	status.difficulty = 7;
	CHECK(!Protocol::DecodePlayerStatus(Protocol::Encode(status, MT::kReportStatus)));
	status.difficulty = 2;
	status.legendaryMult = Protocol::MAX_LEGENDARY_MULT + 1;
	CHECK(!Protocol::DecodePlayerStatus(Protocol::Encode(status, MT::kReportStatus)));
	CHECK(!Protocol::DecodeSessionWorld(Protocol::Encode(Protocol::SessionWorld{ static_cast<std::uint16_t>(Protocol::MAX_IDENTITY_LEVEL + 1) })));
	auto shortened = Protocol::Encode(Protocol::SessionWorld{ 23 });
	shortened.pop_back();
	CHECK(!Protocol::DecodeSessionWorld(shortened));
	static_assert(Protocol::VERSION == 57);
}

TEST("shared rolls: the session file keeps every plugin NPC's roll and the session's level (layout 41), and one without them keeps its older layout")
{
	SessionFile::Snapshot snapshot;
	snapshot.contentHash = 0x1234;
	snapshot.sessionId = SESSION;
	snapshot.deadActors = { 0x0001F0B0 };
	Protocol::NpcIdentity roll{ .playerId = 2, .refId = 0x0001F0B1, .originalBase = RAIDER, .level = 21, .flags = Protocol::NpcIdentity::kLegendary, .mask = TRAITS | STATS };
	roll.templates[0] = LEGEND;
	roll.templates[1] = LEGEND;
	snapshot.npcIdentities = { roll };
	snapshot.npcLevel = 21;
	snapshot.legendaryMult = 1250;
	snapshot.rollLives = { { 0x0001F0B0, 1 }, { 0x0001F0B1, 3 } };
	const auto bytes = SessionFile::Encode(snapshot);
	std::uint16_t layout = 0;
	std::memcpy(&layout, bytes.data() + 4, sizeof(layout));
	CHECK(layout == SessionFile::ROLLS);
	const auto read = SessionFile::Decode(bytes);
	REQUIRE(read);
	CHECK(read->npcIdentities == snapshot.npcIdentities);
	CHECK(read->npcLevel == 21);
	CHECK(read->legendaryMult == 1250);
	CHECK(read->rollLives == snapshot.rollLives);
	CHECK(read->deadActors == snapshot.deadActors);
	CHECK(!read->Empty());
	// Lives alone (every roll since died) are rolls' news too: layout 41.
	auto livesOnly = snapshot;
	livesOnly.npcIdentities.clear();
	livesOnly.npcLevel = 0;
	livesOnly.legendaryMult = 0;
	const auto livesBytes = SessionFile::Encode(livesOnly);
	std::memcpy(&layout, livesBytes.data() + 4, sizeof(layout));
	CHECK(layout == SessionFile::ROLLS);
	const auto livesRead = SessionFile::Decode(livesBytes);
	REQUIRE(livesRead);
	CHECK(livesRead->rollLives == snapshot.rollLives);

	// Without rolls the layout is 36's, as before: a build of 0.10.1 still reads it.
	auto older = livesOnly;
	older.rollLives.clear();
	const auto olderBytes = SessionFile::Encode(older);
	std::memcpy(&layout, olderBytes.data() + 4, sizeof(layout));
	CHECK(layout == SessionFile::OBJECT_PLACES);
	// A spawn's roll in a file is damage (spawns end with the run).
	auto spawned = snapshot;
	spawned.npcIdentities[0].refId = 0xFF001234;
	CHECK(!SessionFile::Decode(SessionFile::Encode(spawned)));
}

// ---- the server --------------------------------------------------------------------------------------------------

namespace
{
	std::vector<std::uint8_t> Status(std::uint16_t a_level, std::uint16_t a_legendaryMult = 0, std::uint8_t a_difficulty = Protocol::NO_DIFFICULTY)
	{
		Protocol::PlayerStatus status;
		status.level = a_level;
		status.legendaryMult = a_legendaryMult;
		status.difficulty = a_difficulty;
		return Protocol::Encode(status, MT::kReportStatus);
	}

	// The last session world a_peer was told within a_wait (nothing if none).
	std::optional<Protocol::SessionWorld> LastWorld(Fixture& a_f, PeerId a_peer, std::chrono::milliseconds a_wait = 500ms)
	{
		std::optional<Protocol::SessionWorld> last;
		while (const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kSessionWorld), a_wait)) {
			const auto world = Protocol::DecodeSessionWorld(*packet);
			REQUIRE(world);
			last = *world;
		}
		return last;
	}

	// The last session level a_peer was told within a_wait (nothing if none).
	std::optional<std::uint16_t> LastLevel(Fixture& a_f, PeerId a_peer, std::chrono::milliseconds a_wait = 500ms)
	{
		std::optional<std::uint16_t> last;
		while (const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kSessionWorld), a_wait)) {
			const auto world = Protocol::DecodeSessionWorld(*packet);
			REQUIRE(world);
			last = world->level;
		}
		return last;
	}
}

TEST("shared rolls: the server keeps the session's NPC level from its players' statuses and tells everyone when it changes (highest)")
{
	Server::Options options;
	options.rules.npcLevel = Protocol::NpcLevelRule::kHighest;
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	CHECK(LastLevel(f, 1) == 0);  // after the welcome: none yet
	f.fake->Deliver(1, Status(12));
	CHECK(LastLevel(f, 1) == 12);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	CHECK(LastLevel(f, 2) == 12);  // a joiner is told it with the welcome
	f.fake->Deliver(2, Status(21));
	CHECK(LastLevel(f, 1) == 21);
	CHECK(LastLevel(f, 2) == 21);
	f.fake->Deliver(2, Status(21));  // the same status again: no news
	CHECK(!LastLevel(f, 1, 300ms));
	// Bob leaves; Carol, level 3, joins late: the level stays (an NPC rolled at it stays that).
	f.fake->Drop(2);
	REQUIRE(f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Deliver(3, Status(3));
	CHECK(LastLevel(f, 3) == 21);
	CHECK(f.LoggedSoon("the session's NPC level is 21"));
}

TEST("shared rolls: by default the session's level and legendary chance are the averages of the players here, told to everyone as players join, level up and leave")
{
	Fixture f;  // average and average, the defaults
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Deliver(1, Status(10, 1000, 2));  // Normal
	f.fake->Deliver(2, Status(21, 2000, 6));  // Survival, as its game says
	Sync(f, 1);
	Sync(f, 2);
	CHECK((LastWorld(f, 1) == Protocol::SessionWorld{ 16, 1500 }));  // 15.5 up
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	CHECK((LastWorld(f, 3) == Protocol::SessionWorld{ 16, 1500 }));  // a joiner is told it with the welcome
	f.fake->Deliver(3, Status(4, 1000, 2));
	Sync(f, 3);
	CHECK((LastWorld(f, 2) == Protocol::SessionWorld{ 12, 1333 }));  // 11.67, 1333.3
	f.fake->Deliver(1, Status(13, 1000, 2));  // Alice levelled up
	Sync(f, 1);
	CHECK((LastWorld(f, 3) == Protocol::SessionWorld{ 13, 1333 }));  // 12.67
	// Bob goes home: the average is of Alice and Carol.
	f.fake->Drop(2);
	REQUIRE(f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	CHECK((LastWorld(f, 1) == Protocol::SessionWorld{ 9, 1000 }));  // 8.5 up
	CHECK(f.LoggedSoon("the session's legendary chance is x1.000"));
	// Alice goes too: Carol's alone. Then Carol: with nobody here the session keeps it for whoever comes next.
	f.fake->Drop(1);
	REQUIRE(f.fake->WaitFor(3, Type(MT::kPlayerLeft)));
	CHECK((LastWorld(f, 3) == Protocol::SessionWorld{ 4, 1000 }));
	f.fake->Drop(3);
	REQUIRE(f.Join(4, Fixture::MakeHello("Dana")));
	CHECK((LastWorld(f, 4) == Protocol::SessionWorld{ 4, 1000 }));
}

TEST("shared rolls: with the host's rule, the session's level is the host game's; with own, there is none")
{
	{
		Server::Options options;
		options.hostIdentity = 0x4805;
		options.rules.npcLevel = Protocol::NpcLevelRule::kHost;
		options.rules.legendary = Protocol::LegendaryRule::kHost;
		Fixture f{ options };
		f.fake->SetLocal(1);
		REQUIRE(f.Join(1, Fixture::MakeHello("Host", 0x4805)));
		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		f.fake->Deliver(2, Status(30, 2000));
		CHECK(LastLevel(f, 2).value_or(0) == 0);
		f.fake->Deliver(1, Status(8, 750));
		CHECK((LastWorld(f, 2) == Protocol::SessionWorld{ 8, 750 }));
	}
	{
		Server::Options options;
		options.rules.npcLevel = Protocol::NpcLevelRule::kOwn;
		options.rules.legendary = Protocol::LegendaryRule::kOwn;
		Fixture f{ options };
		const auto welcome = f.Join(1, Fixture::MakeHello("Alice"));
		REQUIRE(welcome);
		CHECK(welcome->rules.npcLevel == Protocol::NpcLevelRule::kOwn);
		CHECK(welcome->rules.legendary == Protocol::LegendaryRule::kOwn);
		f.fake->Deliver(1, Status(30, 2000));
		CHECK(LastWorld(f, 1).value_or(Protocol::SessionWorld{}) == Protocol::SessionWorld{});
	}
	{
		// A dedicated server has no host game: its host rules are the highest level and the average legendary chance.
		Server::Options options;
		options.rules.npcLevel = Protocol::NpcLevelRule::kHost;
		options.rules.legendary = Protocol::LegendaryRule::kHost;
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		f.fake->Deliver(1, Status(17, 1000));
		f.fake->Deliver(2, Status(5, 2000));
		Sync(f, 1);
		Sync(f, 2);
		CHECK((LastWorld(f, 1) == Protocol::SessionWorld{ 17, 1500 }));
	}
}

TEST("shared rolls: a restarted server's session still has every NPC's roll and its level, and tells a newcomer all of them")
{
	const TempSessionFile file;
	Server::Options       options;
	options.sessionFile = file.path.string();
	Protocol::NpcIdentity roll{ .refId = 0x0001F0B1, .originalBase = RAIDER, .level = 21, .mask = TRAITS };
	roll.templates[0] = VETERAN;
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Alice", 0xA11CE)));
		f.fake->Deliver(1, Status(21));
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { roll.refId, Protocol::ClaimReason::kUnowned } }));
		f.fake->Deliver(1, Protocol::Encode(roll, MT::kReportNpcIdentity));
		Sync(f, 1);
		f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ roll.refId }));  // nobody runs it now
		Sync(f, 1);
	}  // stopping saves
	{
		Fixture f{ options };
		CHECK(f.LoggedSoon("1 NPC rolls, NPC level 21"));
		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		CHECK(LastLevel(f, 2) == 21);
		auto told = WelcomeRolls(f, 2);
		REQUIRE(told.size() == 1);
		REQUIRE(told.contains(roll.refId));
		CHECK(told[roll.refId].templates[0] == VETERAN);
		CHECK(told[roll.refId].level == 21);
	}
}

TEST("shared rolls: each death of a plugin's NPC is a new life, told to every game (its runner too), kept through a restart and told to a newcomer before the rolls")
{
	const TempSessionFile   file;
	Server::Options         options;
	options.sessionFile = file.path.string();
	constexpr std::uint32_t REF = 0x0001F0B1;
	const auto              lifeOf = [](const std::vector<std::vector<std::uint8_t>>& a_packets, std::uint32_t a_ref) {
		std::optional<std::uint16_t> last;
		for (const auto& lives : Decoded(a_packets, MT::kRollLives, [](auto a_data) { return Protocol::DecodeRollLives(a_data); })) {
			for (const auto& life : lives) {
				if (life.refId == a_ref) {
					last = life.lives;
				}
			}
		}
		return last;
	};
	const auto death = Protocol::Encode(Protocol::ActorDeath{ REF, 0, true }, MT::kReportDeath);
	{
		Fixture f{ options };
		REQUIRE(f.Join(1, Fixture::MakeHello("Alice", 0xA11CE)));
		REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { REF, Protocol::ClaimReason::kUnowned } }));
		Protocol::NpcIdentity roll{ .refId = REF, .originalBase = RAIDER, .level = 9, .mask = TRAITS };
		roll.templates[0] = VETERAN;
		f.fake->Deliver(1, Protocol::Encode(roll, MT::kReportNpcIdentity));
		Clear(f, { 1, 2 });
		f.fake->Deliver(1, death);
		Sync(f, 1);
		CHECK(lifeOf(Received(f, 1), REF) == 1);  // the runner's game is told too: its own next roll takes it
		CHECK(lifeOf(Received(f, 2), REF) == 1);
		f.fake->Deliver(2, death);  // the same death again, from another game: no new life
		Sync(f, 2);
		CHECK(!lifeOf(Received(f, 1), REF));
		// Another plugin NPC rolled before: the newcomer is told its roll after the lives.
		constexpr std::uint32_t OTHER = 0x0001F0B2;
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { OTHER, Protocol::ClaimReason::kUnowned } }));
		roll.refId = OTHER;
		f.fake->Deliver(1, Protocol::Encode(roll, MT::kReportNpcIdentity));
		Sync(f, 1);
		f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ OTHER }));
		Sync(f, 1);
	}  // stopping saves
	{
		Fixture f{ options };
		REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
		auto       got = Received(f, 3);
		CHECK(lifeOf(got, REF) == 1);
		const auto livesAt = std::ranges::find_if(got, [](const auto& a_p) { return !a_p.empty() && a_p[0] == Type(MT::kRollLives); });
		const auto rollsAt = std::ranges::find_if(got, [](const auto& a_p) { return !a_p.empty() && a_p[0] == Type(MT::kNpcIdentities); });
		CHECK(livesAt < rollsAt);  // a roll told is seeded with its life: the life comes first
	}
}

TEST("shared rolls: a newcomer is told the session's rolls packed, many to a packet")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	constexpr std::uint32_t COUNT = 40;
	std::vector<Protocol::ActorClaim> claims;
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		claims.push_back({ 0x0001F000 + i, Protocol::ClaimReason::kUnowned });
	}
	f.fake->Deliver(1, Protocol::Encode(claims));
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		Protocol::NpcIdentity roll{ .refId = 0x0001F000 + i, .originalBase = RAIDER, .level = 9, .mask = TRAITS };
		roll.templates[0] = VETERAN;
		f.fake->Deliver(1, Protocol::Encode(roll, MT::kReportNpcIdentity));
	}
	Sync(f, 1);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto got = Received(f, 2);
	CHECK(Count(got, MT::kNpcIdentity) == 0);
	CHECK(Count(got, MT::kNpcIdentities) == (COUNT + Protocol::MAX_IDENTITIES_PER_PACKET - 1) / Protocol::MAX_IDENTITIES_PER_PACKET);
	std::size_t told = 0;
	for (const auto& list : Decoded(got, MT::kNpcIdentities, [](auto a_data) { return Protocol::DecodeNpcIdentities(a_data); })) {
		told += list.size();
	}
	CHECK(told == COUNT);
}
