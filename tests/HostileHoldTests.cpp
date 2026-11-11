#include "Test.h"

#include "game/HostileHold.h"
#include "game/StageRules.h"

#include <chrono>
#include <cstddef>
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
	// InstKickOut 100 starting the quest here: the Institute twice (the start-up's row first, then its own); running already: its own.
	auto rows = HostileRows(File::kFallout4, 0x16D036, 100, true);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].stage == 0);
	CHECK(rows[1].stage == 100);
	CHECK(rows[0].faction == 0x05E558 && rows[1].faction == 0x05E558);
	rows = HostileRows(File::kFallout4, 0x16D036, 100, false);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].stage == 100);
	// DN079's 520 (the cannery's people) starting the quest: its 0's turrets too.
	rows = HostileRows(File::kFallout4, 0x02633E, 520, true);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].faction == 0x19EC30);
	CHECK(rows[1].faction == 0x03EFAE);
	// Stage 0 itself: once. A stage of a quest whose start-up turns nobody: its own alone. Nothing: nothing.
	CHECK(HostileRows(File::kFallout4, 0x16D036, 0, true).size() == 1);
	CHECK(HostileRows(File::kCoast, 0x004C8E, 70, true).size() == 1);
	CHECK(HostileRows(File::kFallout4, 0x01CC2A, 10, true).empty());
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
	}
	// The ones the audit of 2026-11-10 named, and the SAFE rows that were kMoves|kHostile before.
	const auto has = [](File a_file, std::uint32_t a_quest, std::uint16_t a_stage, File a_factionFile, std::uint32_t a_faction) {
		const auto rows = HostileOf(a_file, a_quest, a_stage);
		return std::ranges::any_of(rows, [&](const Hostile& a_row) { return a_row.factionFile == static_cast<std::uint8_t>(a_factionFile) && a_row.faction == a_faction; });
	};
	CHECK(has(File::kCoast, 0x004C8E, 70, File::kCoast, 0x004462));        // DLC03CoA_DialogueNucleus 70: the Children of Atom (by DLC03CoA_Kickout 700)
	CHECK(has(File::kFallout4, 0x08FCED, 500, File::kFallout4, 0x026DCF));  // DN036 500: Phyllis
	CHECK(has(File::kFallout4, 0x16D036, 100, File::kFallout4, 0x05E558));  // InstKickOut 100: the Institute (MQ00's Factions[3], by hand)
	CHECK(has(File::kFallout4, 0x0C9B0B, 10, File::kFallout4, 0x05DE41));   // MQ00 10: the Brotherhood
	CHECK(has(File::kFallout4, 0x0229EE, 400, File::kFallout4, 0x05E558));  // MQ302 400: the Institute
	CHECK(has(File::kCoast, 0x00AEC3, 210, File::kFallout4, 0x106C2F));     // DLC03FarHarborS02 210: Malcolm into PlayerEnemyFaction (Fallout4.esm's)
	CHECK(has(File::kNukaWorld, 0x000802, 1500, File::kNukaWorld, 0x00F438));  // DLC04MQ02 1500: the Disciples (by DLC04RaiderKickout 20)
	CHECK(has(File::kNukaWorld, 0x03684F, 740, File::kNukaWorld, 0x03E4F2));   // Kiddie Kingdom 740: Oswald
	CHECK(has(File::kNukaWorld, 0x03684F, 600, File::kNukaWorld, 0x056F29));   // Kiddie Kingdom 600: the hostile ghouls
	CHECK(HostileOf(File::kNukaWorld, 0x01E34A, 10).size() == 8);  // DLC04GZMainQuest 10: every Galactic Zone enemy faction (by hand)
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
	CHECK(setEnemy == 188);
	CHECK(setPlayerEnemy == 10);
	CHECK(added == 22);
}

