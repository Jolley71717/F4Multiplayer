#include "Test.h"

#include "game/HostileHold.h"
#include "game/StageRules.h"

#include <chrono>

namespace
{
	using namespace std::chrono_literals;
	using HostileHold::Clock;
	using StageRules::Verdict;

	constexpr std::uint64_t KEY = (0x0323DEull << 16) | 700;  // DLC03CoA_Kickout 700
	constexpr std::uint64_t OTHER = (0x16D036ull << 16) | 100;
}

TEST("hostile hold: a member of the faction is near when alive and within RANGE")
{
	using HostileHold::Near;
	using HostileHold::RANGE;
	CHECK(Near(0.0f, 0.0f, 0.0f, true));
	CHECK(Near(RANGE, 0.0f, 0.0f, true));
	CHECK(Near(5000.0f, 5000.0f, 0.0f, true));  // 7071 u
	CHECK(!Near(RANGE + 1.0f, 0.0f, 0.0f, true));
	CHECK(!Near(6000.0f, 6000.0f, 0.0f, true));  // 8485 u
	CHECK(!Near(0.0f, 0.0f, 0.0f, false));  // dead
	CHECK(!Near(0.0f, 0.0f, RANGE + 1.0f, true));  // straight up, too far
	static_assert(RANGE > StageRules::NEAR_FRIEND);  // farther than "beside the friend": a faction that near comes at the player
}

TEST("hostile hold: a held stage's wait counts play only, and a line is due as it begins and every LOG_EVERY after")
{
	HostileHold::Waits waits;
	const auto         t0 = Clock::time_point{} + 1h;
	waits.Tick(t0, false);
	CHECK(waits.Size() == 0);

	auto judged = waits.Hold(KEY);
	CHECK(judged.begun);
	CHECK(judged.logDue);
	CHECK(judged.played == 0s);
	CHECK(waits.Holds(KEY));
	CHECK(!waits.Holds(OTHER));

	// 10 s of play: judged again, not begun, no line due yet.
	waits.Tick(t0 + 10s, false);
	judged = waits.Hold(KEY);
	CHECK(!judged.begun);
	CHECK(!judged.logDue);
	CHECK(judged.played == 10s);

	// 4 minutes paused (a menu, a loading screen): nothing counted.
	waits.Tick(t0 + 10s + 4min, true);
	judged = waits.Hold(KEY);
	CHECK(judged.played == 10s);
	CHECK(!judged.logDue);

	// LOG_EVERY of play since the hold began: a line.
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY - 10s, false);
	judged = waits.Hold(KEY);
	CHECK(judged.played == HostileHold::LOG_EVERY);
	CHECK(judged.logDue);
	// ... and not again at once.
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY, false);
	judged = waits.Hold(KEY);
	CHECK(!judged.logDue);
	CHECK(judged.played == HostileHold::LOG_EVERY + 10s);

	// A second stage held later has its own count.
	const auto second = waits.Hold(OTHER);
	CHECK(second.begun);
	CHECK(second.played == 0s);
	CHECK(waits.Size() == 2);
	waits.Tick(t0 + 10s + 4min + HostileHold::LOG_EVERY + 30s, false);
	CHECK(waits.Hold(OTHER).played == 30s);
	CHECK(waits.Hold(KEY).played == HostileHold::LOG_EVERY + 40s);

	// Set: the whole wait, and it is no longer held.
	const auto waited = waits.End(KEY);
	CHECK(waited.has_value());
	CHECK(*waited == HostileHold::LOG_EVERY + 40s);
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
	// Fighting counts as near (the caller sets factionNear); the plain combat hold still holds a caught-up one as any stage.
	CHECK(Judge(&hostile, { .live = false, .factionNear = false, .held = true }) == Verdict::kLater);
	// The opening doesn't hold it (it moves nobody), a payment or an unshared story stage is still skipped.
	CHECK(Judge(&hostile, { .live = false, .factionNear = false, .opening = true }) == Verdict::kSet);
	const StageRules::Rule pays{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kPays, 0 };
	CHECK(Judge(&pays, { .live = true, .nearFriend = true }) == Verdict::kSkip);
	// With kMoves too: the hold first, then the move's rules (offered far, set beside the friend).
	const StageRules::Rule both{ 1, 0x004C8E, 70, StageRules::kHostile | StageRules::kMoves, 0 };
	CHECK(Judge(&both, { .live = true, .factionNear = true, .followable = true }) == Verdict::kHostileWaits);
	CHECK(Judge(&both, { .live = true, .factionNear = false, .followable = true }) == Verdict::kOffer);
	CHECK(Judge(&both, { .live = true, .nearFriend = true, .factionNear = true }) == Verdict::kSet);
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
}
