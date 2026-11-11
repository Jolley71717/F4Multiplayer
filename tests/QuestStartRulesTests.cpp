#include "Test.h"

#include "game/QuestStartRules.h"

#include <array>

// Starting one chosen quest through the engine's own start (game/QuestStartRules.h): when the engine is asked, what its
// answer means, and the check that the engine's function is the one that was read.
namespace
{
	using QuestStartRules::Facts;
	using QuestStartRules::Judge;
	using QuestStartRules::Outcome;
	using QuestStartRules::Verdict;

	// The five bytes of a near call at a_at to a_target.
	std::array<std::uint8_t, 5> CallTo(std::uintptr_t a_at, std::uintptr_t a_target)
	{
		const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(a_at + 5)));
		return { 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
	}

	// 1.11.240, loaded at BASE: EnsureQuestStarted +66B440, FillAliases +66EE80, BeginStartUpQuest +7FF280.
	constexpr std::uintptr_t BASE = 0x7FF700000000;
	constexpr std::uintptr_t ENSURE = BASE + 0x66B440;
	constexpr std::uintptr_t FILL = BASE + 0x66EE80;
	constexpr std::uintptr_t BEGIN = BASE + 0x7FF280;
	constexpr std::uintptr_t FILL_AT = ENSURE + QuestStartRules::FILL_CALL;
	constexpr std::uintptr_t BEGIN_AT = ENSURE + QuestStartRules::BEGIN_CALL;

	static_assert(QuestStartRules::ENSURE_STARTED == 2207742);
	static_assert(QuestStartRules::FILL_ALIASES == 2207786);
	static_assert(QuestStartRules::BEGIN_START_UP == 2214209);
	static_assert(FILL_AT == BASE + 0x66B48F);
	static_assert(BEGIN_AT == BASE + 0x66B4A2);

	bool AsRead(std::span<const std::uint8_t> a_begins, const std::array<std::uint8_t, 5>& a_fillCall, const std::array<std::uint8_t, 5>& a_beginCall)
	{
		return QuestStartRules::AsRead(a_begins, a_fillCall, FILL_AT, FILL, a_beginCall, BEGIN_AT, BEGIN);
	}
}

TEST("quest start: a stopped quest with nothing of an earlier run left is started")
{
	CHECK(Judge(Facts{}) == Verdict::kStart);
	// Flags that say nothing about a run: starts enabled (0x10), failed (0x40), wants dormant (0x4000).
	CHECK(Judge(Facts{ .flags = 0x4050 }) == Verdict::kStart);
	// A quest that runs once and hasn't; one that has run and may run again.
	CHECK(Judge(Facts{ .flags = QuestStartRules::RUN_ONCE }) == Verdict::kStart);
	CHECK(Judge(Facts{ .alreadyRun = true }) == Verdict::kStart);
	CHECK(!QuestStartRules::Waits(Verdict::kStart));
}

TEST("quest start: a quest that runs, or is starting up, is never started")
{
	CHECK(Judge(Facts{ .flags = QuestStartRules::ENABLED }) == Verdict::kRunning);
	// Whatever else is on it: it is the running that counts.
	CHECK(Judge(Facts{ .flags = QuestStartRules::ENABLED | QuestStartRules::STOP_START, .promoting = true, .refsAliased = 4, .locsAliased = 1, .eventData = true, .alreadyRun = true }) ==
		  Verdict::kRunning);
	CHECK(Judge(Facts{ .flags = QuestStartRules::ENABLED | QuestStartRules::RUN_ONCE, .alreadyRun = true }) == Verdict::kRunning);
	// Not a stop to wait out: asking again would find it running again.
	CHECK(!QuestStartRules::Waits(Verdict::kRunning));
}

TEST("quest start: a quest that is stopping is never started, and may be asked again")
{
	// The game's stop-or-start flag; a start's references still being promoted.
	CHECK(Judge(Facts{ .flags = QuestStartRules::STOP_START }) == Verdict::kStopping);
	CHECK(Judge(Facts{ .promoting = true }) == Verdict::kStopping);
	CHECK(Judge(Facts{ .flags = QuestStartRules::STOP_START, .refsAliased = 3 }) == Verdict::kStopping);
	CHECK(QuestStartRules::Waits(Verdict::kStopping));
}

TEST("quest start: a stopped quest that still holds its last run's fills is not started with them")
{
	// `stopquest` only clears the enabled flag; the fills stay until the story teller has shut the run down. The engine's
	// alias fill returns at once, filling nothing, while any of the three is there: the start would run with the old fills.
	CHECK(Judge(Facts{ .refsAliased = 1 }) == Verdict::kFilled);
	CHECK(Judge(Facts{ .locsAliased = 1 }) == Verdict::kFilled);
	CHECK(Judge(Facts{ .eventData = true }) == Verdict::kFilled);
	CHECK(Judge(Facts{ .flags = QuestStartRules::RUN_ONCE, .refsAliased = 2, .alreadyRun = true }) == Verdict::kFilled);
	CHECK(QuestStartRules::Waits(Verdict::kFilled));
}

