#include "Test.h"

#include "game/HostileHold.h"
#include "game/StageRules.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using HostileHold::Clock;
	using StageRules::Verdict;

	constexpr std::uint64_t KEY = (0x0323DEull << 16) | 700;  // DLC03CoA_Kickout 700
	constexpr std::uint64_t OTHER = (0x16D036ull << 16) | 100;
}

TEST("hostile hold: a member of the faction is near when within RANGE, and counts unless dead, disabled or following a player")
{
	using HostileHold::Counts;
	using HostileHold::Near;
	using HostileHold::RANGE;
	CHECK(Near(0.0f, 0.0f, 0.0f));
	CHECK(Near(RANGE, 0.0f, 0.0f));
	CHECK(Near(5000.0f, 5000.0f, 0.0f));  // 7071 u
	CHECK(!Near(RANGE + 1.0f, 0.0f, 0.0f));
	CHECK(!Near(6000.0f, 6000.0f, 0.0f));  // 8485 u
	CHECK(!Near(0.0f, 0.0f, RANGE + 1.0f));  // straight up, too far
	static_assert(RANGE > StageRules::NEAR_FRIEND);  // farther than "beside the friend": a faction that near comes at the player
	// alive, disabled, follows a player
	CHECK(Counts(true, false, false));
	CHECK(!Counts(false, false, false));  // dead
	CHECK(!Counts(true, true, false));    // disabled
	CHECK(!Counts(true, false, true));    // a companion in the faction (Danse, Deacon, X6-88), ours or a friend's: beside a player, the kick-out's script handles them
	CHECK(!Counts(false, true, true));
	static_assert(HostileHold::CALL_TRIES > 1);
}

TEST("hostile hold: a held stage's wait counts play only, a line is due as it begins and every LOG_EVERY after, and the HUD's once after NOTICE_AFTER")
{
	HostileHold::Waits waits;
	const auto         t0 = Clock::time_point{} + 1h;
	waits.Tick(t0, false);
	CHECK(waits.Size() == 0);

	constexpr std::uint32_t ATOM = 0x03004462;  // the Children of Atom, in this game
	auto judged = waits.Hold(KEY, ATOM);
	CHECK(judged.begun);
	CHECK(judged.logDue);
	CHECK(!judged.noticeDue);
	CHECK(judged.played == 0s);
	CHECK(waits.Holds(KEY));
	CHECK(!waits.Holds(OTHER));

	// 10 s of play: judged again, not begun, no line due yet.
	waits.Tick(t0 + 10s, false);
	judged = waits.Hold(KEY, ATOM);
	CHECK(!judged.begun);
	CHECK(!judged.logDue);
	CHECK(!judged.noticeDue);
	CHECK(judged.played == 10s);

	// 4 minutes paused (a menu, a loading screen): nothing counted.
	waits.Tick(t0 + 10s + 4min, true);
	judged = waits.Hold(KEY, ATOM);
	CHECK(judged.played == 10s);
	CHECK(!judged.logDue);
	CHECK(!judged.noticeDue);

	// NOTICE_AFTER of play: the HUD line, once; then LOG_EVERY of play since the hold began: a line.
	static_assert(HostileHold::NOTICE_AFTER < HostileHold::LOG_EVERY);
	waits.Tick(t0 + 10s + 4min + HostileHold::NOTICE_AFTER - 10s, false);
	judged = waits.Hold(KEY, ATOM);
	CHECK(judged.played == HostileHold::NOTICE_AFTER);
	CHECK(judged.noticeDue);
	CHECK(!judged.logDue);
	waits.Tick(t0 + 10s + 4min + HostileHold::NOTICE_AFTER, false);
	CHECK(!waits.Hold(KEY, ATOM).noticeDue);
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY - 10s, false);
	judged = waits.Hold(KEY, ATOM);
	CHECK(judged.played == HostileHold::LOG_EVERY);
	CHECK(judged.logDue);
	CHECK(!judged.noticeDue);
	// ... and not again at once.
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY, false);
	judged = waits.Hold(KEY, ATOM);
	CHECK(!judged.logDue);
	CHECK(judged.played == HostileHold::LOG_EVERY + 10s);

	// A second stage held later has its own count.
	const auto second = waits.Hold(OTHER);
	CHECK(second.begun);
	CHECK(second.played == 0s);
	CHECK(waits.Size() == 2);
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY + 30s, false);
	CHECK(waits.Hold(OTHER).played == 30s);
	CHECK(waits.Hold(KEY, ATOM).played == HostileHold::LOG_EVERY + 40s);

	// Set: the whole wait and the faction it last waited for, and it is no longer held.
	const auto waited = waits.End(KEY);
	REQUIRE(waited.has_value());
	CHECK(waited->played == HostileHold::LOG_EVERY + 40s);
	CHECK(waited->faction == ATOM);
	CHECK(!waits.Holds(KEY));
	CHECK(!waits.End(KEY).has_value());  // ended twice: nothing
	CHECK(waits.Size() == 1);

	// A held stage whose quest went away is dropped (Keep), the others kept with their counts.
	waits.Hold(KEY);
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY + 40s, false);
	waits.Keep([](std::uint64_t a_key) { return a_key == OTHER; });
	CHECK(waits.Size() == 1);
	CHECK(!waits.Holds(KEY));
	CHECK(waits.Hold(OTHER).played == 40s);
	CHECK(waits.End(OTHER)->faction == 0);  // held with no faction named

	waits.Clear();
	CHECK(waits.Size() == 0);
	CHECK(!waits.Holds(OTHER));
}

