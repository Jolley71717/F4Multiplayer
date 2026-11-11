// Settlement attacks (common/AttackBook.h): the attacks known, the host's game's reports of its runs, a friend's game's
// view of the host's word and its verdicts on its own runs and its mirrors, the notices, and the server's book.

#include "Test.h"

#include "AttackBook.h"

#include <set>

using namespace std::chrono_literals;
using namespace AttackBook;

namespace
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t RAIDERS = 0x001125E2;  // WorkshopRaiderAttack01
	constexpr std::uint32_t GUNNERS = 0x000C97A8;  // WorkshopGunnerAttack01
	constexpr std::uint32_t LOCATION = 0x0001F228;

	Seen Running(std::uint32_t a_quest, std::uint32_t a_workshop, bool a_begun = false, bool a_done = false)
	{
		return { a_quest, true, a_workshop, LOCATION, a_begun, a_done };
	}

	Seen Stopped(std::uint32_t a_quest, bool a_begun = false, bool a_done = false)
	{
		return { a_quest, false, 0, 0, a_begun, a_done };
	}

	Protocol::SettlementAttack HostAttack(std::uint32_t a_quest, std::uint32_t a_workshop, Phase a_phase, Outcome a_outcome = Outcome::kNone)
	{
		return { a_workshop, a_quest, LOCATION, a_phase, a_outcome };
	}
}

TEST("attack book: the attacks are Fallout4.esm's random settlement attacks, each once, found by their ID there")
{
	std::set<std::uint32_t> ids;
	for (const auto& attack : ATTACKS) {
		CHECK(attack.quest != 0 && (attack.quest >> 24) == 0);
		CHECK(attack.editorId.starts_with("Workshop") && attack.editorId.find("Attack") != std::string_view::npos);
		ids.insert(attack.quest);
		const auto found = Find(attack.quest);
		REQUIRE(found);
		CHECK(found->editorId == attack.editorId);
		// Whatever load index the game gives the file, the ID there is the same.
		CHECK(Find(attack.quest | 0x01000000) == found);
	}
	CHECK(ids.size() == ATTACKS.size());
	CHECK(Find(RAIDERS)->editorId == "WorkshopRaiderAttack01");
	// Preston's "Defend X" job is no random attack here (each player's own for now), nor is any other quest.
	CHECK(!Find(0x000A1412));  // MinRadiantOwned01
	CHECK(!Find(0x000289E4));  // Followers
}