TEST("quest start: a quest that runs once and has run is not started again")
{
	CHECK(Judge(Facts{ .flags = QuestStartRules::RUN_ONCE, .alreadyRun = true }) == Verdict::kRanOnce);
	// Nothing to wait for: it is never started again.
	CHECK(!QuestStartRules::Waits(Verdict::kRanOnce));
}

TEST("quest start: every verdict and outcome has its own reason for the log")
{
	const std::array verdicts{ Verdict::kStart, Verdict::kRunning, Verdict::kStopping, Verdict::kFilled, Verdict::kRanOnce };
	for (std::size_t i = 0; i < verdicts.size(); ++i) {
		CHECK(!QuestStartRules::Why(verdicts[i]).empty());
		CHECK(QuestStartRules::Why(verdicts[i]) != "unknown");
		for (std::size_t j = i + 1; j < verdicts.size(); ++j) {
			CHECK(QuestStartRules::Why(verdicts[i]) != QuestStartRules::Why(verdicts[j]));
		}
	}
	const std::array outcomes{ Outcome::kStarted, Outcome::kNoFill, Outcome::kNotEnabled };
	for (std::size_t i = 0; i < outcomes.size(); ++i) {
		CHECK(QuestStartRules::Why(outcomes[i]) != "unknown");
		for (std::size_t j = i + 1; j < outcomes.size(); ++j) {
			CHECK(QuestStartRules::Why(outcomes[i]) != QuestStartRules::Why(outcomes[j]));
		}
	}
}

TEST("quest start: the start counts only when the game says so and the quest is enabled after")
{
	using QuestStartRules::After;
	CHECK(After(true, QuestStartRules::ENABLED) == Outcome::kStarted);
	CHECK(After(true, QuestStartRules::ENABLED | 0x4010) == Outcome::kStarted);
	// The game couldn't fill an alias that must fill: it cleared the rest and says no. Whatever the flags then say.
	CHECK(After(false, 0) == Outcome::kNoFill);
	CHECK(After(false, QuestStartRules::ENABLED) == Outcome::kNoFill);
	// It says yes and the quest isn't enabled: not what was read from the game, so not a start.
	CHECK(After(true, 0) == Outcome::kNotEnabled);
	CHECK(After(true, QuestStartRules::STOP_START | QuestStartRules::RUN_ONCE) == Outcome::kNotEnabled);
}

TEST("quest start: the game's function is used only when it is the one that was read")
{
	const auto fill = CallTo(FILL_AT, FILL);
	const auto begin = CallTo(BEGIN_AT, BEGIN);
	// As 1.11.240 has them: `call FillAliases` at +66B48F is E8 EC 39 00 00, `call BeginStartUpQuest` at +66B4A2 is E8 D9 3D 19 00.
	CHECK((fill == std::array<std::uint8_t, 5>{ 0xE8, 0xEC, 0x39, 0x00, 0x00 }));
	CHECK((begin == std::array<std::uint8_t, 5>{ 0xE8, 0xD9, 0x3D, 0x19, 0x00 }));
	CHECK(AsRead(QuestStartRules::BEGINS, fill, begin));

	// Another plugin's hook at its start (a jump over the first five bytes), or another build of the game.
	auto hooked = QuestStartRules::BEGINS;
	hooked[0] = 0xE9;
	CHECK(!AsRead(hooked, fill, begin));
	for (std::size_t i = 0; i < QuestStartRules::BEGINS.size(); ++i) {
		auto other = QuestStartRules::BEGINS;
		other[i] ^= 0x01;
		CHECK(!AsRead(other, fill, begin));
	}
	// Too few bytes read.
	CHECK(!AsRead(std::span{ QuestStartRules::BEGINS }.first(12), fill, begin));
	CHECK(!AsRead(std::span<const std::uint8_t>{}, fill, begin));

	// A call that goes elsewhere (retargeted by a patch), in either place; the two swapped.
	CHECK(!AsRead(QuestStartRules::BEGINS, CallTo(FILL_AT, FILL + 0x10), begin));
	CHECK(!AsRead(QuestStartRules::BEGINS, fill, CallTo(BEGIN_AT, BEGIN - 0x40)));
	CHECK(!AsRead(QuestStartRules::BEGINS, CallTo(FILL_AT, BEGIN), CallTo(BEGIN_AT, FILL)));
	// Not a call at all: a jump with the same displacement, or nops.
	auto jump = fill;
	jump[0] = 0xE9;
	CHECK(!AsRead(QuestStartRules::BEGINS, jump, begin));
	CHECK(!AsRead(QuestStartRules::BEGINS, fill, std::array<std::uint8_t, 5>{ 0x90, 0x90, 0x90, 0x90, 0x90 }));
	// The right bytes read from the wrong place resolve elsewhere.
	CHECK(!QuestStartRules::AsRead(QuestStartRules::BEGINS, fill, FILL_AT + 1, FILL, begin, BEGIN_AT, BEGIN));
}