TEST("hostile hold: a stage that turns a faction on the player waits while any of them is near, and is set once none is; never skipped")
{
	using StageRules::Judge;
	const StageRules::Rule hostile{ 1, 0x004C8E, 70, StageRules::kHostile, 0 };
	// Caught up (a joiner, a load), the faction near: waits, its quest with it; none near: set.
	CHECK(Judge(&hostile, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&hostile, { .live = false, .factionNear = false }) == Verdict::kSet);
	// Live, the friend far: the same (no offer, no skip); live beside the friend: set at once, they are in that fight together.
	CHECK(Judge(&hostile, { .live = true, .factionNear = true, .followable = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&hostile, { .live = true, .factionNear = false, .followable = true }) == Verdict::kSet);
	CHECK(Judge(&hostile, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
	// A fight with anyone else is not "near" (the caller counts the faction by distance only): the capped combat hold holds
	// it, caught up or live far from the friend, as it holds any caught-up stage; live beside the friend it is set mid-fight.
	CHECK(Judge(&hostile, { .live = false, .factionNear = false, .held = true }) == Verdict::kLater);
	CHECK(Judge(&hostile, { .live = true, .factionNear = false, .held = true, .followable = true }) == Verdict::kLater);
	CHECK(Judge(&hostile, { .live = true, .nearFriend = true, .factionNear = false, .held = true }) == Verdict::kSet);
	CHECK(Judge(&hostile, { .live = false, .factionNear = true, .held = true }) == Verdict::kHostileWaits);  // the faction near: the hold, not the cap
	// The opening doesn't hold it (it moves nobody), a payment or an unshared story stage is still skipped.
	CHECK(Judge(&hostile, { .live = false, .factionNear = false, .opening = true }) == Verdict::kSet);
	const StageRules::Rule pays{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kPays, 0 };
	CHECK(Judge(&pays, { .live = true, .nearFriend = true }) == Verdict::kSkip);
	const StageRules::Rule story{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kStory, 0 };
	CHECK(Judge(&story, { .sharedStory = false, .live = false }) == Verdict::kSkip);
	CHECK(Judge(&story, { .sharedStory = true, .live = false }) == Verdict::kSet);
	// With kMoves too: the hold first, then the move's rules (offered far, set beside the friend); not gone with, the stage
	// isn't set (it would move the player) but the faction is turned on them here all the same: kHostileOnly, never kSkip.
	const StageRules::Rule both{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kMoves, 0 };
	CHECK(Judge(&both, { .live = true, .factionNear = true, .followable = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&both, { .live = true, .factionNear = false, .followable = true }) == Verdict::kOffer);
	CHECK(Judge(&both, { .live = true, .factionNear = false, .held = true, .followable = true }) == Verdict::kOffer);  // fighting: offered all the same, as any move
	CHECK(Judge(&both, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
	CHECK(Judge(&both, { .live = false, .nearFriend = true, .factionNear = false, .followable = true, .offered = 40s }) == Verdict::kSet);  // came to the friend
	CHECK(Judge(&both, { .live = false, .factionNear = false, .followable = true, .offered = StageRules::OFFER_FOR }) == Verdict::kHostileOnly);  // the offer run out
	CHECK(Judge(&both, { .live = true, .factionNear = false }) == Verdict::kHostileOnly);   // nobody to follow
	CHECK(Judge(&both, { .live = false, .factionNear = false }) == Verdict::kHostileOnly);  // caught up (a joiner, a load)
	CHECK(Judge(&both, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);  // ... but the hold first
	CHECK(Judge(&both, { .live = false, .factionNear = false, .held = true }) == Verdict::kLater);  // ... and the fight's cap
	CHECK(Judge(&both, { .live = false, .factionNear = false, .opening = true }) == Verdict::kOpeningWaits);
	// A plain move is still skipped; a bridge that turns a faction on the player is still warned and set (it is needed all the same).
	const StageRules::Rule moves{ 1, 0x004C8E, 70, StageRules::kMoves, 0 };
	CHECK(Judge(&moves, { .live = false }) == Verdict::kSkip);
	const StageRules::Rule bridge{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kMoves | StageRules::kBridge, 0 };
	CHECK(Judge(&bridge, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&bridge, { .live = false, .factionNear = false }) == Verdict::kWarn);
	CHECK(Judge(&bridge, { .live = false, .factionNear = false, .warned = StageRules::WARNING }) == Verdict::kSet);
}

TEST("hostile hold: a stage judged for its quest's start-up stage too is held for the start-up's faction (InstKickOut 0, DN079 0)")
{
	using namespace StageRules;
	// The quests' start-up stages as their records flag them (QuestStageLayout::StartUps): InstKickOut's and DN079's are their 0.
	constexpr std::array<std::uint16_t, 1> START_UP_0{ 0 };
	constexpr std::span<const std::uint16_t> NOT_STARTING{};  // the quest runs here already
	// InstKickOut 100 starting the quest here: the Institute twice (the start-up's row first, then its own); running already: its own.
	auto rows = HostileRows(File::kFallout4, 0x16D036, 100, START_UP_0);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].stage == 0);
	CHECK(rows[1].stage == 100);
	CHECK(rows[0].faction == 0x05E558 && rows[1].faction == 0x05E558);
	rows = HostileRows(File::kFallout4, 0x16D036, 100, NOT_STARTING);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].stage == 100);
	// DN079's 520 (the cannery's people) starting the quest: its 0's turrets too.
	rows = HostileRows(File::kFallout4, 0x02633E, 520, START_UP_0);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].faction == 0x19EC30);
	CHECK(rows[1].faction == 0x03EFAE);
	// Stage 0 itself: once. A stage of a quest whose start-up turns nobody: its own alone. Nothing: nothing.
	CHECK(HostileRows(File::kFallout4, 0x16D036, 0, START_UP_0).size() == 1);
	CHECK(HostileRows(File::kCoast, 0x004C8E, 70, START_UP_0).size() == 2);  // the Children of Atom and their interior faction
	CHECK(HostileRows(File::kFallout4, 0x01CC2A, 10, START_UP_0).empty());
}

TEST("hostile hold: a stage that reaches a kick-out only on a condition of the game it runs in is held for nobody (HOSTILE's conditional rows)")
{
	using namespace StageRules;
	constexpr std::uint32_t INSTITUTE = 0x05E558;
	constexpr std::array<std::uint16_t, 1> START_UP_0{ 0 };
	constexpr std::span<const std::uint16_t> NOT_STARTING{};
	// The Minutemen's and the endings' stages reach the Institute's kick-out through MinutemenCentralScript.CheckForMQAdvance,
	// which goes on only for a player the Institute has thrown out already: the row stays in the table (HostileOf), and the
	// stage is judged without it (HostileRows): no hold while Institute members are near, no "turned on your group" line.
	for (const auto [quest, stage] : { std::pair{ 0x0AA778u, 980 }, std::pair{ 0x136329u, 200 }, std::pair{ 0x215CC7u, 10 }, std::pair{ 0x03A457u, 70 },
			 std::pair{ 0x05DEE4u, 2000 }, std::pair{ 0x0229EEu, 1000 }, std::pair{ 0x10C64Au, 2000 }, std::pair{ 0x0FDC8Cu, 250 } }) {
		const auto rows = HostileOf(File::kFallout4, quest, static_cast<std::uint16_t>(stage));
		REQUIRE(rows.size() == 1);
		CHECK(rows.front().conditional);
		CHECK(rows.front().faction == INSTITUTE);
		CHECK(HostileRows(File::kFallout4, quest, static_cast<std::uint16_t>(stage), NOT_STARTING).empty());
		CHECK(HostileRows(File::kFallout4, quest, static_cast<std::uint16_t>(stage), START_UP_0).empty());
	}
	CHECK(HostileRows(File::kCoast, 0x040A87, 400, NOT_STARTING).empty());  // Far Harbor's settlement radiant, through the same chain
	// The kick-out itself and the stages that set it for sure are held as before.
	CHECK(HostileRows(File::kFallout4, 0x16D036, 0, NOT_STARTING).size() == 1);    // InstKickOut 0
	CHECK(HostileRows(File::kFallout4, 0x16D036, 100, START_UP_0).size() == 2);
	CHECK(HostileRows(File::kFallout4, 0x0229EE, 400, NOT_STARTING).size() == 1);  // MQ302 400: the Institute, for sure
	CHECK(!HostileOf(File::kFallout4, 0x0229EE, 400).front().conditional);
	CHECK(HostileRows(File::kFallout4, 0x15BD39, 30, NOT_STARTING).size() == HostileOf(File::kFallout4, 0x15BD39, 30).size());  // InstMassFusion 30

	// 26 rows, all the Institute's by SetEnemy, each its stage's only row (a stage that also turns the faction for sure
	// has no conditional row), and exactly the stages whose dismissal of X6-88 is conditional (DISMISSES), whose warning
	// is kept for the real kick-out the same way.
	std::size_t marked = 0;
	for (const auto& row : HOSTILE) {
		const auto dismissal = DismissalOf(static_cast<File>(row.file), row.quest, row.stage);
		if (!row.conditional) {
			CHECK(!(dismissal && dismissal->conditional));
			continue;
		}
		++marked;
		CHECK(row.factionFile == 0);
		CHECK(row.faction == INSTITUTE);
		CHECK(row.call == kSetEnemy);
		CHECK(HostileOf(static_cast<File>(row.file), row.quest, row.stage).size() == 1);
		REQUIRE(dismissal != nullptr);
		CHECK(dismissal->conditional);
		// None is a stage that moves the player: QuestSync makes a row's call itself only for those (Verdict::kHostileOnly),
		// and a kick-out run here in a stage's place (KICKOUTS) is never one of these.
		const auto rule = Find(static_cast<File>(row.file), row.quest, row.stage);
		CHECK((!rule || !(rule->flags & kMoves)));
		CHECK(!KickOutOf(static_cast<File>(row.file), row.quest, row.stage));
	}
	CHECK(marked == 26);
}

TEST("hostile hold: the HOSTILE table is sorted, each row a shared quest's stage with a faction in a file of its own")
{
	using namespace StageRules;
	for (auto it = std::next(std::begin(HOSTILE)); it != std::end(HOSTILE); ++it) {
		const auto& before = *std::prev(it);
		CHECK((std::tuple{ before.file, before.quest, before.stage } <= std::tuple{ it->file, it->quest, it->stage }));
		CHECK(it->file <= 3);
		CHECK(it->factionFile <= 3);
		CHECK(it->faction != 0);
		// A stage's rows each name another faction, and an actor put into a PlayerEnemy faction is only ever its first.
		for (auto other = it; other != std::begin(HOSTILE);) {
			--other;
			if (std::tuple{ other->file, other->quest, other->stage } != std::tuple{ it->file, it->quest, it->stage }) {
				break;
			}
			CHECK((std::pair{ other->factionFile, other->faction } != std::pair{ it->factionFile, it->faction }));
			CHECK(it->call != kAddToFaction);
		}
	}
	// The ones the audit of 2026-11-10 named, and the SAFE rows that were kMoves|kHostile before.
	const auto has = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage, File a_factionFile, std::uint32_t a_faction) {
		const auto rows = HostileOf(a_file, a_quest, a_stage);
		return std::ranges::any_of(rows, [&](const Hostile& a_row) { return a_row.factionFile == static_cast<std::uint8_t>(a_factionFile) && a_row.faction == a_faction; });
	};
	CHECK(has(File::kCoast, 0x004C8E, 70, File::kCoast, 0x004462));        // DLC03CoA_DialogueNucleus 70: the Children of Atom (by DLC03CoA_Kickout 700)
	CHECK(has(File::kFallout4, 0x08FCED, 500, File::kFallout4, 0x026DCF));  // DN036 500: Phyllis
	CHECK(has(File::kFallout4, 0x16D036, 100, File::kFallout4, 0x05E558));  // InstKickOut 100: the Institute (MQ00's Factions[3], by hand)
	CHECK(has(File::kFallout4, 0x0C9B0B, 10, File::kFallout4, 0x05DE41));   // MQ00 10: the Brotherhood (a quickstart, never shared: its row is never used)
	CHECK(has(File::kFallout4, 0x0229EE, 400, File::kFallout4, 0x05E558));  // MQ302 400: the Institute
	CHECK(has(File::kCoast, 0x00AEC3, 210, File::kFallout4, 0x106C2F));     // DLC03FarHarborS02 210: Malcolm into PlayerEnemyFaction (Fallout4.esm's)
	CHECK(has(File::kNukaWorld, 0x000802, 1500, File::kNukaWorld, 0x00F438));  // DLC04MQ02 1500: the Disciples (by DLC04RaiderKickout 20)
	CHECK(has(File::kNukaWorld, 0x03684F, 740, File::kNukaWorld, 0x03E4F2));   // Kiddie Kingdom 740: Oswald
	CHECK(has(File::kNukaWorld, 0x03684F, 600, File::kNukaWorld, 0x056F29));   // Kiddie Kingdom 600: the hostile ghouls
	CHECK(HostileOf(File::kNukaWorld, 0x01E34A, 10).size() == 8);  // DLC04GZMainQuest 10: every Galactic Zone enemy faction (by hand)
	// Far Harbor's endings and Nuka-World's raider kick-out and Power Play, shared for their outcomes.
	CHECK(has(File::kCoast, 0x015366, 100, File::kCoast, 0x004462));          // DLC03MQ06_NukeHandling 100: the Children of Atom (by DLC03CoA_Kickout 700)
	CHECK(has(File::kCoast, 0x01AC2B, 50, File::kCoast, 0x005C7E));           // DLC03MQ06_WindFarmHandling 50: Far Harbor's town
	CHECK(has(File::kCoast, 0x0323DE, 700, File::kCoast, 0x004462));          // DLC03CoA_Kickout 700
	CHECK(has(File::kCoast, 0x032C24, 100, File::kCoast, 0x00463D));          // DLC03AcadiaDefense 100: Acadia
	CHECK(has(File::kNukaWorld, 0x027704, 20, File::kNukaWorld, 0x00F438));   // DLC04RaiderKickout 20: the Disciples (the first the scan meets)
	// Every faction a stage turns for sure has a row, not only the first: Nuka-World's kick-out turns the three gangs and
	// the market's guards (DLC04KOQuestScript.DLC04SendKickOut, one SetEnemy after another), so a friend among the
	// Operators or the Pack is held as one among the Disciples is; Open Season's 1500 sets it.
	for (const auto& [quest, stage] : std::initializer_list<std::pair<std::uint32_t, std::uint16_t>>{ { 0x027704, 20 }, { 0x000802, 1500 } }) {
		CHECK(HostileOf(File::kNukaWorld, quest, stage).size() == 4);
		CHECK(HostileOf(File::kNukaWorld, quest, stage).front().faction == 0x00F438);  // the first row is the one it always was
		CHECK(has(File::kNukaWorld, quest, stage, File::kNukaWorld, 0x00F439));  // the Operators
		CHECK(has(File::kNukaWorld, quest, stage, File::kNukaWorld, 0x00F43A));  // the Pack
		CHECK(has(File::kNukaWorld, quest, stage, File::kNukaWorld, 0x042CAA));  // Shank's crime faction (the market's guards)
		for (const auto& row : HostileOf(File::kNukaWorld, quest, stage)) {
			CHECK(row.call == kSetEnemy);  // Gage put into PlayerEnemyFaction (on a condition) is no row
		}
	}
	// The Combat Zone's 250 turns the arena's raiders and its fighters, 245 and 247 set it for sure; 200, 240 and 243
	// set it on a condition only, and keep the one row they had (the fighters are listed by the scan, to be read by hand).
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 245, 247, 250 }) {
		CHECK(HostileOf(File::kFallout4, 0x02074B, stage).size() == 2);
		CHECK(has(File::kFallout4, 0x02074B, stage, File::kFallout4, 0x117D52));  // CZ_Faction
		CHECK(has(File::kFallout4, 0x02074B, stage, File::kFallout4, 0x247303));  // CZ_CombatantFaction
	}
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 200, 240, 243 }) {
		CHECK(HostileOf(File::kFallout4, 0x02074B, stage).size() == 1);
		CHECK(has(File::kFallout4, 0x02074B, stage, File::kFallout4, 0x117D52));
	}
	// The Children of Atom's kick-out turns their interior faction too, and so does every stage that sets it for sure;
	// the nuke's 100 sets it on a condition (one row).
	CHECK(has(File::kCoast, 0x0323DE, 700, File::kCoast, 0x05D37C));
	CHECK(has(File::kCoast, 0x004C8E, 70, File::kCoast, 0x05D37C));
	CHECK(HostileOf(File::kCoast, 0x015366, 100).size() == 1);
	CHECK(HostileOf(File::kNukaWorld, 0x000805, 8).size() == 3);              // DLC04MQ05 8: the snubbed gang, one of the three (by hand)
	CHECK(has(File::kNukaWorld, 0x000805, 8, File::kNukaWorld, 0x00F439));    // the Operators
	CHECK(has(File::kNukaWorld, 0x000805, 8, File::kNukaWorld, 0x00F43A));    // the Pack
	CHECK(HostileOf(File::kNukaWorld, 0x000805, 1).empty());                  // its quickstarts: no row (kMoves alone)
	CHECK(HostileOf(File::kFallout4, 0x04542B, 120).size() == 1);  // DN049 120: one shop's robots
	// Not one: the opening, a stage with no faction, another file's same IDs.
	CHECK(HostileOf(File::kFallout4, 0x01CC2A, 10).empty());
	CHECK(HostileOf(File::kRobot, 0x004C8E, 70).empty());
	// No RULES row carries kHostile any more: the hold comes from this table (QuestSync adds the flag).
	for (const auto& rule : RULES) {
		CHECK(!(rule.flags & kHostile));
	}
	// StartCombat stages have no row (a passing fight): Safari 195, DN092 13.
	CHECK(HostileOf(File::kNukaWorld, 0x0260E2, 195).empty());
	CHECK(HostileOf(File::kFallout4, 0x01D806, 13).empty());
	// How each is made: the fragment's call, so QuestSync can make it without the stage.
	CHECK(HostileOf(File::kFallout4, 0x15BD39, 30).front().call == kSetPlayerEnemy);  // InstMassFusion 30: BoSKickOut 20's SetPlayerEnemy(BrotherhoodofSteelFaction)
	CHECK(HostileOf(File::kFallout4, 0x022A08, 470).front().call == kSetEnemy);       // MS17 470: CovenantFaction.SetEnemy(PlayerFaction)
	CHECK(HostileOf(File::kFallout4, 0x16D036, 100).front().call == kSetEnemy);       // InstKickOut 100: PlayerFaction.SetEnemy(the Institute)
	CHECK(HostileOf(File::kCoast, 0x00AEC3, 210).front().call == kAddToFaction);      // Malcolm into PlayerEnemyFaction
	std::size_t setEnemy = 0, setPlayerEnemy = 0, added = 0;
	for (const auto& row : HOSTILE) {
		setEnemy += row.call == kSetEnemy;
		setPlayerEnemy += row.call == kSetPlayerEnemy;
		added += row.call == kAddToFaction;
	}
	CHECK(setEnemy == 250);
	CHECK(setPlayerEnemy == 34);
	CHECK(added == 23);
}