TEST("hostile hold: the stages that move the player too (a kMoves row, no bridge) are the 18 known, and each can be made without the stage")
{
	using namespace StageRules;
	// A friend far away: the stage isn't set (it would move the player), its faction is turned on them by the row's call
	// (Verdict::kHostileOnly): one QuestSync makes itself, SetEnemy or SetPlayerEnemy, never an actor put in a faction.
	std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> movers;
	for (const auto& rule : RULES) {
		if ((rule.flags & kMoves) && !(rule.flags & kBridge)) {
			for (const auto& row : HostileOf(static_cast<File>(rule.file), rule.quest, rule.stage)) {
				CHECK(row.call != kAddToFaction);
				movers.emplace_back(rule.file, rule.quest, rule.stage);
			}
		}
	}
	const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint16_t>> known{
		{ 0, 0x022A08, 465 }, { 0, 0x022A08, 467 }, { 0, 0x022A08, 470 },  // MS17: the Covenant
		{ 0, 0x04402C, 2 }, { 0, 0x04402C, 3 }, { 0, 0x04402C, 4 }, { 0, 0x04402C, 5 }, { 0, 0x04402C, 6 }, { 0, 0x04402C, 7 }, { 0, 0x04402C, 8 }, { 0, 0x04402C, 9 },  // BoSM02: the airport's ghouls
		{ 0, 0x0ADCE5, 2 }, { 0, 0x0ADCE5, 3 }, { 0, 0x0ADCE5, 4 },  // BoSM04: the same
		{ 0, 0x142FD3, 20 },                                        // RR304: the Brotherhood
		{ 0, 0x15BD39, 30 },                                        // InstMassFusion: the Brotherhood's kick-out
		{ 0, 0x16D036, 100 },                                       // InstKickOut: the Institute
		{ 3, 0x03684F, 6 },                                         // Kiddie Kingdom's quickstart: the hostile ghouls
	};
	CHECK(movers == known);
	// The start-up stages that are HOSTILE's and whose quest's rows are kMoves are judged with them (Starting): none of
	// the two (InstKickOut 0, DN079 0) is a mover itself.
	CHECK(!Find(File::kFallout4, 0x16D036, 0));
	CHECK(!Find(File::kFallout4, 0x02633E, 0));
}

TEST("hostile hold: the three movers that are a faction's kick-out run the kick-out itself here, not gone with the friend (KICKOUTS)")
{
	using namespace StageRules;
	// Each is one of the 18 movers (a kMoves row, no bridge, HOSTILE's), and its kick-out is a quest of the same file.
	for (const auto& row : KICKOUTS) {
		const auto rule = Find(static_cast<File>(row.file), row.quest, row.stage);
		REQUIRE(rule != nullptr);
		CHECK((rule->flags & (kMoves | kBridge)) == kMoves);
		CHECK(!HostileOf(static_cast<File>(row.file), row.quest, row.stage).empty());
		CHECK(KickOutOf(static_cast<File>(row.file), row.quest, row.stage) == &row);
	}
	// The Brotherhood's kick-out, BoSKickOut 20, for InstMassFusion 30 and RR304 20; the Institute's is its quest's start-up.
	const auto massFusion = KickOutOf(File::kFallout4, 0x15BD39, 30);
	REQUIRE(massFusion != nullptr);
	CHECK(massFusion->kickQuest == 0x16FF04 && massFusion->kickStage == 20);
	const auto rr304 = KickOutOf(File::kFallout4, 0x142FD3, 20);
	REQUIRE(rr304 != nullptr);
	CHECK(rr304->kickQuest == 0x16FF04 && rr304->kickStage == 20);
	const auto institute = KickOutOf(File::kFallout4, 0x16D036, 100);
	REQUIRE(institute != nullptr);
	CHECK(institute->kickQuest == 0x16D036 && institute->kickStage == 0);
	// The Brotherhood's kick-out through InstMassFusion 30 is a SetPlayerEnemy row: with the kick-out run here, no mover's
	// bare call is one (a plain SetPlayerEnemy would leave the player in the faction and Danse at their side).
	for (const auto& rule : RULES) {
		if ((rule.flags & kMoves) && !(rule.flags & kBridge) && !KickOutOf(static_cast<File>(rule.file), rule.quest, rule.stage)) {
			for (const auto& row : HostileOf(static_cast<File>(rule.file), rule.quest, rule.stage)) {
				CHECK(row.call == kSetEnemy);
			}
		}
	}
	// The other 15 movers have none: the Covenant, the airport's ferals and the park's ghouls have no companion and no quests to fail.
	CHECK(!KickOutOf(File::kFallout4, 0x022A08, 470));
	CHECK(!KickOutOf(File::kFallout4, 0x04402C, 2));
	CHECK(!KickOutOf(File::kNukaWorld, 0x03684F, 6));
	CHECK(std::size(KICKOUTS) == 3);
}