TEST("attack book: the host's game reports a run as it is rolled, begins, is defended and ends, each once")
{
	HostRuns runs;
	CHECK(runs.Look({ Stopped(RAIDERS) }).empty());

	auto reports = runs.Look({ Running(RAIDERS, SANCTUARY) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].workshop == SANCTUARY && reports[0].quest == RAIDERS && reports[0].location == LOCATION && reports[0].phase == Phase::kRolled);
	CHECK(runs.Look({ Running(RAIDERS, SANCTUARY) }).empty());

	reports = runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kUnderway);

	reports = runs.Look({ Running(RAIDERS, SANCTUARY, true, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kDefended);
	CHECK(runs.Live().size() == 1);

	reports = runs.Look({ Stopped(RAIDERS, true, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kOver && reports[0].outcome == Outcome::kDefended && reports[0].workshop == SANCTUARY);
	CHECK(runs.Look({ Stopped(RAIDERS, true, true) }).empty());
	CHECK(runs.Size() == 0);
}

TEST("attack book: a run's outcome is by its stages: defended, resolved undefended, or called off before it began")
{
	CHECK(OutcomeOf(true, true) == Outcome::kDefended);
	CHECK(OutcomeOf(false, true) == Outcome::kDefended);
	CHECK(OutcomeOf(true, false) == Outcome::kResolved);
	CHECK(OutcomeOf(false, false) == Outcome::kCalledOff);

	HostRuns runs;
	(void)runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	auto reports = runs.Look({ Stopped(RAIDERS, true, false) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].outcome == Outcome::kResolved);

	(void)runs.Look({ Running(GUNNERS, SANCTUARY) });
	reports = runs.Look({ Stopped(GUNNERS) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].outcome == Outcome::kCalledOff);
}

TEST("attack book: a run seen first mid-attack is reported at its phase; one that moved settlement between looks ends and starts again")
{
	HostRuns runs;
	auto     reports = runs.Look({ Running(RAIDERS, SANCTUARY, true) });
	REQUIRE(reports.size() == 1);
	CHECK(reports[0].phase == Phase::kUnderway);

	reports = runs.Look({ Running(RAIDERS, RED_ROCKET) });
	REQUIRE(reports.size() == 2);
	CHECK(reports[0].workshop == SANCTUARY && reports[0].phase == Phase::kOver && reports[0].outcome == Outcome::kResolved);
	CHECK(reports[1].workshop == RED_ROCKET && reports[1].phase == Phase::kRolled);

	// A run whose workshop alias isn't filled yet isn't reported until it is.
	HostRuns early;
	CHECK(early.Look({ Running(RAIDERS, 0) }).empty());
	CHECK(early.Look({ Running(RAIDERS, SANCTUARY) }).size() == 1);
}

TEST("attack book: a friend's game keeps its own run on a settlement the host doesn't own, and asks when it doesn't know")
{
	CHECK(Judge({ .word = Word::kNotHost }) == Verdict::kKeep);
	CHECK(Judge({ .word = Word::kUnknown }) == Verdict::kAsk);
	CHECK(Judge({ .word = Word::kUnknown, .askedLately = true }) == Verdict::kKeep);  // never stopped on a guess
	CHECK(Judge({ .word = Word::kNotHost, .begun = true, .loadedHere = true }) == Verdict::kKeep);
}

TEST("attack book: a friend's own run at the host's settlement is defused first, wherever their player is, then stopped")
{
	// Not defused yet: stage 100 first, so the quest's 1000 rolls no damage on the friend's save, there or away.
	for (const bool loaded : { false, true }) {
		for (const bool begun : { false, true }) {
			CHECK(Judge({ .word = Word::kHost, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
			CHECK(Judge({ .word = Word::kUnknown, .hostAttackHere = true, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
			CHECK(Judge({ .word = Word::kNotHost, .hostAttackHere = true, .begun = begun, .loadedHere = loaded }) == Verdict::kDefuse);
		}
	}
	// Defused: stopped, unless its own fight is on around the friend's player (stopped once they leave).
	CHECK(Judge({ .word = Word::kHost, .done = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .done = true, .loadedHere = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .begun = true, .done = true }) == Verdict::kStop);
	CHECK(Judge({ .word = Word::kHost, .begun = true, .done = true, .loadedHere = true }) == Verdict::kHold);
}

TEST("attack book: a mirror starts with the host's run, never begins or resolves on its own, and stops defused when the host's run ends")
{
	// Not running here: started, unless the host's run is over, ours runs elsewhere, or a start is under way.
	CHECK(Next({ .hostPhase = Phase::kRolled }) == MirrorStep::kStart);
	CHECK(Next({ .hostPhase = Phase::kUnderway }) == MirrorStep::kStart);  // a joiner's
	CHECK(Next({ .hostPhase = Phase::kOver }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kRolled, .elsewhere = true }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kRolled, .startTried = true }) == MirrorStep::kNone);

	// Running: waits for the host while their run is rolled.
	CHECK(Next({ .hostPhase = Phase::kRolled, .running = true }) == MirrorStep::kNone);
	// Our own timer beat the cancel and began it: defused at once.
	CHECK(Next({ .hostPhase = Phase::kRolled, .running = true, .begun = true }) == MirrorStep::kDefuse);
	// The host's attack is on: "Defend X" shown here only with our player at the settlement.
	CHECK(Next({ .hostPhase = Phase::kUnderway, .running = true }) == MirrorStep::kNone);
	CHECK(Next({ .hostPhase = Phase::kUnderway, .running = true, .loadedHere = true }) == MirrorStep::kShowObjective);
	CHECK(Next({ .hostPhase = Phase::kUnderway, .running = true, .objectiveShown = true, .loadedHere = true }) == MirrorStep::kNone);
	// Defended there: our done stage, which completes the objective.
	CHECK(Next({ .hostPhase = Phase::kDefended, .running = true }) == MirrorStep::kDefuse);
	CHECK(Next({ .hostPhase = Phase::kDefended, .running = true, .done = true }) == MirrorStep::kNone);
	// Over: defused first, then stopped.
	CHECK(Next({ .hostPhase = Phase::kOver, .running = true }) == MirrorStep::kDefuse);
	CHECK(Next({ .hostPhase = Phase::kOver, .running = true, .done = true }) == MirrorStep::kStop);
	CHECK(Next({ .hostPhase = Phase::kOver, .running = true, .begun = true, .done = true, .loadedHere = true }) == MirrorStep::kStop);
}

TEST("attack book: a friend's view keeps the host's word and live attacks, takes their settlements as the host's, and forgets all when the host leaves")
{
	View view;
	const auto now = Clock::now();
	CHECK(view.WordOn(SANCTUARY) == Word::kUnknown);
	view.OnOwner({ RED_ROCKET, false });
	CHECK(view.WordOn(RED_ROCKET) == Word::kNotHost);

	CHECK(!view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(view.WordOn(SANCTUARY) == Word::kHost);
	CHECK(view.LiveAt(SANCTUARY));
	CHECK(view.LiveOf(RAIDERS)->phase == Phase::kRolled);
	CHECK(view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kUnderway), now) == Phase::kRolled);
	CHECK(view.LiveOf(RAIDERS)->phase == Phase::kUnderway);
	CHECK(view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kOver, Outcome::kDefended), now) == Phase::kUnderway);
	CHECK(!view.LiveAt(SANCTUARY));
	CHECK(!view.LiveOf(RAIDERS));
	CHECK(view.WordOn(SANCTUARY) == Word::kHost);

	// The host's game said otherwise of a settlement it attacked: its word stands.
	view.OnOwner({ SANCTUARY, false });
	CHECK(view.WordOn(SANCTUARY) == Word::kNotHost);

	(void)view.OnAttack(HostAttack(GUNNERS, RED_ROCKET, Phase::kUnderway), now);
	view.OnOwner({ 0, false });
	CHECK(view.WordOn(SANCTUARY) == Word::kUnknown);
	CHECK(view.WordOn(RED_ROCKET) == Word::kUnknown);
	CHECK(view.LiveCount() == 0);
}

TEST("attack book: a friend's view forgets a live attack never told over after LIVE_FOR, and keeps no more than its caps")
{
	View       view;
	const auto now = Clock::now();
	(void)view.OnAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now);
	CHECK(view.Expire(now + View::LIVE_FOR - 1s).empty());
	const auto gone = view.Expire(now + View::LIVE_FOR);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0] == RAIDERS);
	CHECK(view.LiveCount() == 0);

	for (std::uint32_t i = 0; i < View::MAX_OWNERS + 10; ++i) {
		view.OnOwner({ 0x00100000 + i, true });
	}
	CHECK(view.Owners() == View::MAX_OWNERS);
	for (std::uint32_t i = 0; i < View::MAX_LIVE + 5; ++i) {
		(void)view.OnAttack(HostAttack(0x00200000 + i, SANCTUARY, Phase::kRolled), now);
	}
	CHECK(view.LiveCount() == View::MAX_LIVE);
}

TEST("attack book: a friend hears of the host's attack as it begins, and its end only if they heard it begin")
{
	CHECK(NoticeFor(Phase::kRolled, Outcome::kNone, false).empty());
	CHECK(NoticeFor(Phase::kUnderway, Outcome::kNone, false) == "attack.under");
	CHECK(NoticeFor(Phase::kDefended, Outcome::kNone, true) == "attack.defended");
	CHECK(NoticeFor(Phase::kOver, Outcome::kResolved, true) == "attack.lost");
	CHECK(NoticeFor(Phase::kOver, Outcome::kDefended, true).empty());  // told when it was defended
	CHECK(NoticeFor(Phase::kOver, Outcome::kCalledOff, true).empty());
	CHECK(NoticeFor(Phase::kOver, Outcome::kResolved, false).empty());
	CHECK(NoticeFor(Phase::kDefended, Outcome::kNone, false).empty());
}

TEST("attack book: the server's book keeps the host's word and live attacks, passes each phase once, and asks the host's game once in a while")
{
	Book       book;
	const auto now = Clock::now();
	CHECK(!book.OwnerOf(SANCTUARY));
	CHECK(book.SetOwner(SANCTUARY, true));
	CHECK(!book.SetOwner(SANCTUARY, true));
	CHECK(book.SetOwner(SANCTUARY, false));
	CHECK(book.OwnerOf(SANCTUARY) == false);

	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(!book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kRolled), now));
	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kUnderway), now));
	CHECK(book.Live().size() == 1);
	CHECK(book.OnHostAttack(HostAttack(RAIDERS, SANCTUARY, Phase::kOver, Outcome::kResolved), now));
	CHECK(book.Live().empty());

	CHECK(book.Ask(RED_ROCKET, now));
	CHECK(!book.Ask(RED_ROCKET, now + Book::ASK_EVERY - 1s));
	CHECK(book.Ask(RED_ROCKET, now + Book::ASK_EVERY));
	CHECK(book.Ask(SANCTUARY, now));

	(void)book.OnHostAttack(HostAttack(GUNNERS, RED_ROCKET, Phase::kRolled), now);
	CHECK(book.Expire(now + Book::LIVE_FOR - 1s) == 0);
	CHECK(book.Expire(now + Book::LIVE_FOR) == 1);

	for (std::uint32_t i = 0; i < Book::MAX_OWNERS + 10; ++i) {
		CHECK(book.SetOwner(0x00100000 + i, true));  // past the cap: told, not kept
	}
	CHECK(book.OwnersKept() == Book::MAX_OWNERS);
	book.Clear();
	CHECK(book.OwnersKept() == 0);
	CHECK(book.Owners().empty());
}