TEST("hostile hold: a stage that turns one actor on the player by itself is held for that actor (HOSTILE_ACTORS; tools/questscan.ps1 -HostileActors)")
{
	using namespace StageRules;
	const auto key = [](const HostileActor& a_row) { return std::tuple{ a_row.file, a_row.quest, a_row.stage, a_row.holderFile, a_row.holder, a_row.alias }; };
	CHECK(std::ranges::is_sorted(HOSTILE_ACTORS, {}, key));
	CHECK(std::ranges::adjacent_find(HOSTILE_ACTORS, {}, key) == std::end(HOSTILE_ACTORS));
	for (const auto& row : HOSTILE_ACTORS) {
		const auto file = static_cast<File>(row.file);
		CHECK(row.file <= static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK(row.holderFile <= row.file);  // the holder is the quest's own file's, or a master's
		CHECK(row.holder != 0);
		// A developer's quickstart is never shared: no row.
		CHECK(!IsDebug(file, row.quest, row.stage));
		// No row's stage moves the player: none is a mover whose actor could not be turned here without the stage.
		const auto rule = Find(file, row.quest, row.stage);
		CHECK((!rule || !(rule->flags & kMoves)));
		CHECK(ActorsHold(rule ? rule->flags : std::uint8_t{ 0 }));
		// Nor is one a mover that stands for a kick-out: the kick-out's companion is the kick-out's own (DISMISSES).
		CHECK(!KickOutOf(file, row.quest, row.stage));
	}
	// A kick-out's own stages turn its companion (Danse into PlayerEnemyFaction, Gage): HOSTILE's and DISMISSES', no row here.
	CHECK(HostileActorsOf(File::kFallout4, 0x16FF04, 20).empty());   // BoSKickOut 20
	CHECK(HostileActorsOf(File::kFallout4, 0x15BD39, 30).empty());   // InstMassFusion 30, which sets it
	CHECK(HostileActorsOf(File::kNukaWorld, 0x027704, 20).empty());  // DLC04RaiderKickout 20

	// Safari Adventure's 195: Cito (its alias 1), when the talk goes wrong. No HOSTILE row: no faction changes.
	auto rows = HostileActorsOf(File::kNukaWorld, 0x0260E2, 195);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].holderFile == 3);
	CHECK(rows[0].holder == 0x0260E2);
	CHECK(rows[0].alias == 1);
	CHECK(HostileOf(File::kNukaWorld, 0x0260E2, 195).empty());
	// Kiddie Kingdom's 570, 590 and 600: Oswald (its alias 2).
	for (const std::uint16_t stage : std::initializer_list<std::uint16_t>{ 570, 590, 600 }) {
		rows = HostileActorsOf(File::kNukaWorld, 0x03684F, stage);
		REQUIRE(!rows.empty());
		CHECK(std::ranges::any_of(rows, [](const HostileActor& a_row) { return a_row.holder == 0x03684F && a_row.alias == 2; }));
	}
	// Sierra Petrovita's attack: HOSTILE names PlayerEnemyFaction, which she is not in before the stage; here, her alias.
	rows = HostileActorsOf(File::kNukaWorld, 0x035790, 10);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].holder == 0x035790);
	CHECK(rows[0].alias == 0);
	REQUIRE(HostileOf(File::kNukaWorld, 0x035790, 10).size() == 1);
	CHECK(HostileOf(File::kNukaWorld, 0x035790, 10).front().call == kAddToFaction);
	// An actor another quest's alias holds: Restoring Order's 885 starts the Mechanist's fight, DLC01Lair's alias 3.
	rows = HostileActorsOf(File::kRobot, 0x0010F5, 885);
	REQUIRE(!rows.empty());
	CHECK(std::ranges::any_of(rows, [](const HostileActor& a_row) { return a_row.holderFile == 2 && a_row.holder == 0x0008BB && a_row.alias == 3; }));
	// The audit's other stages that went through as any stage: each has its actor now.
	CHECK(!HostileActorsOf(File::kFallout4, 0x01D806, 13).empty());   // DN092 13, 14, 76
	CHECK(!HostileActorsOf(File::kFallout4, 0x01D806, 14).empty());
	CHECK(!HostileActorsOf(File::kFallout4, 0x01D806, 76).empty());
	CHECK(!HostileActorsOf(File::kFallout4, 0x03479A, 26).empty());   // DN102 26, 27
	CHECK(!HostileActorsOf(File::kFallout4, 0x03479A, 27).empty());
	CHECK(!HostileActorsOf(File::kFallout4, 0x168550, 20).empty());   // MS05BPostQuest 20, 30
	CHECK(!HostileActorsOf(File::kFallout4, 0x168550, 30).empty());
	CHECK(!HostileActorsOf(File::kCoast, 0x02C687, 8).empty());       // DLC03_ADV017 8
	CHECK(!HostileActorsOf(File::kNukaWorld, 0x000808, 50).empty());  // DLC04Grandchester 50
	// A stage with no such call: none. Another file's quest of the same ID: none.
	CHECK(HostileActorsOf(File::kFallout4, 0x01CC2A, 10).empty());
	CHECK(HostileActorsOf(File::kFallout4, 0x0260E2, 195).empty());

	// Judged with its quest's start-up stages when the stage starts the quest here, as HOSTILE's rows are, each actor once.
	constexpr std::span<const std::uint16_t> NOT_STARTING{};
	constexpr std::array<std::uint16_t, 1>   STARTS_AT_570{ 570 };
	CHECK(HostileActorRows(File::kNukaWorld, 0x03684F, 590, NOT_STARTING).size() == HostileActorsOf(File::kNukaWorld, 0x03684F, 590).size());
	const auto merged = HostileActorRows(File::kNukaWorld, 0x03684F, 590, STARTS_AT_570);
	CHECK(merged.size() == HostileActorsOf(File::kNukaWorld, 0x03684F, 590).size());  // Oswald, both stages': once
	CHECK(merged.front().stage == 570);                                                // the start-up's row first
	CHECK(HostileActorRows(File::kNukaWorld, 0x03684F, 570, STARTS_AT_570).size() == HostileActorsOf(File::kNukaWorld, 0x03684F, 570).size());  // a start-up stage itself: its own, once
	constexpr std::array<std::uint16_t, 1> STARTS_AT_195{ 195 };
	CHECK(HostileActorRows(File::kNukaWorld, 0x0260E2, 200, STARTS_AT_195).size() == 1);  // a stage with none takes the start-up's
	CHECK(HostileActorRows(File::kNukaWorld, 0x0260E2, 200, NOT_STARTING).empty());

	// The stage is kHostile for its actors unless it moves the player and is no bridge: that one is set beside the friend or not at all.
	CHECK(ActorsHold(0));
	CHECK(ActorsHold(kHostile));
	CHECK(ActorsHold(kMoves | kBridge));
	CHECK(!ActorsHold(kMoves));
	CHECK(!ActorsHold(kMoves | kHostile));
	// And is then judged as a faction's: it waits while the actor is near and the friend is not, live or caught up, and is set once it is not.
	const Rule actorOnly{ 3, 0x0260E2, 195, kHostile, 0 };
	CHECK(Judge(&actorOnly, { .live = true, .nearFriend = false, .factionNear = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&actorOnly, { .live = false, .factionNear = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&actorOnly, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
	CHECK(Judge(&actorOnly, { .live = true, .nearFriend = false, .factionNear = false }) == Verdict::kSet);
	CHECK(Judge(&actorOnly, { .live = false, .factionNear = false }) == Verdict::kSet);
}

TEST("hostile hold: the stages that move the player too (a kMoves row, no bridge) are 5 known that are judged, the rest quickstarts never shared, and each can be made without the stage")
{
	using namespace StageRules;
	// A friend far away: the stage isn't set (it would move the player), its faction is turned on them by the row's call
	// (Verdict::kHostileOnly): one QuestSync makes itself, SetEnemy or SetPlayerEnemy, never an actor put in a faction.
	std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> movers;
	std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> played;
	for (const auto& rule : RULES) {
		if ((rule.flags & kMoves) && !(rule.flags & kBridge)) {
			const auto rows = HostileOf(static_cast<File>(rule.file), rule.quest, rule.stage);
			for (const auto& row : rows) {
				CHECK(row.call != kAddToFaction);
			}
			if (!rows.empty()) {
				movers.emplace_back(rule.file, rule.quest, rule.stage);
				if (!(rule.flags & kDebug)) {
					played.emplace_back(rule.file, rule.quest, rule.stage);
				}
			}
		}
	}
	// The ones play can set, the only ones ever judged kHostileOnly.
	const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> known{
		{ 0, 0x022A08, 465 }, { 0, 0x022A08, 467 }, { 0, 0x022A08, 470 },  // MS17: the Covenant
		{ 0, 0x15BD39, 30 },                                               // InstMassFusion: the Brotherhood's kick-out
		{ 0, 0x16D036, 100 },                                              // InstKickOut: the Institute
	};
	CHECK(played == known);
	// The others are quickstarts, never shared (kDebug), never judged: at least these, each on the way to a faction turned
	// on the player.
	const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> quickstarts{
		{ 0, 0x043275, 1 },  // RR303: the Brotherhood
		{ 0, 0x04402C, 2 }, { 0, 0x04402C, 3 }, { 0, 0x04402C, 4 }, { 0, 0x04402C, 5 }, { 0, 0x04402C, 6 }, { 0, 0x04402C, 7 }, { 0, 0x04402C, 8 }, { 0, 0x04402C, 9 },  // BoSM02: the airport's ghouls
		{ 0, 0x06D646, 1 }, { 0, 0x06D646, 2 }, { 0, 0x06D646, 3 }, { 0, 0x06D646, 4 }, { 0, 0x06D646, 5 }, { 0, 0x06D646, 6 }, { 0, 0x06D646, 7 }, { 0, 0x06D646, 8 }, { 0, 0x06D646, 9 },  // DN084 (Mass Fusion): the Institute on the Brotherhood's path, the Brotherhood's kick-out on the Institute's
		{ 0, 0x0ADCE5, 2 }, { 0, 0x0ADCE5, 3 }, { 0, 0x0ADCE5, 4 },                         // BoSM04: the same as BoSM02
		{ 0, 0x0FDC8C, 1 }, { 0, 0x0FDC8C, 2 }, { 0, 0x0FDC8C, 3 },                         // BoS303 (Mass Fusion, the Brotherhood's side)
		{ 0, 0x142FD3, 1 }, { 0, 0x142FD3, 20 }, { 0, 0x142FD3, 25 }, { 0, 0x142FD3, 30 },  // RR304: the Brotherhood (play starts the quest at its 100)
		{ 0, 0x174029, 1 }, { 0, 0x174029, 2 }, { 0, 0x174029, 3 }, { 0, 0x174029, 4 }, { 0, 0x174029, 5 }, { 0, 0x174029, 6 }, { 0, 0x174029, 7 }, { 0, 0x174029, 8 },  // Inst307Fight: its 1, which its 2 to 8 go through, runs the Brotherhood's kick-out
		{ 2, 0x0008BB, 8 },  // the Mechanist's lair's quickstart: its robots (by 1350)
		{ 3, 0x03684F, 6 },  // Kiddie Kingdom's quickstart: the hostile ghouls
	};
	for (const auto& quickstart : quickstarts) {
		CHECK(std::ranges::find(movers, quickstart) != movers.end());
		CHECK(IsDebug(static_cast<File>(std::get<0>(quickstart)), std::get<1>(quickstart), std::get<2>(quickstart)));
	}
	CHECK(movers.size() >= known.size() + quickstarts.size());
	// The start-up stages that are HOSTILE's and whose quest's rows are kMoves are judged with them (Starting): none of
	// the two (InstKickOut 0, DN079 0) is a mover itself.
	CHECK(!Find(File::kFallout4, 0x16D036, 0));
	CHECK(!Find(File::kFallout4, 0x02633E, 0));
}

TEST("hostile hold: the movers that are a faction's kick-out run the kick-out itself here, not gone with the friend (KICKOUTS)")
{
	using namespace StageRules;
	// Each is one of the 5 movers (a kMoves row, no bridge, HOSTILE's) that are judged (none marked a developer's quickstart),
	// and its kick-out is a quest of the same file.
	for (const auto& row : KICKOUTS) {
		const auto rule = Find(static_cast<File>(row.file), row.quest, row.stage);
		REQUIRE(rule != nullptr);
		CHECK(rule->flags == kMoves);
		CHECK(!IsDebug(static_cast<File>(row.file), row.quest, row.stage));
		CHECK(!HostileOf(static_cast<File>(row.file), row.quest, row.stage).empty());
		CHECK(KickOutOf(static_cast<File>(row.file), row.quest, row.stage) == &row);
	}
	// The Brotherhood's kick-out, BoSKickOut 20, for InstMassFusion 30; the Institute's is its quest's start-up.
	const auto massFusion = KickOutOf(File::kFallout4, 0x15BD39, 30);
	REQUIRE(massFusion != nullptr);
	CHECK(massFusion->kickQuest == 0x16FF04 && massFusion->kickStage == 20);
	const auto institute = KickOutOf(File::kFallout4, 0x16D036, 100);
	REQUIRE(institute != nullptr);
	CHECK(institute->kickQuest == 0x16D036 && institute->kickStage == 0);
	// The developers' quickstarts that reach a kick-out (Mass Fusion's DN084 1 to 9 and BoS303 0 to 3, the airport
	// battle's Inst307Fight 0 to 8, the Railroad's RR303 1 and RR304 1, 20, 25 and 30) have no row: a stage only the
	// console sets runs no kick-out in a friend's game. RR304 20 had one once, taken for the stage Rockets' Red Glare
	// reaches in play: play starts that quest at its 100, and nothing but the console sets its 20.
	for (std::uint16_t stage = 1; stage <= 9; ++stage) {
		CHECK(!KickOutOf(File::kFallout4, 0x06D646, stage));
	}
	for (std::uint16_t stage = 0; stage <= 8; ++stage) {
		CHECK(!KickOutOf(File::kFallout4, 0x174029, stage));
	}
	for (std::uint16_t stage = 0; stage <= 3; ++stage) {
		CHECK(!KickOutOf(File::kFallout4, 0x0FDC8C, stage));
	}
	CHECK(!KickOutOf(File::kFallout4, 0x043275, 1));
	for (const std::uint16_t stage : { std::uint16_t{ 1 }, std::uint16_t{ 20 }, std::uint16_t{ 25 }, std::uint16_t{ 30 } }) {
		CHECK(!KickOutOf(File::kFallout4, 0x142FD3, stage));
	}
	// The Brotherhood's kick-out through InstMassFusion 30 is a SetPlayerEnemy row: with the kick-out run here, no mover a
	// friend's game can send has a bare call that is one (a plain SetPlayerEnemy would leave the player in the faction and
	// Danse at their side). Only a stage marked a quickstart (kDebug) is let off, and only because it is never shared: it
	// is never judged kHostileOnly, so its row's call is never made (the next test).
	std::size_t bare = 0;
	for (const auto& rule : RULES) {
		if ((rule.flags & kMoves) && !(rule.flags & kBridge) && !KickOutOf(static_cast<File>(rule.file), rule.quest, rule.stage)) {
			for (const auto& row : HostileOf(static_cast<File>(rule.file), rule.quest, rule.stage)) {
				if (rule.flags & kDebug) {
					bare += row.call == kSetPlayerEnemy;
					continue;
				}
				CHECK(row.call == kSetEnemy);
			}
		}
	}
	CHECK(bare > 0);  // the marking is what lets those through: without it the check above would fail for them
	// The other 3 movers that are judged have none: the Covenant has no companion and no quests to
	// fail. Nor have the airport's ferals, the lair's robots and the park's ghouls (quickstarts, never shared).
	CHECK(!KickOutOf(File::kFallout4, 0x022A08, 470));
	CHECK(!KickOutOf(File::kFallout4, 0x04402C, 2));
	CHECK(!KickOutOf(File::kNukaWorld, 0x03684F, 6));
	CHECK(std::size(KICKOUTS) == 2);  // the two that play can set; the airport battle's 1 and RR304's 20 are quickstarts
}

TEST("hostile hold: a developer's quickstart is never shared: skipped whatever the situation, with no kick-out and no faction turned (kDebug)")
{
	using namespace StageRules;
	// The 180 in the scan (RuleScan's DEBUG): Mass Fusion's DN084 1 to 9 and BoS303 0 to 3, the airport battle's 0 to 8,
	// the others on the way to a faction's kick-out that nothing but the console sets (these 47 first), and every other
	// quickstart whose designer's note says so that sets a stage a friend's game would be sent, DLC04Grandchester 0,
	// a move alone, and the quickstarts of the quests shared since (CZ 1, RRAct3Pickup 1-2, DLC01Lair 3-9, DLC04MQ05 0-3 and
	// Far Harbor's three) and MQ105 0; the 20 developers' stages that only move the player (their warps, teleports and
	// stage 0 quickstarts: 200 in the scan now); and the four stage 0s of SafeScan.QuickStart (MS07, BoSM04Postquest,
	// DN154, MS05BTapes): 204.
	std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> debug;
	for (const auto& rule : RULES) {
		if (rule.flags & kDebug) {
			debug.emplace_back(rule.file, rule.quest, rule.stage);
		}
	}
	const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> first{
		{ 0, 0x0229EE, 0 }, { 0, 0x0229EE, 1 }, { 0, 0x0229EE, 2 }, { 0, 0x0229EE, 3 },  // MQ302
		{ 0, 0x043275, 1 }, { 0, 0x043275, 2 }, { 0, 0x043275, 3 }, { 0, 0x043275, 4 },  // RR303
		{ 0, 0x06D646, 1 }, { 0, 0x06D646, 2 }, { 0, 0x06D646, 3 }, { 0, 0x06D646, 4 }, { 0, 0x06D646, 5 }, { 0, 0x06D646, 6 }, { 0, 0x06D646, 7 }, { 0, 0x06D646, 8 }, { 0, 0x06D646, 9 },  // DN084
		{ 0, 0x0B1752, 0 },                                                                                  // MQ206
		{ 0, 0x0C9B0B, 10 }, { 0, 0x0C9B0B, 30 }, { 0, 0x0C9B0B, 40 }, { 0, 0x0C9B0B, 50 }, { 0, 0x0C9B0B, 60 },  // MQ00
		{ 0, 0x0DDFDB, 0 }, { 0, 0x0DDFDB, 1 },                                                              // Min301
		{ 0, 0x0DFB3C, 0 },                                                                                  // MinDestBoS
		{ 0, 0x0FDC8C, 0 }, { 0, 0x0FDC8C, 1 }, { 0, 0x0FDC8C, 2 }, { 0, 0x0FDC8C, 3 },                      // BoS303
		{ 0, 0x10C64C, 10 },                                                                                 // MQ302RR
		{ 0, 0x142FD3, 1 }, { 0, 0x142FD3, 2 }, { 0, 0x142FD3, 20 }, { 0, 0x142FD3, 25 }, { 0, 0x142FD3, 26 }, { 0, 0x142FD3, 27 }, { 0, 0x142FD3, 30 },  // RR304
		{ 0, 0x174029, 0 }, { 0, 0x174029, 1 }, { 0, 0x174029, 2 }, { 0, 0x174029, 3 }, { 0, 0x174029, 4 }, { 0, 0x174029, 5 }, { 0, 0x174029, 6 }, { 0, 0x174029, 7 }, { 0, 0x174029, 8 },  // Inst307Fight
	};
	for (const auto& quickstart : first) {
		CHECK(std::ranges::find(debug, quickstart) != debug.end());
	}
	CHECK(debug.size() == 204);
	// Each is skipped in every situation a friend's stage can meet: next to the friend or far, live or caught up, the
	// faction near or not, the story shared or not, the player warned or offered. Never set, never kHostileOnly (which
	// would make its row's bare call), never held for later.
	for (const auto& [file, quest, stage] : debug) {
		CHECK(IsDebug(static_cast<File>(file), quest, stage));
		CHECK(!KickOutOf(static_cast<File>(file), quest, stage));
		const auto rule = Find(static_cast<File>(file), quest, stage);
		REQUIRE(rule != nullptr);
		for (unsigned bits = 0; bits < 256; ++bits) {
			Situation now;
			now.sharedStory = (bits & 1) != 0;
			now.live = (bits & 2) != 0;
			now.nearFriend = (bits & 4) != 0;
			now.factionNear = (bits & 8) != 0;
			now.held = (bits & 16) != 0;
			now.followable = (bits & 32) != 0;
			if (bits & 64) {
				now.warned = WARNING;
			}
			if (bits & 128) {
				now.offered = std::chrono::seconds(1);
			}
			CHECK(Judge(rule, now) == Verdict::kSkip);
		}
	}
	// The stages of the same quests that play sets are not: Mass Fusion's 0 and 10 on, the airport battle's 10 on.
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 0));
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 10));
	CHECK(!IsDebug(File::kFallout4, 0x06D646, 225));
	CHECK(!IsDebug(File::kFallout4, 0x174029, 10));
	// Only Fallout4.esm's rows: the same form IDs in another file are other quests.
	CHECK(!IsDebug(File::kCoast, 0x06D646, 2));
	// No stage without a row, and no kick-out's own stage.
	CHECK(!IsDebug(File::kFallout4, 0x16FF04, 20));
	CHECK(!IsDebug(File::kFallout4, 0x16D036, 100));
}

TEST("hostile hold: the base game's kick-outs and faction outcomes are HOSTILE's, and the ones that dismiss a companion name them (DISMISSES; tools/questscan.ps1 -Hostile)")
{
	using namespace StageRules;
	const auto has = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_faction) {
		const auto rows = HostileOf(a_file, a_quest, a_stage);
		return std::ranges::any_of(rows, [&](const Hostile& a_row) { return a_row.factionFile == 0 && a_row.faction == a_faction; });
	};
	constexpr std::uint32_t BROTHERHOOD = 0x05DE41, RAILROAD = 0x0994F6, INSTITUTE = 0x05E558;
	// The Brotherhood's kick-out (BoSKickOut 20, and 30 for Mass Fusion's), and what sets it: the airport battle's quickstart Inst307Fight 1 (never shared),
	// InstMassFusion 30, Mass Fusion's quickstart DN084 2 (never shared); the Railroad's (RRKickOut 100) and BoS302B 30, which sets it.
	CHECK(has(File::kFallout4, 0x16FF04, 20, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x16FF04, 30, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x174029, 1, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x15BD39, 30, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x06D646, 2, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x16C207, 100, RAILROAD));
	CHECK(has(File::kFallout4, 0x107A1C, 30, RAILROAD));
	// Danse's exile (BoSDansePostGame 20: the Brotherhood hostile while he follows), Eddie Winter's 20, the D.B. Technical boss
	// room's 510; Mass Fusion's Brotherhood path turns the Institute (DN084 1, by BoS303 15).
	CHECK(has(File::kFallout4, 0x01E5E7, 20, BROTHERHOOD));
	CHECK(has(File::kFallout4, 0x144FCA, 20, 0x14BF23));
	CHECK(has(File::kFallout4, 0x07E1BB, 510, 0x19A782));
	CHECK(has(File::kFallout4, 0x06D646, 1, INSTITUTE));
	// A settlement given (DN117), the soft expulsion (no faction turned), the final battles' 200 (they set DN084 450): no row.
	CHECK(HostileOf(File::kFallout4, 0x082615, 10).empty());
	CHECK(HostileOf(File::kFallout4, 0x16426D, 10).empty());
	CHECK(HostileOf(File::kFallout4, 0x0F8D39, 200).empty());
	CHECK(HostileOf(File::kFallout4, 0x0F8D3A, 200).empty());

	// DISMISSES: sorted, each a HOSTILE stage, each naming a companion's quest (COM*) and its alias: the companion the stage
	// dismisses, whom QuestSync warns the player about once when they are the one following (a warning, not a hold).
	constexpr std::uint32_t COM_DANSE = 0x0BBD97, COM_DEACON = 0x0BBD9B, COM_X688 = 0x0BBD95;
	for (auto it = std::begin(DISMISSES); it != std::end(DISMISSES); ++it) {
		CHECK(!HostileOf(static_cast<File>(it->file), it->quest, it->stage).empty());
		CHECK(it->companionFile <= 3);
		CHECK(it->companionQuest != 0);
		CHECK(DismissalOf(static_cast<File>(it->file), it->quest, it->stage) == &*it);
		if (it != std::begin(DISMISSES)) {
			const auto& before = *std::prev(it);
			CHECK((std::tuple{ before.file, before.quest, before.stage } < std::tuple{ it->file, it->quest, it->stage }));
		}
	}
	const auto dismisses = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage, std::uint32_t a_companionQuest) {
		const auto row = DismissalOf(a_file, a_quest, a_stage);
		return row && row->companionFile == 0 && row->companionQuest == a_companionQuest && row->alias == 0;
	};
	// The kick-outs dismiss the faction's companion (Danse, Deacon, X6-88): themselves, and every stage that sets one.
	CHECK(dismisses(File::kFallout4, 0x16FF04, 20, COM_DANSE));   // BoSKickOut 20
	CHECK(dismisses(File::kFallout4, 0x16FF04, 30, COM_DANSE));   // BoSKickOut 30
	CHECK(dismisses(File::kFallout4, 0x16C207, 100, COM_DEACON));  // RRKickOut 100
	CHECK(dismisses(File::kFallout4, 0x16D036, 0, COM_X688));     // InstKickOut 0
	CHECK(dismisses(File::kFallout4, 0x174029, 1, COM_DANSE));    // Inst307Fight 1 (BoSKickOut 20): a quickstart, never shared, so its row is never used
	CHECK(dismisses(File::kFallout4, 0x15BD39, 30, COM_DANSE));   // InstMassFusion 30 (BoSKickOut 20)
	CHECK(dismisses(File::kFallout4, 0x06D646, 2, COM_DANSE));    // DN084 2 (BoSKickOut 30): a quickstart, never shared, so its row is never used
	CHECK(dismisses(File::kFallout4, 0x107A1C, 30, COM_DEACON));  // BoS302B 30 (RRKickOut 100)
	CHECK(dismisses(File::kFallout4, 0x043275, 1, COM_DANSE));    // RR303 1 (BoSKickOut 20): a quickstart, never shared, so its row is never used
	CHECK(dismisses(File::kFallout4, 0x03A457, 70, COM_X688));    // Min02 70 (the Institute's, through the Minutemen's script)
	// Every KICKOUTS mover's kick-out is a DISMISSES row too, and a sure one: the warning is given before the kick-out is run here.
	for (const auto& kick : KICKOUTS) {
		const auto row = DismissalOf(static_cast<File>(kick.file), kick.kickQuest, kick.kickStage);
		REQUIRE(row != nullptr);
		CHECK(!row->conditional);
	}
	// A faction turned with no companion dismissed: Danse's exile, Eddie Winter, Phyllis, MQ00's helper (it sets the globals and
	// SetEnemy only), the Nucleus's kick-out.
	CHECK(!DismissalOf(File::kFallout4, 0x01E5E7, 20));
	CHECK(!DismissalOf(File::kFallout4, 0x144FCA, 20));
	CHECK(!DismissalOf(File::kFallout4, 0x08FCED, 500));
	CHECK(!DismissalOf(File::kFallout4, 0x0C9B0B, 10));
	CHECK(!DismissalOf(File::kCoast, 0x004C8E, 70));
	// Not a HOSTILE stage at all.
	CHECK(!DismissalOf(File::kFallout4, 0x082615, 10));
	CHECK(!DismissalOf(File::kRobot, 0x16FF04, 20));
}

TEST("hostile hold: a stage that dismisses a companion only on a condition is marked, and gives no warning (DISMISSES' conditional; tools/questscan.ps1 -Hostile)")
{
	using namespace StageRules;
	const auto conditional = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage) {
		const auto row = DismissalOf(a_file, a_quest, a_stage);
		REQUIRE(row != nullptr);
		return row->conditional;
	};
	// The kick-outs and the stages that set one outright dismiss the companion in every game.
	CHECK(!conditional(File::kFallout4, 0x16FF04, 20));   // BoSKickOut 20
	CHECK(!conditional(File::kFallout4, 0x16FF04, 30));   // BoSKickOut 30
	CHECK(!conditional(File::kFallout4, 0x16C207, 100));  // RRKickOut 100
	CHECK(!conditional(File::kFallout4, 0x16D036, 0));    // InstKickOut's start-up
	CHECK(!conditional(File::kFallout4, 0x174029, 1));    // Inst307Fight 1
	CHECK(!conditional(File::kFallout4, 0x15BD39, 30));   // InstMassFusion 30
	CHECK(!conditional(File::kFallout4, 0x107A1C, 30));   // BoS302B 30
	CHECK(!conditional(File::kFallout4, 0x043275, 1));    // RR303 1 (a quickstart: never used)
	// The Minutemen's and the story's stages that only move the Minutemen on reach the Institute's kick-out through
	// MinutemenCentralScript.CheckForMQAdvance, which starts it only when the Minutemen are the story's last way on:
	// X6-88 stays in nearly every game that sets them.
	CHECK(conditional(File::kFallout4, 0x03A457, 70));    // Min02 70 (Taking Independence)
	CHECK(conditional(File::kFallout4, 0x03A457, 600));   // Min02 600
	CHECK(conditional(File::kFallout4, 0x0229EE, 2));     // MQ302 2
	CHECK(conditional(File::kFallout4, 0x0229EE, 1000));  // MQ302 1000
	std::size_t marked = 0;
	for (const auto& row : DISMISSES) {
		marked += row.conditional;
		// All of them X6-88's: only the Institute's kick-out is behind that condition.
		if (row.conditional) {
			CHECK(row.companionFile == 0);
			CHECK(row.companionQuest == 0x0BBD95);
		}
	}
	// 60 from the scan's -Hostile, with MQ302Post 10 (conditional: it sets MQ302Min 2000) and Nuka-World's raider kick-out
	// 20 (Gage's, not conditional), whose quests are shared since.
	CHECK(conditional(File::kFallout4, 0x215CC7, 10));    // MQ302Post 10
	CHECK(!conditional(File::kNukaWorld, 0x027704, 20));  // DLC04RaiderKickout 20
	CHECK(std::size(DISMISSES) == 62);
	CHECK(marked == 26);
}

TEST("hostile hold: a conditional dismissal's false warning doesn't use up the companion's real one (DismissalWarnings)")
{
	using namespace StageRules;
	constexpr std::uint32_t X688 = 0x000BBDA0, DANSE = 0x0005DE4D;  // stand-ins for two companions' form IDs
	const auto min02 = DismissalOf(File::kFallout4, 0x03A457, 70);       // conditional: X6-88, through the Minutemen's script
	const auto kickOut = DismissalOf(File::kFallout4, 0x16D036, 0);      // the Institute's kick-out: X6-88, for sure
	const auto brotherhood = DismissalOf(File::kFallout4, 0x16FF04, 20);  // the Brotherhood's: Danse
	REQUIRE(min02 != nullptr);
	REQUIRE(kickOut != nullptr);
	REQUIRE(brotherhood != nullptr);
	REQUIRE(min02->conditional);
	REQUIRE(!kickOut->conditional);

	DismissalWarnings warnings;
	// A friend takes the Castle, then more of the Minutemen's stages: X6-88 follows the player and stays. No line.
	CHECK(!warnings.Due(*min02, X688));
	CHECK(!warnings.Due(*min02, X688));
	// Later in the session the friend is thrown out of the Institute: the kick-out's warning is still there, once.
	CHECK(warnings.Due(*kickOut, X688));
	CHECK(!warnings.Due(*kickOut, X688));
	CHECK(!warnings.Due(*min02, X688));
	// Each companion has their own: Danse's is not spent by X6-88's.
	CHECK(warnings.Due(*brotherhood, DANSE));
	CHECK(!warnings.Due(*brotherhood, DANSE));
	// A new session (or a load) starts over; a conditional row still gives none.
	warnings.Clear();
	CHECK(!warnings.Due(*min02, X688));
	CHECK(warnings.Due(*kickOut, X688));
	CHECK(warnings.Due(*brotherhood, DANSE));
}

TEST("hostile hold: the dismissal's warning names the faction whose kick-out drops the companion, not whichever the stage turns on the way (DismissalFaction)")
{
	using namespace StageRules;
	constexpr std::uint32_t BROTHERHOOD = 0x05DE41, RAILROAD = 0x0994F6, INSTITUTE = 0x05E558;
	const auto factionOf = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage) -> std::pair<std::uint8_t, std::uint32_t> {
		const auto row = DismissalOf(a_file, a_quest, a_stage);
		REQUIRE(row != nullptr);
		const auto named = DismissalFaction(*row);
		REQUIRE(named != nullptr);
		return { named->factionFile, named->faction };
	};
	CHECK((factionOf(File::kFallout4, 0x16FF04, 20) == std::pair<std::uint8_t, std::uint32_t>{ 0, BROTHERHOOD }));  // BoSKickOut: Danse
	CHECK((factionOf(File::kFallout4, 0x16C207, 100) == std::pair<std::uint8_t, std::uint32_t>{ 0, RAILROAD }));    // RRKickOut: Deacon
	CHECK((factionOf(File::kFallout4, 0x16D036, 0) == std::pair<std::uint8_t, std::uint32_t>{ 0, INSTITUTE }));     // InstKickOut: X6-88
	// InstMassFusion 30 turns the Brotherhood on the player and the kick-out drops Danse: the Brotherhood, whatever
	// else the stage's rows hold.
	CHECK((factionOf(File::kFallout4, 0x15BD39, 30) == std::pair<std::uint8_t, std::uint32_t>{ 0, BROTHERHOOD }));
	CHECK((factionOf(File::kFallout4, 0x174029, 1) == std::pair<std::uint8_t, std::uint32_t>{ 0, BROTHERHOOD }));
	// Every row names one, a faction of the companion's own file, and one per companion quest.
	for (const auto& row : DISMISSES) {
		const auto named = DismissalFaction(row);
		REQUIRE(named != nullptr);
		CHECK(named->faction != 0);
		CHECK(named->factionFile == row.companionFile);
	}
	for (auto it = std::begin(COMPANION_FACTIONS); it != std::end(COMPANION_FACTIONS); ++it) {
		for (auto other = std::next(it); other != std::end(COMPANION_FACTIONS); ++other) {
			CHECK((std::pair{ it->companionFile, it->companionQuest } != std::pair{ other->companionFile, other->companionQuest }));
		}
	}
	// That faction is one the stage turns on the player (HOSTILE), but for the stages that reach the kick-out through
	// another quest on a condition and turn only another faction for sure themselves: BoS303's quickstarts 1 and 2 (the
	// police station's feral ghouls), never shared. The first HOSTILE row of such a stage is what the line named before,
	// and it is not the companion's faction (nor is Mass Fusion's quickstart 8's, its lab's security: the Brotherhood,
	// which its kick-out turns for sure, is a further row of it).
	const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> elsewhere{ { 0, 0x0FDC8C, 1 }, { 0, 0x0FDC8C, 2 } };
	std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> found;
	for (const auto& row : DISMISSES) {
		const auto named = DismissalFaction(row);
		const auto rows = HostileOf(static_cast<File>(row.file), row.quest, row.stage);
		if (!std::ranges::any_of(rows, [&](const Hostile& a_row) { return a_row.factionFile == named->factionFile && a_row.faction == named->faction; })) {
			found.emplace_back(row.file, row.quest, row.stage);
		}
	}
	CHECK(found == elsewhere);
}
